/*
 * SAMOS / AtlasOS — kernel/tests/test_ptgate.c
 * ---------------------------------------------------------------------------
 * K0.7-P4c — PROTOTYPE d'un passage d'écriture contrôlé pour les tables de
 * pages, fondé sur une désactivation TRÈS brève de CR0.WP.
 * Conception : docs/design/K0.7-P4c-controlled-writes.md.
 *
 * ISOLEMENT : ce fichier n'est compilé que dans l'IMAGE DE TEST (TEST_SRCS du
 * Makefile). Le VMM ne l'utilise pas ; l'image de production ne contient ni
 * ces fonctions ni aucune écriture de CR0 qui retire WP (vérifié par
 * check_elf.sh --production, symbole ptgate_write_entry interdit).
 *
 * Principe (SDM vol. 3A §4.6) : avec CR0.WP = 1, une écriture superviseur sur
 * une page dont la PTE (ou la PDE) a R/W = 0 provoque une #PF ; avec
 * CR0.WP = 0, elle réussit. Le passage :
 *   1. valide TOUT avant d'ouvrir la fenêtre (cadre de type PAGETABLE, index,
 *      politique de la valeur, absence d'imbrication, WP actif) ;
 *   2. masque les IRQ (irq_save) — cela ne bloque NI les NMI, NI les
 *      exceptions synchrones, NI les #MC ;
 *   3. WP = 0, UNE écriture de 32 bits, WP = 1 ;
 *   4. restaure IF, relit et compare.
 * La fenêtre ne contient qu'une instruction de stockage vers une adresse
 * validée (identité présente) : elle ne peut pas produire de #PF tant que la
 * validation est correcte. Une NMI ou une #MC pendant la fenêtre s'exécuterait
 * avec WP = 0 ; elles mènent à panic (pas de gestionnaire spécifique en K0).
 */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pmm.h>

/* Fenêtre en cours : refuse l'imbrication (appel depuis une exception ou un
 * gestionnaire pendant la fenêtre). */
static volatile uint32_t g_ptgate_active;
/* Injection de faute (TESTS) : abandon APRÈS validation et ouverture de la
 * fenêtre, AVANT l'écriture, pour éprouver le chemin de restauration. */
static volatile bool g_ptgate_inject_abort;
static uint32_t g_ptgate_nested_refusals;

#define PTGATE_ENTRIES 1024u

static bool page_forbidden(uint32_t va)
{
    uint32_t page = va & PTE_FRAME;
    const char *r[3][2] = {
        { __df_guard_start, __df_guard_end },
        { __stack_guard_start, __stack_guard_end },
        { __stack_top_guard_start, __stack_top_guard_end },
    };
    if (page == 0) {
        return true;
    }
    for (unsigned i = 0; i < 3; i++) {
        if (page >= (uint32_t)(uintptr_t)r[i][0] && page < (uint32_t)(uintptr_t)r[i][1]) {
            return true;
        }
    }
    return false;
}

/* Écrit table[index] = value dans une table de pages. Ne fait PAS
 * d'invalidation TLB (l'appelant connaît l'adresse virtuelle concernée). */
