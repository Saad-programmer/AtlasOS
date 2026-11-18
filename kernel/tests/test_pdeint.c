/*
 * SAMOS / AtlasOS — kernel/tests/test_pdeint.c
 * ---------------------------------------------------------------------------
 * K0.7-P4f-B2 — Contre-épreuves de l'intégrité du répertoire (défaut D3,
 * audit P4f-A). Mode ISOLÉ « test=pdeint » : ces tests falsifient des
 * descripteurs PMM et forcent, par le prototype de test P4c, des entrées
 * interdites (pour éprouver l'audit). Jamais exécutés par « test », absents
 * de l'image de production.
 *
 * Sûreté sous mutation : toute écriture de PDE acceptée à tort est annulée
 * aussitôt par le prototype ptgate_write_entry (sa propre fenêtre WP, hors
 * pt_gate_write), qui peut effacer ou rétablir une PDE ; l'échec reste une
 * assertion nommée, sans translation corrompue pour la suite.
 */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <samos/frame.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pmm.h>

samos_err_t ptgate_write_entry(uint32_t table_phys, uint32_t index, uint32_t value);

#define K_PDE 0
#define K_PTE 1

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

static frame_desc_t *meta_rw(void)
{
    return (frame_desc_t *)(uintptr_t)pmm_state()->meta;
}

static bool identity_ro(uint32_t frame)
{
    uint32_t pte = entry_pte(frame);
    return (pte & PTE_PRESENT) && (pte & PTE_FRAME) == frame && !(pte & PTE_WRITE);
}

/* État observable : PDE et PTE des régions de test, compteurs. */
struct snap {
    uint32_t pde[5], pte[2], refs[2], free_count, tables, windows;
};
static const uint32_t g_regions[5] = { 0xCE000000u, 0xCF000000u, 0xF8000000u, 0xF4000000u, 0xF0000000u };

static void take(struct snap *s, uint32_t f, uint32_t g)
{
    uint32_t r = 0;
    for (int i = 0; i < 5; i++) {
        s->pde[i] = pdir()[g_regions[i] >> 22] & ~(uint32_t)PTE_ACCESSED;
    }
    s->pte[0] = entry_pte(g_regions[0]) & ~(uint32_t)(PTE_ACCESSED | PTE_DIRTY);
    s->pte[1] = entry_pte(g_regions[1]) & ~(uint32_t)(PTE_ACCESSED | PTE_DIRTY);
    s->refs[0] = pmm_refs(f);
    s->refs[1] = pmm_refs(g);
    s->free_count = pmm_state()->free_count;
    s->tables = paging_table_count();
    paging_gate_stats(&s->windows, &r);
}

static bool same(const struct snap *a, const struct snap *b)
{
    for (int i = 0; i < 5; i++) {
        if (a->pde[i] != b->pde[i]) {
            return false;
        }
    }
    return a->pte[0] == b->pte[0] && a->pte[1] == b->pte[1] && a->refs[0] == b->refs[0] &&
           a->refs[1] == b->refs[1] && a->free_count == b->free_count && a->tables == b->tables &&
           a->windows == b->windows;
}

/* Écriture de PDE par le passage ; si elle est acceptée (mutation), la PDE
 * d'origine est rétablie par le prototype (effacement ou valeur exacte). */
static samos_err_t pde_try(uint32_t va, uint32_t value)
{
    uint32_t before = pdir()[va >> 22];
    samos_err_t e = paging_gate_test_write(K_PDE, va, value);
    if (e == SAMOS_OK) {
        (void)ptgate_write_entry(paging_directory(), va >> 22, before & ~(uint32_t)PTE_ACCESSED);
        for (uint32_t off = 0; off < 0x400000u; off += 0x1000u) {
            cpu_invlpg((const void *)(uintptr_t)(va + off));
        }
    }
    return e;
}

static bool audit_ok(void)
{
    struct paging_audit a;
    return paging_audit(&a) && a.dup_pdes == 0 && a.foreign_aliases == 0 && a.struct_writable == 0;
}

