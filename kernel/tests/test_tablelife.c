/*
 * SAMOS / AtlasOS — kernel/tests/test_tablelife.c
 * ---------------------------------------------------------------------------
 * K0.7-P4f-B3 — Cycle de vie et intégrité transactionnelle de la création
 * des tables de pages (défaut D4, audit P4f-A).
 *
 * Mode ISOLÉ « test=tablelife » (statut 33) : erreurs ORDINAIRES (mémoire
 * épuisée, paramètres invalides, projection interdite, région occupée,
 * références saturées). Chacune doit rendre son code EXACT sans aucun effet :
 * ni PDE ni PTE nouvelle, cadres libres, tables et références inchangés. Le
 * mode épuise réellement le PMM : il ne s'exécute jamais avec « test ».
 *
 * Modes DESTRUCTIFS (statut 37 attendu) : violations d'invariant APRÈS
 * l'allocation d'une table (scellement refusé : test=tcseal ; publication du
 * PDE refusée : test=tcpde ; voir aussi test=tablealias et test=pdesteal).
 * Le noyau doit s'arrêter par un diagnostic explicite ; si vmm_map RETOURNE,
 * le mode l'affiche (« internal violation NOT fatal ») et le cas échoue.
 */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <samos/frame.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pmm.h>

static const volatile uint32_t *pdir(void)
{
    return (const volatile uint32_t *)(uintptr_t)paging_directory();
}

static uint32_t entry_pte(uint32_t va)
{
    uint32_t d = pdir()[va >> 22];
    if (!(d & PTE_PRESENT)) {
        return 0;
    }
    return ((const volatile uint32_t *)(uintptr_t)(d & PTE_FRAME))[(va >> 12) & 0x3FFu];
}

static bool audit_ok(void)
{
    struct paging_audit a;
    return paging_audit(&a) && a.dup_pdes == 0 && a.foreign_aliases == 0 && a.struct_writable == 0;
}

/* Effet d'une opération refusée, observé dans une région SANS table. */
struct obs {
    uint32_t pde, free_count, tables, windows, refusals;
};

static void observe(struct obs *o, uint32_t va)
{
    o->pde = pdir()[va >> 22];
    o->free_count = pmm_state()->free_count;
    o->tables = paging_table_count();
    paging_gate_stats(&o->windows, &o->refusals);
}

static bool no_effect(const struct obs *a, uint32_t va)
{
    struct obs b;
    observe(&b, va);
    return a->pde == b.pde && a->free_count == b.free_count && a->tables == b.tables && a->windows == b.windows;
}