samos_err_t ptgate_write_entry(uint32_t table_phys, uint32_t index, uint32_t value);
samos_err_t ptgate_write_entry(uint32_t table_phys, uint32_t index, uint32_t value)
{
    /* --- Validation, fenêtre FERMÉE --- */
    if (g_ptgate_active) {
        g_ptgate_nested_refusals++;
        return SAMOS_EBUSY;                     /* imbrication refusée */
    }
    if ((table_phys & 0xFFFu) != 0 || index >= PTGATE_ENTRIES) {
        return SAMOS_EINVAL;                    /* non alignée, hors limites */
    }
    const struct pmm *p = pmm_state();
    if ((table_phys >> PAGE_SHIFT) >= p->nframes ||
        pmm_core_type(p, table_phys >> PAGE_SHIFT) != FRAME_PAGETABLE) {
        return SAMOS_EPERM;                     /* pas une table de pages */
    }
    uint32_t pa = 0;
    if (!vmm_translate(table_phys, &pa, 0) || pa != table_phys) {
        return SAMOS_EPERM;                     /* la table doit être lisible en identité */
    }
    if (value & PTE_USER) {
        return SAMOS_EPERM;                     /* politique M2.5c : jamais de page utilisateur */
    }
    uint32_t cr0 = cpu_read_cr0();
    if (!(cr0 & CR0_WP)) {
        return SAMOS_EPERM;                     /* protection déjà absente : état anormal */
    }

    /* --- Fenêtre OUVERTE : IRQ masquées, WP = 0 --- */
    uint32_t flags = irq_save();
    g_ptgate_active = 1;
    cpu_write_cr0(cr0 & ~CR0_WP);
    samos_err_t e = SAMOS_OK;
    if (g_ptgate_inject_abort) {
        e = SAMOS_EFAULT;                       /* erreur après validation (test) */
    } else {
        ((volatile uint32_t *)(uintptr_t)table_phys)[index] = value;
    }
    cpu_write_cr0(cr0);                         /* WP rétabli AVANT les IRQ */
    g_ptgate_active = 0;
    irq_restore(flags);
    /* --- Fenêtre FERMÉE --- */

    if (e == SAMOS_OK && ((const volatile uint32_t *)(uintptr_t)table_phys)[index] != value) {
        return SAMOS_EFAULT;                    /* relecture incohérente */
    }
    return e;
}

/* Niveau au-dessus : PTE de l'adresse va (table trouvée dans le répertoire,
 * lu par l'identité), politique des pages interdites, puis INVLPG(va). */
samos_err_t ptgate_set_pte(uint32_t va, uint32_t value);
samos_err_t ptgate_set_pte(uint32_t va, uint32_t value)
{
    if ((va & 0xFFFu) != 0) {
        return SAMOS_EINVAL;
    }
    if ((value & PTE_PRESENT) && page_forbidden(va)) {
        return SAMOS_EPERM;                     /* page nulle, gardes : jamais projetées */
    }
    const volatile uint32_t *pd = (const volatile uint32_t *)(uintptr_t)paging_directory();
    uint32_t pde = pd[va >> 22];
    if (!(pde & PTE_PRESENT)) {
        return SAMOS_ENOENT;                    /* pas de table : le prototype n'en crée pas */
    }
    samos_err_t e = ptgate_write_entry(pde & PTE_FRAME, (va >> 12) & 0x3FFu, value);
    if (e == SAMOS_OK) {
        cpu_invlpg((const void *)(uintptr_t)va);
    }
    return e;
}

/* ---------------------------------------------------------------------------
 * Autotests. Les cibles des tests NÉGATIFS reçoivent leur PROPRE valeur
 * actuelle : même si une mutation faisait accepter une cible interdite, rien
 * ne serait modifié (aucune destruction possible du noyau).
 */
static uint32_t raw_entry(uint32_t table_phys, uint32_t index)
{
    return ((const volatile uint32_t *)(uintptr_t)table_phys)[index];
}

static bool state_intact(uint32_t if_before)
{
    return (cpu_read_cr0() & CR0_WP) && g_ptgate_active == 0 &&
           (cpu_read_eflags() & EFLAGS_IF) == if_before;
}