void ktest_suite_pde_integrity(void)
{
    ktest_suite("K0.7 P4f-B2 page-directory integrity (isolated)");
    const struct pmm *p = pmm_state();
    uint32_t ifl = cpu_read_eflags() & EFLAGS_IF;
    const uint32_t A = g_regions[0], B = g_regions[1], R3 = g_regions[2], R4 = g_regions[3], R5 = g_regions[4];
    uint32_t pd = paging_directory();

    /* Préconditions : deux tables publiées (A, B), trois régions sans table. */
    uint32_t f = pmm_alloc_frame(), g = pmm_alloc_frame();
    bool pre = f != 0 && g != 0 && !(pdir()[A >> 22] & PTE_PRESENT) && !(pdir()[B >> 22] & PTE_PRESENT) &&
               !(pdir()[R3 >> 22] & PTE_PRESENT) && !(pdir()[R4 >> 22] & PTE_PRESENT) &&
               !(pdir()[R5 >> 22] & PTE_PRESENT);
    KTEST_CHECK(pre);
    /* Création de tables par le VMM : depuis P4f-B2, le passage refuse de
     * publier une table non scellée ; si table_create cessait de sceller,
     * plus aucune table ne serait créée (refus fermé), ce que nomme ceci. */
    bool vmm_tables_created = pre && vmm_map(A, f, PTE_WRITE) == SAMOS_OK && vmm_map(B, g, PTE_WRITE) == SAMOS_OK;
    KTEST_CHECK(vmm_tables_created);
    pre = pre && vmm_tables_created;
    if (!pre) {
        return;                                 /* jamais de déréférencement sans préconditions */
    }
    *(volatile uint32_t *)(uintptr_t)A = 0xAAAA0001u;
    *(volatile uint32_t *)(uintptr_t)B = 0xBBBB0002u;
    uint32_t TA = pdir()[A >> 22] & PTE_FRAME, TB = pdir()[B >> 22] & PTE_FRAME;
    KTEST_CHECK(audit_ok());

    /* U : table neuve, non publiée, SCELLÉE (identité sans W) et sans alias :
     * la seule cible qu'une substitution pourrait légitimement viser. Ce
     * cadre est consommé par le test (une table n'est jamais libérée). */
    uint32_t U = pmm_alloc_pagetable();
    uint32_t upte = U != 0 ? entry_pte(U) : 0;
    bool useal = U != 0 && (upte & PTE_PRESENT) && (upte & PTE_FRAME) == U &&
                 paging_gate_test_write(K_PTE, U, upte & ~(uint32_t)PTE_WRITE) == SAMOS_OK;
    if (useal) {
        cpu_invlpg((const void *)(uintptr_t)U);
    }
    KTEST_CHECK(useal && identity_ro(U));
    if (!useal) {
        return;
    }

    struct snap s0, s1;
    take(&s0, f, g);

    /* --- 1. Substitution d'une PDE présente --- */
    samos_err_t e1 = pde_try(A, U | PTE_PRESENT | PTE_WRITE);
    bool substitution_refused = e1 == SAMOS_EEXIST;
    KTEST_CHECK(substitution_refused);
    samos_err_t e1b = pde_try(A, TB | PTE_PRESENT | PTE_WRITE);
    KTEST_CHECK(e1b == SAMOS_EEXIST || e1b == SAMOS_EPERM);
    bool identical_write_refused = pde_try(A, TA | PTE_PRESENT | PTE_WRITE) == SAMOS_EEXIST;
    KTEST_CHECK(identical_write_refused);
    bool pde_clear_refused = pde_try(A, 0) != SAMOS_OK;
    KTEST_CHECK(pde_clear_refused);
    uint32_t pa = 0;
    KTEST_CHECK(vmm_translate(A, &pa, 0) && pa == f && *(volatile uint32_t *)(uintptr_t)A == 0xAAAA0001u);
    take(&s1, f, g);
    bool unchanged_after_substitution = same(&s0, &s1);
    KTEST_CHECK(unchanged_after_substitution);

    /* --- 2. Deux PDE vers une même table ; PDE vers le répertoire --- */
    bool duplicate_refused = pde_try(R3, TB | PTE_PRESENT | PTE_WRITE) == SAMOS_EPERM;
    KTEST_CHECK(duplicate_refused);
    bool directory_target_refused = pde_try(R3, pd | PTE_PRESENT | PTE_WRITE) == SAMOS_EPERM;
    KTEST_CHECK(directory_target_refused);
    take(&s1, f, g);
    KTEST_CHECK(same(&s0, &s1));

    /* --- 3. Publication d'une table NON scellée --- */
    uint32_t W = pmm_alloc_pagetable();         /* identité encore W (cadre libre jusque-là) */
    bool wpre = W != 0 && (entry_pte(W) & PTE_WRITE);
    KTEST_CHECK(wpre);
    take(&s0, f, g);
    bool unsealed_table_refused = wpre && pde_try(R3, W | PTE_PRESENT | PTE_WRITE) == SAMOS_EPERM;
    KTEST_CHECK(unsealed_table_refused);
    take(&s1, f, g);
    KTEST_CHECK(same(&s0, &s1));
    /* W, une fois scellée, peut être publiée (transition autorisée) : elle ne
     * reste pas un cadre PAGETABLE non publié (l'audit le compterait). */
    const uint32_t R6 = 0xEC000000u;
    if (wpre && !(pdir()[R6 >> 22] & PTE_PRESENT)) {
        uint32_t wpte = entry_pte(W);
        KTEST_CHECK(paging_gate_test_write(K_PTE, W, wpte & ~(uint32_t)PTE_WRITE) == SAMOS_OK);
        cpu_invlpg((const void *)(uintptr_t)W);
        KTEST_CHECK(identity_ro(W) && paging_gate_test_write(K_PDE, R6, W | PTE_PRESENT | PTE_WRITE) == SAMOS_OK);
    }

    /* --- 4. Publication d'une table qui possède déjà un alias RW --- */
    frame_desc_t usaved = meta_rw()[U >> 12];
    meta_rw()[U >> 12] = frame_desc_make(FRAME_ALLOCATED, 0);   /* falsification : U « projetable » */
    const uint32_t AL = A + 0x5000u;
    bool alias_made = vmm_map(AL, U, PTE_WRITE) == SAMOS_OK;    /* U non publiée : P4f-B1 l'admet */
    meta_rw()[U >> 12] = usaved;                                 /* PAGETABLE, refs 0 */
    KTEST_CHECK(alias_made && (entry_pte(AL) & PTE_WRITE));
    take(&s0, f, g);
    bool aliased_table_refused = alias_made && pde_try(R3, U | PTE_PRESENT | PTE_WRITE) == SAMOS_EPERM;
    KTEST_CHECK(aliased_table_refused);
    take(&s1, f, g);
    KTEST_CHECK(same(&s0, &s1));
    if (alias_made) {
        KTEST_CHECK(vmm_unmap(AL, 0) == SAMOS_OK && entry_pte(AL) == 0);   /* PAGETABLE : sans référence */
    }

    /* --- 5. Substitution après falsification du PMM --- */
    frame_desc_t tsaved = meta_rw()[TA >> 12];
    meta_rw()[TA >> 12] = frame_desc_make(FRAME_ALLOCATED, 0);
    uint32_t h = pmm_alloc_frame();             /* cadre ordinaire, identité W */
    frame_desc_t hsaved = h != 0 ? meta_rw()[h >> 12] : 0;
    if (h != 0) {
        meta_rw()[h >> 12] = frame_desc_make(FRAME_PAGETABLE, 0);   /* « table » selon le PMM */
    }
    take(&s0, f, g);
    bool falsified_substitution_refused = pde_try(A, U | PTE_PRESENT | PTE_WRITE) == SAMOS_EEXIST &&
                                          (h == 0 || pde_try(A, h | PTE_PRESENT | PTE_WRITE) == SAMOS_EEXIST);
    KTEST_CHECK(falsified_substitution_refused);
    bool falsified_target_refused = h != 0 && pde_try(R4, h | PTE_PRESENT | PTE_WRITE) == SAMOS_EPERM;
    KTEST_CHECK(falsified_target_refused);
    samos_err_t ed1 = vmm_map(A + 0x6000u, TA, PTE_WRITE);       /* P4f-B1 : TA reste une structure */
    if (ed1 == SAMOS_OK) {
        (void)vmm_unmap(A + 0x6000u, 0);
    }
    bool d1_holds_after_attempts = ed1 == SAMOS_EPERM;
    KTEST_CHECK(d1_holds_after_attempts);
    take(&s1, f, g);
    KTEST_CHECK(same(&s0, &s1));
    meta_rw()[TA >> 12] = tsaved;
    if (h != 0) {
        meta_rw()[h >> 12] = hsaved;
        KTEST_CHECK(pmm_free_frame(h) == SAMOS_OK);
    }
    KTEST_CHECK(pmm_core_check(p) == 0);

    /* --- 6. Transition AUTORISÉE : absente -> table neuve, scellée, sans alias --- */
    bool fresh_sealed_table_published = paging_gate_test_write(K_PDE, R3, U | PTE_PRESENT | PTE_WRITE) == SAMOS_OK &&
                                        (pdir()[R3 >> 22] & PTE_FRAME) == U;
    KTEST_CHECK(fresh_sealed_table_published);
    uint32_t k = pmm_alloc_frame();
    KTEST_CHECK(k != 0 && vmm_map(R3, k, PTE_WRITE) == SAMOS_OK && entry_pte(R3) != 0 &&
                vmm_unmap(R3, 0) == SAMOS_OK && pmm_free_frame(k) == SAMOS_OK);
    KTEST_CHECK(identity_ro(U) && identity_ro(TA) && identity_ro(TB) && identity_ro(pd));

    /* --- 7. Contre-épreuve de l'audit : PDE en double écrite HORS du passage
     * (prototype P4c) ; l'audit doit la compter, puis la PDE est retirée. --- */
    bool r5_free = !(pdir()[R5 >> 22] & PTE_PRESENT);
    KTEST_CHECK(r5_free);
    if (r5_free) {
        samos_err_t ew = ptgate_write_entry(pd, R5 >> 22, TB | PTE_PRESENT | PTE_WRITE);
        struct paging_audit a;
        bool ok = paging_audit(&a);
        bool audit_sees_duplicate_pde = ew == SAMOS_OK && !ok && a.dup_pdes == 1;
        kprintf("[TEST] D3 audit after a duplicate PDE written outside pt_gate_write: dup_pdes=%u\n", a.dup_pdes);
        (void)ptgate_write_entry(pd, R5 >> 22, 0);
        KTEST_CHECK(audit_sees_duplicate_pde);
    }

    /* État final : audit propre, WP et IF, permissions RO, gardes, PMM. */
    KTEST_CHECK(audit_ok());
    KTEST_CHECK((cpu_read_cr0() & CR0_WP) && (cpu_read_eflags() & EFLAGS_IF) == ifl);
    KTEST_CHECK(identity_ro((uint32_t)(uintptr_t)&kmain & PTE_FRAME) && identity_ro((uint32_t)(uintptr_t)__kro_idt_start));
    KTEST_CHECK(!vmm_translate((uint32_t)(uintptr_t)__stack_top_guard_start, 0, 0) &&
                !vmm_translate((uint32_t)(uintptr_t)__df_guard_start, 0, 0));
    KTEST_CHECK(vmm_unmap(A, 0) == SAMOS_OK && vmm_unmap(B, 0) == SAMOS_OK && pmm_refs(f) == 0 &&
                pmm_refs(g) == 0 && pmm_free_frame(f) == SAMOS_OK && pmm_free_frame(g) == SAMOS_OK);
    KTEST_CHECK(pmm_core_check(p) == 0);
}

