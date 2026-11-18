/*
 * SAMOS / AtlasOS — kernel/tests/test_canary.c
 * ---------------------------------------------------------------------------
 * K0.7-R1b — Autotests des canaris de pile.
 *
 * CE FICHIER EST LE SEUL compilé avec -fstack-protector-strong (Makefile,
 * image de test uniquement) : c'est le moyen de prouver toute la chaîne
 * (prologue, épilogue, __stack_chk_fail, panic) sans instrumenter le
 * démarrage. Clang 18 n'offre pas d'activation fonction par fonction
 * (l'attribut « stack_protect » lui est inconnu).
 *
 * Localisation de la copie du canari, sans supposer de disposition : la
 * fonction instrumentée parcourt SON PROPRE cadre, du début de son tableau
 * local jusqu'à l'EBP sauvegardé (exclu), et compte les mots égaux à
 * __stack_chk_guard. Clang range la copie juste sous l'EBP sauvegardé
 * (-4(%ebp)), au-dessus des tableaux : on doit en trouver exactement une.
 * Zéro signifie « fonction non instrumentée » ; plus d'une, une ambiguïté
 * (dans ce cas, on ne corrompt rien).
 */
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/paging.h>
#include <samos/kernel.h>
#include <samos/layout.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/stack_protector.h>

/*
 * Retourne le nombre de copies du canari trouvées dans le cadre courant.
 * corrupt = true : modifie UNIQUEMENT cette copie (un bit), si elle est
 * unique. Aucune autre donnée n'est écrite : ni l'EBP sauvegardé, ni
 * l'adresse de retour, qui sont AU-DESSUS de la zone parcourue. L'épilogue
 * détecte alors l'écart et appelle __stack_chk_fail : on ne revient pas.
 */
static SAMOS_NOINLINE unsigned canary_frame_probe(bool corrupt)
{
    volatile uint8_t buf[16];                  /* tableau => instrumentée (-strong) */
    for (unsigned i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)i;
    }
    volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)buf & ~(uintptr_t)3u);
    volatile uint32_t *fp = (volatile uint32_t *)__builtin_frame_address(0);
    volatile uint32_t *slot = 0;
    unsigned copies = 0;
    for (; p < fp; p++) {
        if (*p == __stack_chk_guard) {
            copies++;
            slot = p;
        }
    }
    if (corrupt && copies == 1) {
        *slot ^= 1u;                           /* corruption contrôlée et minimale */
    }
    return copies + (buf[15] == 15u ? 0u : 100u);
}

void ktest_canary_smash(void)
{
    kprintf("[TEST] corrupting the local canary copy of canary_frame_probe\n");
    unsigned copies = canary_frame_probe(true);
    /* Atteint seulement si la corruption n'a pas été détectée. */
    kprintf("[TEST] canary corruption NOT detected (copies found: %u)\n", copies);
}

/* Valeur de référence en mémoire, PAS dans une variable locale : la zone
 * parcourue par canary_frame_probe contient les registres EBX/ESI/EDI que
 * l'appelant lui confie ; si l'appelant gardait la valeur du canari dans l'un
 * d'eux, on trouverait deux copies (constaté à la première exécution). */
static volatile uint32_t g_guard_ref;