void ktest_suite_ptgate(void)
{
    ktest_suite("K0.7 P4c page-table write gate (prototype)");
    const struct pmm *p = pmm_state();
    uint32_t ifl = cpu_read_eflags() & EFLAGS_IF;
    const volatile uint32_t *pd = (const volatile uint32_t *)(uintptr_t)paging_directory();
    uint32_t kpage = (uint32_t)(uintptr_t)&kmain & PTE_FRAME;
    uint32_t pt0 = pd[0] & PTE_FRAME;                       /* table de 0-4 MiB */

    /* --- Négatifs : validation AVANT toute ouverture de fenêtre --- */
    uint32_t a = pmm_alloc_frame();
    KTEST_CHECK(a != 0);
    /* cadre KERNEL (.text) */
    KTEST_CHECK(ptgate_write_entry(kpage, 0, raw_entry(kpage, 0)) == SAMOS_EPERM);
    /* cadre ALLOCATED, FREE, RESERVED (VGA), hors mémoire gérée */
    KTEST_CHECK(ptgate_write_entry(a, 0, raw_entry(a, 0)) == SAMOS_EPERM);
    KTEST_CHECK(ptgate_write_entry(0xB8000u, 0, 0) == SAMOS_EPERM);
    KTEST_CHECK(ptgate_write_entry(p->nframes << PAGE_SHIFT, 0, 0) == SAMOS_EPERM);
    KTEST_CHECK(ptgate_write_entry(0xFFFFF000u, 0, 0) == SAMOS_EPERM);
    /* index hors limites, adresse non alignée */
    KTEST_CHECK(ptgate_write_entry(pt0, 1024u, raw_entry(pt0, 0)) == SAMOS_EINVAL);
    KTEST_CHECK(ptgate_write_entry(pt0 + 4u, 0, raw_entry(pt0, 1)) == SAMOS_EINVAL);
    /* valeur interdite : bit U sur la propre PTE de kmain (refus AVANT écriture) */
    uint32_t kpte = raw_entry(pt0, (kpage >> 12) & 0x3FFu);
    KTEST_CHECK(ptgate_write_entry(pt0, (kpage >> 12) & 0x3FFu, kpte | PTE_USER) == SAMOS_EPERM);
    KTEST_CHECK(raw_entry(pt0, (kpage >> 12) & 0x3FFu) == kpte);
    /* pages interdites (garde, page nulle) : jamais rendues présentes */
    KTEST_CHECK(ptgate_set_pte((uint32_t)(uintptr_t)__stack_top_guard_start, a | PTE_PRESENT) == SAMOS_EPERM);
    KTEST_CHECK(ptgate_set_pte((uint32_t)(uintptr_t)__df_guard_start, a | PTE_PRESENT) == SAMOS_EPERM);
    KTEST_CHECK(ptgate_set_pte(0, a | PTE_PRESENT) == SAMOS_EPERM);
    KTEST_CHECK(!vmm_translate((uint32_t)(uintptr_t)__stack_top_guard_start, 0, 0));
    /* adresse sans table */
    KTEST_CHECK(ptgate_set_pte(0xF0000000u, a | PTE_PRESENT) == SAMOS_ENOENT);
    KTEST_CHECK(state_intact(ifl));

    /* Imbrication : simulée en marquant une fenêtre en cours. */
    uint32_t nested0 = g_ptgate_nested_refusals;
    g_ptgate_active = 1;
    samos_err_t en = ptgate_write_entry(pt0, (kpage >> 12) & 0x3FFu, kpte);
    g_ptgate_active = 0;
    KTEST_CHECK(en == SAMOS_EBUSY && g_ptgate_nested_refusals == nested0 + 1u);

    /* --- Scénario positif sur une table SCELLÉE (test seulement) --- */
    const uint32_t base = 0xC4000000u, va = base + 0x1000u;
    uint32_t f = pmm_alloc_frame(), g = pmm_alloc_frame();
    bool pre = f != 0 && g != 0;
    KTEST_CHECK(pre);
    pre = pre && vmm_map(base, f, PTE_WRITE) == SAMOS_OK;  /* crée la table X */
    uint32_t X = pd[base >> 22] & PTE_FRAME;
    pre = pre && (pd[base >> 22] & PTE_PRESENT) && X != 0 && X < 0x400000u &&
          pmm_core_type(p, X >> PAGE_SHIFT) == FRAME_PAGETABLE;
    KTEST_CHECK(pre);
    if (!pre) {
        /* Préconditions absentes (par exemple sous une mutation d'une autre
         * protection) : ne JAMAIS déréférencer une table invalide. L'échec
         * est signalé par l'assertion ci-dessus. */
        return;
    }
    /* X est une table SCELLÉE : depuis K0.7-P4e-2, le VMM retire W de son
     * alias d'identité avant de la publier (en P4c, ce test la scellait
     * lui-même par une écriture directe dans la table 0, désormais RO). */
    uint32_t xpte0 = raw_entry(pt0, (X >> 12) & 0x3FFu);
    KTEST_CHECK((xpte0 & PTE_PRESENT) && !(xpte0 & PTE_WRITE));
    uint32_t cr2 = 0, err = 0, v = 0;
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)(X + 4u), 0) == -1);   /* X est R/O */
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(cr2 == X + 4u && err == 3u);                /* P = 1, W = 1 : protection */

    /* Écriture contrôlée : PTE de va = g, présent, inscriptible. */
    uint32_t want = g | PTE_PRESENT | PTE_WRITE;
    KTEST_CHECK(ptgate_set_pte(va, want) == SAMOS_OK);
    KTEST_CHECK(raw_entry(X, (va >> 12) & 0x3FFu) == want);  /* valeur réellement écrite */
    KTEST_CHECK(state_intact(ifl));                          /* WP et IF restaurés */
    uint32_t pa = 0;
    KTEST_CHECK(vmm_translate(va, &pa, 0) && pa == g);
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)va, 0x9A7E0001u) == 0);  /* TLB : projection effective */
    KTEST_CHECK(*(volatile uint32_t *)(uintptr_t)g == 0x9A7E0001u);
    /* Après fermeture : X toujours R/O pour une écriture directe. */
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)(X + 4u), 0) == -1);

    /* Chemin d'erreur APRÈS validation : rien n'est écrit, tout est restauré. */
    g_ptgate_inject_abort = true;
    samos_err_t ea = ptgate_set_pte(va, 0);
    g_ptgate_inject_abort = false;
    /* Comparaison hors bits A et D : l'écriture par va ci-dessus les a posés
     * (écriture du CPU par adresse PHYSIQUE, possible même si X est R/O en
     * identité ; constaté : 0x...063 au lieu de 0x...003). */
    KTEST_CHECK(ea == SAMOS_EFAULT &&
                (raw_entry(X, (va >> 12) & 0x3FFu) & ~(uint32_t)(PTE_ACCESSED | PTE_DIRTY)) == want);
    KTEST_CHECK((raw_entry(X, (va >> 12) & 0x3FFu) & (PTE_ACCESSED | PTE_DIRTY)) ==
                (PTE_ACCESSED | PTE_DIRTY));                 /* A et D posés par le CPU */
    KTEST_CHECK(state_intact(ifl));

    /* Même scénario avec IF = 1 à l'entrée : IF doit être restauré à 1. */
    uint32_t fl = irq_save();
    cpu_sti();
    KTEST_CHECK(ptgate_set_pte(va, 0) == SAMOS_OK);          /* retrait */
    bool if_kept = (cpu_read_eflags() & EFLAGS_IF) != 0;
    cpu_cli();
    irq_restore(fl);
    KTEST_CHECK(if_kept && (cpu_read_cr0() & CR0_WP));
    KTEST_CHECK(!vmm_translate(va, 0, 0));
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)va, &v) == -1);    /* INVLPG effectif */

    /* Nettoyage : X reste scellée (une table n'est jamais libérée), état
     * PMM/VMM cohérent. */
    KTEST_CHECK(vmm_unmap(base, 0) == SAMOS_OK && pmm_refs(f) == 0);
    KTEST_CHECK(pmm_refs(g) == 0);        /* le passage ne prend pas de référence (limite) */
    KTEST_CHECK(pmm_free_frame(f) == SAMOS_OK && pmm_free_frame(g) == SAMOS_OK &&
                pmm_free_frame(a) == SAMOS_OK);
    KTEST_CHECK(pmm_core_check(p) == 0 && state_intact(ifl));
}
