/*
 * SAMOS / AtlasOS — kernel/tests/ktest.c
 * Moteur des autotests noyau (voir ktest.h).
 *
 * Modes (ligne de commande) :
 *   test          toutes les suites, verdict SUCCESS/FAILURE
 *   test=panic    panique volontaire (vérifie le chemin panic, statut 37)
 *   test=ud|de|gp|np|df|triple   exceptions fatales volontaires (K0.4,
 *                 voir test_faults.c)
 *   test=kbd      ligne tapée au clavier (frappes injectées par QEMU, K0.5)
 */
#include <samos/bootinfo.h>
#include <arch/cpu.h>
#include <arch/paging.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/panic.h>
#include <samos/qemu_dbg.h>
#include <samos/string.h>

static unsigned g_checks;
static unsigned g_failures;
static unsigned g_skipped;                 /* K0.7-SCOPE-RESOLVE : préconditions non réunies */
static const char *g_suite;          /* suite en cours */
static unsigned g_suite_checks0;     /* compteurs au début de la suite */
static unsigned g_suite_fail0;
static unsigned g_suites;

/* Bilan de la suite en cours. Le banc de test exige pour CHAQUE suite une
 * ligne « done: N checks » avec N > 0 : une suite qui ne serait plus appelée,
 * ou qui ne vérifierait plus rien, ne peut donc pas passer inaperçue. */
static void suite_close(void)
{
    if (g_suite != 0) {
        kprintf("[TEST] suite %s done: %u checks, %u failures\n", g_suite,
                g_checks - g_suite_checks0, g_failures - g_suite_fail0);
    }
}

void ktest_suite(const char *name)
{
    suite_close();
    g_suite = name;
    g_suite_checks0 = g_checks;
    g_suite_fail0 = g_failures;
    g_suites++;
    kprintf("[TEST] suite %s\n", name);
}

/*
 * K0.7-P4e-1 — Invariant global vérifié à CHAQUE assertion : hors de la
 * fenêtre du passage (paging.c : pt_gate_write), la pagination active
 * implique CR0.WP = 1. Aucune assertion n'est évaluée dans une fenêtre (aucun
 * appel n'y a lieu). Un défaut qui laisserait WP à 0 est donc signalé par une
 * assertion NOMMÉE, à la première vérification qui suit.
 *
 * Réparation (image de TEST seulement) : WP est rétabli après le signalement.
 * Sans elle, le passage de production refuserait ensuite toute écriture
 * (« WP déjà absent », refus fermé correct) et les KASSERT de get_table ou du
 * scellement arrêteraient le noyau par une panique : le défaut serait alors
 * observé comme une panique (statut 37) et non comme une assertion nommée.
 * Cette écriture de CR0 n'existe pas dans l'image de production
 * (check_elf.sh --production n'y admet que paging_init et pt_gate_write).
 */
static void wp_invariant(const char *expr, const char *file, int line)
{
    uint32_t cr0 = cpu_read_cr0();
    if ((cr0 & CR0_PG) && !(cr0 & CR0_WP)) {
        g_checks++;
        g_failures++;
        kprintf("[FAIL] CR0.WP actif hors fenetre (invariant global) at %s:%d (constate avant '%s')\n",
                file, line, expr);
        cpu_write_cr0(cr0 | CR0_WP);
    }
}

void ktest_check(bool cond, const char *expr, const char *file, int line)
{
    g_checks++;
    wp_invariant(expr, file, line);
    if (cond) {
        kprintf("[PASS] %s\n", expr);
        return;
    }
    g_failures++;
    kprintf("[FAIL] %s at %s:%d\n", expr, file, line);
}

unsigned ktest_failures(void)
{
    return g_failures;
}

void ktest_skip(const char *name, const char *reason)
{
    g_skipped++;
    kprintf("[SKIP] %s: %s\n", name, reason);
}

unsigned ktest_skipped(void)
{
    return g_skipped;
}

/* Fonctions imbriquées non inlinées : la trace de pile doit montrer plusieurs
 * cadres. */
static __attribute__((noinline)) SAMOS_NORETURN void deliberate_panic_inner(int code)
{
    panic("deliberate test panic (%d)", code);
}

static __attribute__((noinline)) SAMOS_NORETURN void deliberate_panic(void)
{
    deliberate_panic_inner(42);
}

/*
 * K0.7-P4d — Scellement dans l'image de test. Les modes qui écrivent
 * VOLONTAIREMENT dans l'IDT ou les gestionnaires pour provoquer leur faute
 * (df et dfsentinel : idt_set_present ; nested : interrupt_register)
 * s'exécutent sans scellement. Tous les autres modes, y compris stack
 * (double faute par débordement de S1) et kbd (IRQ clavier), s'exécutent
 * APRÈS le scellement de production : ils prouvent que le noyau scellé
 * fonctionne. Les suites, dont certaines enregistrent des gestionnaires,
 * s'exécutent avant le scellement ; la suite P4d vérifie ensuite l'état
 * scellé.
 */