void ktest_suite_canary(void)
{
    ktest_suite("K0.7 stack canary");
    g_guard_ref = __stack_chk_guard;
    uint32_t ga = (uint32_t)(uintptr_t)&__stack_chk_guard;

    /* Valeur : écrite par _start (différente de celle de l'image), non nulle,
     * octet faible nul. (Probabilité qu'un tirage RDTSC redonne exactement
     * la valeur par défaut : 2^-24, acceptée.) */
    KTEST_CHECK(g_guard_ref != 0);
    KTEST_CHECK((g_guard_ref & 0xFFu) == 0);
    KTEST_CHECK(g_guard_ref != STACK_GUARD_DEFAULT);

    /* Appel instrumenté normal : exactement une copie dans le cadre, retour
     * normal, contenu local intact. Zéro copie = instrumentation absente. */
    KTEST_CHECK(canary_frame_probe(false) == 1);
    KTEST_CHECK(canary_frame_probe(false) == 1);
    KTEST_CHECK(__stack_chk_guard == g_guard_ref);                     /* inchangé */

    /* Emplacement : dans .rodata, page en lecture seule et superviseur. */
    KTEST_CHECK(ga >= (uint32_t)(uintptr_t)__rodata_start &&
                ga + 4u <= (uint32_t)(uintptr_t)__rodata_end);
    uint32_t pa = 0, fl = 0;
    KTEST_CHECK(vmm_translate(ga & PTE_FRAME, &pa, &fl) && pa == (ga & PTE_FRAME));
    KTEST_CHECK((fl & PTE_PRESENT) && !(fl & PTE_WRITE) && !(fl & PTE_USER));
    KTEST_CHECK((cpu_read_cr0() & CR0_WP) != 0);

    /* Écriture refusée par le MMU : faute de protection en écriture à
     * l'adresse exacte du canari, valeur inchangée. */
    uint32_t cr2 = 0, err = 0;
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)ga, g_guard_ref ^ 0xFFFFFF00u) == -1);
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(cr2 == ga && (err & 3u) == 3u);               /* P = 1, W = 1 */
    KTEST_CHECK(__stack_chk_guard == g_guard_ref);
}

/*
 * K0.7-R1d — Sentinelles de la pile de double faute (S2). Tests NON
 * destructifs : S2 n'est utilisée que par la tâche #DF, absente ici ; la
 * seule écriture (test de détection) porte sur un mot sentinelle et est
 * annulée immédiatement. Le scénario destructif est le mode test=dfsentinel.
 */
void ktest_suite_df_sentinel(void)
{
    ktest_suite("K0.7 double-fault stack sentinels");
    volatile uint32_t *w = df_stack_sentinels();
    uintptr_t lo = df_stack_bottom(), hi = df_stack_top();
    const struct tss *df = tss_double_fault();

    /* Disposition : 4 mots au bas exact de S2, S2 alignée et de 4 KiB, ESP
     * initial de la tâche #DF au sommet, TSS #DF entièrement sous la zone. */
    KTEST_CHECK((uintptr_t)w == lo && (lo & 15u) == 0 && hi - lo == 4096u);
    KTEST_CHECK(df->esp == (uint32_t)hi);
    /* La zone sentinelle ne recouvre ni les TSS ni la GDT, où qu'ils soient
     * placés (l'ordre des objets de .bss n'est pas garanti). */
    uintptr_t zlo = lo, zhi = lo + DF_SENTINEL_WORDS * 4u;
    uintptr_t tm = (uintptr_t)tss_main(), gd = (uintptr_t)gdt_table();
    KTEST_CHECK((uintptr_t)df + sizeof(struct tss) <= zlo || (uintptr_t)df >= zhi);
    KTEST_CHECK(tm + sizeof(struct tss) <= zlo || tm >= zhi);
    KTEST_CHECK(gd + GDT_ENTRIES * sizeof(struct gdt_entry) <= zlo || gd >= zhi);
    KTEST_CHECK(lo + DF_SENTINEL_WORDS * 4u < hi);

    /* Initialisation : chaque mot a sa valeur, et elles sont distinctes. */
    bool values_ok = true;
    for (uint32_t i = 0; i < DF_SENTINEL_WORDS; i++) {
        values_ok = values_ok && w[i] == DF_SENTINEL_VALUE(i);
    }
    KTEST_CHECK(values_ok);
    KTEST_CHECK(DF_SENTINEL_VALUE(0) != DF_SENTINEL_VALUE(1) &&
                DF_SENTINEL_VALUE(2) != DF_SENTINEL_VALUE(3));

    /* Intactes après toutes les suites précédentes (exceptions, fautes de
     * page attendues, IRQ, PMM/VMM : tout cela s'exécute sur S1). */
    KTEST_CHECK(df_stack_sentinel_damage() == 0);

    /* Détection : un mot altéré est signalé par son bit, puis restauré. */
    uint32_t saved = w[1];
    w[1] = saved ^ 0x00010000u;
    uint32_t mask = df_stack_sentinel_damage();
    w[1] = saved;
    KTEST_CHECK(mask == (1u << 1));
    KTEST_CHECK(df_stack_sentinel_damage() == 0);

    /* S1 inchangée : page de garde toujours non projetée. */
    KTEST_CHECK(boot_stack_guard_intact());
}