/*
 * Mode destructif (QEMU, statut 37) : la table publiée la plus récente est
 * marquée FREE dans le PMM (falsification) ; la création de table suivante
 * reçoit ce cadre et le PMM tente de le remettre à zéro par l'identité, qui
 * est SCELLÉE : #PF (code 3) avant toute publication.
 */
void ktest_pde_steal(void)
{
    struct pmm *p = (struct pmm *)(uintptr_t)pmm_state();
    uint32_t f = pmm_alloc_frame(), g = pmm_alloc_frame();
    if (f == 0 || g == 0 || vmm_map(0xCB000000u, f, PTE_WRITE) != SAMOS_OK) {
        kprintf("[TEST] pdesteal: preconditions not met\n");
        return;
    }
    uint32_t nt = pdir()[0xCB000000u >> 22] & PTE_FRAME;   /* hint du PMM = nt */
    meta_rw()[nt >> 12] = frame_desc_make(FRAME_FREE, 0);
    p->free_count++;
    p->hint = nt >> 12;
    kprintf("[TEST] pdesteal: published table 0x%08x marked FREE; next table allocation\n", nt);
    (void)vmm_map(0xCC000000u, g, PTE_WRITE);
    kprintf("[TEST] pdesteal: active table reused NOT trapped\n");
}