void ktest_suite_table_lifecycle(void)
{
    ktest_suite("K0.7 P4f-B3 page-table creation lifecycle (isolated)");
    const struct pmm *p = pmm_state();
    uint32_t ifl = cpu_read_eflags() & EFLAGS_IF;
    const uint32_t R1 = 0xC1000000u, R2 = 0xC2000000u, R3 = 0xC3000000u;
    bool pre = !(pdir()[R1 >> 22] & PTE_PRESENT) && !(pdir()[R2 >> 22] & PTE_PRESENT) &&
               !(pdir()[R3 >> 22] & PTE_PRESENT);
    KTEST_CHECK(pre);
    if (!pre) {
        return;
    }
    uint32_t free_start = p->free_count;
    struct obs o;

    /* --- 1. Mémoire insuffisante (épuisement RÉEL du PMM) --- */
    uint32_t data = pmm_alloc_frame();
    KTEST_CHECK(data != 0);
    uint32_t head = 0, held = 0, f;
    while ((f = pmm_alloc_frame()) != 0) {      /* liste chaînée dans les cadres eux-mêmes */
        *(volatile uint32_t *)(uintptr_t)f = head;
        head = f;
        held++;
    }
    KTEST_CHECK(p->free_count == 0 && held > 0);
    observe(&o, R1);
    samos_err_t eo = vmm_map(R1, data, PTE_WRITE);
    bool oom_enomem_no_effect = eo == SAMOS_ENOMEM && no_effect(&o, R1) && pmm_refs(data) == 0 &&
                                entry_pte(R1) == 0;
    KTEST_CHECK(oom_enomem_no_effect);
    while (head != 0) {
        uint32_t nx = *(volatile uint32_t *)(uintptr_t)head;
        (void)pmm_free_frame(head);
        head = nx;
    }
    bool oom_no_frame_lost = p->free_count == free_start - 1u;   /* seul `data` reste pris */
    KTEST_CHECK(oom_no_frame_lost);
    kprintf("[TEST] D4 OOM: vmm_map=%d with %u frames held; free restored to %u\n", (int)eo, held, p->free_count);

    /* --- 2. Paramètres invalides, projections interdites --- */
    observe(&o, R2);
    bool params_refused = vmm_map(R2 + 4u, data, 0) == SAMOS_EINVAL &&           /* va non alignée */
                          vmm_map(R2, data + 4u, 0) == SAMOS_EINVAL &&           /* pa non alignée */
                          vmm_map(R2, data, PTE_USER) == SAMOS_EINVAL &&         /* droits (M2.5c) */
                          vmm_map(R2, data, 0x010u) == SAMOS_EINVAL;             /* PCD */
    KTEST_CHECK(params_refused && no_effect(&o, R2));
    uint32_t kpage = (uint32_t)(uintptr_t)&kmain & PTE_FRAME;
    bool forbidden_refused = vmm_map(R2, kpage, 0) == SAMOS_EPERM &&                       /* image (E3) */
                             vmm_map(R2, paging_directory(), 0) == SAMOS_EPERM &&          /* répertoire */
                             vmm_map(R2, 0xB8000u, 0) == SAMOS_EPERM &&                    /* réservé */
                             vmm_map((uint32_t)(uintptr_t)__stack_top_guard_start, data, 0) == SAMOS_EPERM && /* garde (E2) */
                             vmm_map(0, data, 0) == SAMOS_EPERM;                           /* page nulle (E1) */
    KTEST_CHECK(forbidden_refused);
    bool ordinary_errors_no_effect = no_effect(&o, R2) && pmm_refs(data) == 0;
    KTEST_CHECK(ordinary_errors_no_effect);

    /* --- 3. Références saturées : ERANGE AVANT toute création de table --- */
    uint32_t r = pmm_alloc_frame();
    KTEST_CHECK(r != 0);
    while (r != 0 && pmm_refs(r) < FRAME_REFS_MAX) {
        (void)pmm_ref(r);
    }
    observe(&o, R3);
    samos_err_t er = r != 0 ? vmm_map(R3, r, 0) : SAMOS_EINVAL;
    bool erange_no_table_created = er == SAMOS_ERANGE && no_effect(&o, R3) && pmm_refs(r) == FRAME_REFS_MAX;
    KTEST_CHECK(erange_no_table_created);
    while (r != 0 && pmm_refs(r) > 0) {
        (void)pmm_unref(r);
    }
    KTEST_CHECK(r != 0 && pmm_free_frame(r) == SAMOS_OK);

    /* --- 4. Cycle de vie complet d'une création RÉUSSIE (R2) ---
     * FREE -> PAGETABLE -> initialisée -> scellée -> publiée. */
    uint32_t tables0 = paging_table_count(), free0 = p->free_count;
    bool created = vmm_map(R2, data, PTE_WRITE) == SAMOS_OK;
    KTEST_CHECK(created);
    uint32_t pde = pdir()[R2 >> 22], t = (pde & PTE_PRESENT) ? (pde & PTE_FRAME) : 0;
    uint32_t idpte = t != 0 ? entry_pte(t) : 0, nonzero = 0;
    for (uint32_t i = 1; t != 0 && i < 1024u; i++) {
        nonzero += ((const volatile uint32_t *)(uintptr_t)t)[i] != 0 ? 1u : 0u;
    }
    bool lifecycle_published_sealed_unique =
        created && t != 0 && pmm_core_type(p, t >> 12) == FRAME_PAGETABLE && nonzero == 0 &&
        (idpte & PTE_PRESENT) && (idpte & PTE_FRAME) == t && !(idpte & PTE_WRITE) &&
        (pde & 0xFFFu & ~(uint32_t)PTE_ACCESSED) == (PTE_PRESENT | PTE_WRITE) &&
        paging_table_count() == tables0 + 1u && p->free_count == free0 - 1u && audit_ok();
    KTEST_CHECK(lifecycle_published_sealed_unique);
    /* Région désormais occupée : EEXIST, sans effet. */
    observe(&o, R2);
    KTEST_CHECK(vmm_map(R2, data, 0) == SAMOS_EEXIST && no_effect(&o, R2) && pmm_refs(data) == 1);
    KTEST_CHECK(vmm_unmap(R2, 0) == SAMOS_OK && pmm_refs(data) == 0 && pmm_free_frame(data) == SAMOS_OK);
    KTEST_CHECK(t != 0 && pmm_free_frame(t) == SAMOS_EINVAL);      /* table : jamais libérée */

    /* État final. */
    KTEST_CHECK(audit_ok() && pmm_core_check(p) == 0);
    KTEST_CHECK((cpu_read_cr0() & CR0_WP) && (cpu_read_eflags() & EFLAGS_IF) == ifl);
    KTEST_CHECK(p->free_count == free_start - 1u);                 /* seule la table publiée en R2 */
}

/* Mode destructif test=tcseal : le scellement de la table neuve est refusé
 * (fenêtre du passage simulée en cours : EBUSY). */
void ktest_table_seal_refused(void)
{
    uint32_t f = pmm_alloc_frame();
    if (f == 0 || (pdir()[0xC6000000u >> 22] & PTE_PRESENT)) {
        kprintf("[TEST] tcseal: preconditions not met\n");
        return;
    }
    kprintf("[TEST] tcseal: gate busy, creating a table at 0xc6000000\n");
    paging_gate_test_set_active(true);
    samos_err_t e = vmm_map(0xC6000000u, f, PTE_WRITE);
    paging_gate_test_set_active(false);
    kprintf("[TEST] internal violation NOT fatal (vmm_map returned %d)\n", (int)e);
}

/* Mode destructif test=tcpde : la publication du PDE est refusée APRÈS le
 * scellement (refus injecté). */
void ktest_table_pde_refused(void)
{
    uint32_t f = pmm_alloc_frame();
    if (f == 0 || (pdir()[0xC7000000u >> 22] & PTE_PRESENT)) {
        kprintf("[TEST] tcpde: preconditions not met\n");
        return;
    }
    kprintf("[TEST] tcpde: PDE refusal injected, creating a table at 0xc7000000\n");
    paging_test_inject_pde_refusal(true);
    samos_err_t e = vmm_map(0xC7000000u, f, PTE_WRITE);
    paging_test_inject_pde_refusal(false);
    kprintf("[TEST] internal violation NOT fatal (vmm_map returned %d)\n", (int)e);
}