static bool mode_writes_tables(const char *mode)
{
    return strcmp(mode, "df") == 0 || strcmp(mode, "dfsentinel") == 0 ||
           strcmp(mode, "nested") == 0;
}

static void seal_or_fail(void)
{
    samos_err_t e = paging_seal_critical();
    if (e != SAMOS_OK) {
        kprintf("[TEST] critical structure sealing failed (%d)\n", (int)e);
        qemu_exit(QEMU_EXIT_FAILURE);
    }
    kprintf("[TEST] critical structures sealed\n");
}

/* Bilan final et sortie QEMU (33 si aucun échec, 35 sinon). */
static SAMOS_NORETURN void finish(void)
{
    suite_close();
    kprintf("[TEST] suites=%u checks=%u failures=%u\n", g_suites, g_checks, g_failures);
    /* Ligne séparée : le format de la ligne précédente est lu tel quel par le
     * moteur de mutations et par le harnais TLB. */
    kprintf("[TEST] skipped=%u\n", g_skipped);
    if (g_failures == 0) {
        kprintf("[TEST] ALL TESTS PASSED\n");
        qemu_exit(QEMU_EXIT_SUCCESS);
    }
    kprintf("[TEST] SOME TESTS FAILED\n");
    qemu_exit(QEMU_EXIT_FAILURE);
}

void ktest_run_all(void)
{
    char mode[32];
    if (cmdline_value("test", mode, sizeof(mode))) {
        if (!mode_writes_tables(mode)) {
            seal_or_fail();
        }
        if (strcmp(mode, "panic") == 0) {
            deliberate_panic();
        }
        if (strcmp(mode, "kbd") == 0) {
            ktest_keyboard_line();
            qemu_exit(QEMU_EXIT_SUCCESS);   /* le verdict porte sur la ligne affichée */
        }
        /* K0.7-P4f-B1 — Mode ISOLÉ : falsifie des descripteurs PMM (D1). Jamais
         * exécuté par le mode « test » normal. */
        if (strcmp(mode, "pmmtrust") == 0) {
            ktest_suite_pmm_trust();
            finish();
        }
        /* K0.7-P4f-B2 — Mode ISOLÉ : intégrité du répertoire (D3). */
        if (strcmp(mode, "pdeint") == 0) {
            ktest_suite_pde_integrity();
            finish();
        }
        /* K0.7-P4f-B3 — Mode ISOLÉ : erreurs ordinaires de la création des
         * tables (épuise réellement le PMM). */
        if (strcmp(mode, "tablelife") == 0) {
            ktest_suite_table_lifecycle();
            finish();
        }
        /* K0.7-P4f-B4 — Modes ISOLÉS : preuves de cohérence du TLB (D2). */
        if (strcmp(mode, "tlbexp") == 0) {
            ktest_suite_tlb_experiment();
            finish();
        }
        if (strcmp(mode, "tlbvmm") == 0) {
            ktest_suite_tlb_vmm();
            finish();
        }
        /* K0.7-P4f-D — Mode ISOLÉ : intégrité de l'image du noyau (N2, N5). */
        /* K1.0-N3 — Mode ISOLÉ : cadres encore projetés (N3-I1 à I5). */
        if (strcmp(mode, "n3") == 0) {
            ktest_suite_n3();
            finish();
        }
        if (strcmp(mode, "imageint") == 0) {
            ktest_suite_image_integrity();
            finish();
        }
        if (ktest_fatal_mode(mode)) {
            kprintf("[TEST] fatal mode '%s' returned: fault not raised\n", mode);
            qemu_exit(QEMU_EXIT_FAILURE);
        }
        kprintf("[TEST] unknown test mode '%s'\n", mode);
        qemu_exit(QEMU_EXIT_FAILURE);
    }

    ktest_suite_boot();
    ktest_suite_layout();
    ktest_suite_console();
    ktest_suite_cpu();
    ktest_suite_irq();
    ktest_suite_mm();
    ktest_suite_canary();
    ktest_suite_df_sentinel();
    ktest_suite_stack_layout();
    ktest_suite_top_guard();
    ktest_suite_df_guard();
    ktest_suite_s2_init();
    ktest_suite_critical_layout();
    ktest_suite_ptgate();

    /* K0.7-P4d — Dernière écriture légitime des tables scellées passée :
     * scellement de production, puis vérification de l'état scellé.
     * K0.7-P4e-1 : un échec du scellement est une ASSERTION (et non une sortie
     * immédiate) : les suites suivantes s'exécutent et nomment le défaut
     * (par exemple CR0.WP laissé à 0, qui fait refuser le passage). Les modes
     * destructifs gardent seal_or_fail : ils n'ont de sens que scellés. */
    samos_err_t seal_err = paging_seal_critical();
    KTEST_CHECK(seal_err == SAMOS_OK);
    if (seal_err == SAMOS_OK) {
        kprintf("[TEST] critical structures sealed\n");
    }
    ktest_suite_sealed();
    ktest_suite_vmm_gate();
    ktest_suite_pt_seal();

    finish();
}
