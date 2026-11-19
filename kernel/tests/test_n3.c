/*
 * SAMOS / AtlasOS — kernel/tests/test_n3.c
 * ---------------------------------------------------------------------------
 * K1.0-N3 — Protection des cadres encore projetés, face à des métadonnées PMM
 * falsifiées (docs/review/K1.0-N3-implementation.md).
 *
 * Mode ISOLÉ « test=n3 » (statut 33) : une assertion nommée par invariant au
 * moins (préfixes n3_i1 à n3_i5), et les cas R1, R1b, R2 (partie non fatale),
 * R3, R4, R5 du rapport de conception. Les falsifications écrivent
 * directement dans g_meta et g_pmm, comme le ferait une écriture parasite du
 * noyau ; chaque cas restaure un état cohérent avant le suivant.
 *
 * Modes DESTRUCTIFS (statut 37), N3-I2 : un cadre projeté, rempli d'un motif,
 * est marqué FREE et désigné au PMM comme prochain cadre :
 *   n3type   allocation de DONNÉES (R2) ;
 *   n3table  allocation d'une TABLE par vmm_map (R6).
 * L'allocateur doit s'arrêter AVANT la remise à zéro : le banc QEMU compare
 * l'empreinte affichée avant l'allocation et celle du diagnostic.
 */
#include <arch/paging.h>
#include <samos/frame.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pmm.h>

static frame_desc_t *meta_rw(void)
{
    return (frame_desc_t *)(uintptr_t)pmm_state()->meta;
}

static struct pmm *pmm_rw(void)
{
    return (struct pmm *)(uintptr_t)pmm_state();
}

static volatile uint32_t *word(uint32_t addr)
{
    return (volatile uint32_t *)(uintptr_t)addr;
}

static frame_desc_t desc(uint32_t phys)
{
    return meta_rw()[phys >> 12];
}

static void forge(uint32_t phys, enum frame_type t, uint32_t refs)
{
    meta_rw()[phys >> 12] = frame_desc_make(t, refs);
}

static void fill(uint32_t va, uint32_t tag)
{
    for (uint32_t i = 0; i < 1024u; i++) {
        word(va)[i] = tag ^ i;
    }
}

static bool holds(uint32_t va, uint32_t tag)
{
    for (uint32_t i = 0; i < 1024u; i++) {
        if (word(va)[i] != (tag ^ i)) {
            return false;
        }
    }
    return true;
}

/* Même empreinte que pmm.c (FNV-1a sur 4 KiB, lecture par l'identité). */
static uint32_t page_checksum(uint32_t phys)
{
    const volatile uint8_t *b = (const volatile uint8_t *)(uintptr_t)phys;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 4096u; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

static bool audit(struct paging_audit *a)
{
    return paging_audit(a);
}

/*
 * K1.0-N3-TEST-CLOSE — Borne de parcours de l'oracle (X9 : dernière PTE d'une
 * table ; X10 : dernière PDE). Un cadre est projeté à `va` (adresse vérifiée
 * libre au préalable), rempli d'un motif, puis son compteur est FALSIFIÉ à 0 :
 * seul l'oracle peut alors refuser la libération et la réattribution. Si
 * l'oracle manque cette PTE (mutation), la libération réussit, le cadre est
 * redistribué (hint forcé sur lui) et remis à zéro : assertions dédiées.
 */
struct n3_bound {
    bool mapped, counted, free_refused, not_reallocated, data_intact, cleanup;
};

static struct n3_bound n3_bound_probe(uint32_t va, uint32_t tag)
{
    struct n3_bound r = { 0 };
    const struct pmm *p = pmm_state();
    uint32_t f = pmm_alloc_frame(), pa = 0, fl = 0;
    if (f == 0 || vmm_translate(va, 0, 0)) {
        return r;                               /* adresse occupée ou mémoire épuisée : tout échoue */
    }
    r.mapped = vmm_map(va, f, PTE_WRITE) == SAMOS_OK && vmm_translate(va, &pa, &fl) && pa == f &&
               (fl & PTE_PRESENT) && pmm_refs(f) == 1;
    if (!r.mapped) {
        return r;
    }
    fill(va, tag);
    r.counted = vmm_frame_dynamically_mapped(f) && vmm_dynamic_mappings(f) == 1;
    forge(f, FRAME_ALLOCATED, 0);               /* refs falsifié : seul l'oracle protège */
    frame_desc_t d = desc(f);
    uint32_t fc = p->free_count;
    r.free_refused = pmm_free_frame(f) == SAMOS_EBUSY && desc(f) == d && p->free_count == fc;
    /* Réattribution : le next-fit est pointé sur f. Cadre non FREE : un autre
     * cadre est rendu ; si f avait été libéré à tort, il serait rendu, remis à
     * zéro. Sous la version saine, aucun arrêt (f n'est jamais candidat). */
    pmm_rw()->hint = f >> 12;
    uint32_t g = pmm_alloc_frame();
    r.not_reallocated = g != 0 && g != f;
    r.data_intact = holds(va, tag);
    if (g != 0 && g != f) {
        (void)pmm_free_frame(g);
    }
    /* Retrait (U2 : refs falsifié, pas de unref), puis libération. Sous une
     * mutation, f peut avoir été redistribué : on le rend dans tous les cas. */
    r.cleanup = vmm_unmap(va, 0) == SAMOS_OK && !vmm_translate(va, 0, 0) && pmm_refs(f) == 0 &&
                pmm_free_frame(f) == SAMOS_OK;
    if (g == f) {
        (void)pmm_free_frame(f);
    }
    return r;
}

void ktest_suite_n3(void)
{
    ktest_suite("K1.0 N3 mapped frames vs PMM metadata (isolated)");
    const struct pmm *p = pmm_state();
    struct paging_audit a;
    const uint32_t VA1 = 0xD1000000u, VA2 = 0xD1001000u, VA3 = 0xD1002000u, VA4 = 0xD1003000u;
    const uint32_t top = paging_idmap_top();

    /* --- N3-I3 : relevé indépendant, identité exclue --- */
    uint32_t f0 = pmm_alloc_frame();
    bool n3_i3_identity_not_counted = f0 != 0 && !vmm_frame_dynamically_mapped(f0) &&
                                      vmm_dynamic_mappings(f0) == 0 &&
                                      !vmm_frame_dynamically_mapped((uint32_t)(uintptr_t)&ktest_suite_n3 & PTE_FRAME);
    KTEST_CHECK(n3_i3_identity_not_counted);
    /* Dernière page de l'identité : dans la table de la frontière quand
     * idmap_top n'est pas multiple de 4 MiB ; projetée en identité, jamais
     * dynamiquement. */
    bool n3_i3_boundary_identity_not_counted = (top & 0x3FFFFFu) == 0 ||
                                               (vmm_translate(top - 4096u, 0, 0) &&
                                                !vmm_frame_dynamically_mapped(top - 4096u));
    KTEST_CHECK(n3_i3_boundary_identity_not_counted);
    /* Première page au-delà de l'identité : même PDE que la fin de l'identité
     * lorsque idmap_top n'est pas multiple de 4 MiB (128 MiB : 0x07fe0000). */
    bool n3_i3_boundary_mapping_counted = vmm_map(top, f0, PTE_WRITE) == SAMOS_OK &&
                                          vmm_frame_dynamically_mapped(f0) && vmm_dynamic_mappings(f0) == 1;
    KTEST_CHECK(n3_i3_boundary_mapping_counted);
    kprintf("[TEST] N3 boundary: idmap_top 0x%08x, same PDE as identity: %d\n", top, (int)((top & 0x3FFFFFu) != 0));
    bool sane_audit = audit(&a) && a.ref_mismatch == 0 && a.mapped_type_mismatch == 0 && a.ref_surplus == 0;
    bool n3_i5_audit_clean_with_mappings = sane_audit;
    KTEST_CHECK(n3_i5_audit_clean_with_mappings);
    frame_desc_t d0 = desc(f0);
    forge(f0, FRAME_FREE, 0);
    bool n3_i3_independent_of_pmm = vmm_frame_dynamically_mapped(f0) && vmm_dynamic_mappings(f0) == 1;
    meta_rw()[f0 >> 12] = d0;
    KTEST_CHECK(n3_i3_independent_of_pmm);
    KTEST_CHECK(vmm_unmap(top, 0) == SAMOS_OK && pmm_refs(f0) == 0 && pmm_free_frame(f0) == SAMOS_OK);

    /* --- R1 : `refs` falsifié (1 -> 0) --- */
    uint32_t x = pmm_alloc_frame();
    KTEST_CHECK(x != 0 && vmm_map(VA1, x, PTE_WRITE) == SAMOS_OK && pmm_refs(x) == 1);
    fill(VA1, 0xA11CE000u);
    forge(x, FRAME_ALLOCATED, 0);
    bool r1_lies = pmm_refs(x) == 0 && pmm_core_check(p) == 0;
    KTEST_CHECK(r1_lies);
    /* N3-I5 : le déficit est visible à l'audit. */
    bool r1_audit_ok = audit(&a);
    bool n3_i5_ref_deficit_seen = !r1_audit_ok && a.ref_mismatch == 1 && a.mapped_type_mismatch == 0;
    KTEST_CHECK(n3_i5_ref_deficit_seen);
    kprintf("[TEST] N3 audit R1: ref_mismatch=%u mapped_type_mismatch=%u\n", a.ref_mismatch, a.mapped_type_mismatch);
    /* N3-I1 : libération refusée, sans effet. */
    frame_desc_t dx = desc(x);
    uint32_t fc = p->free_count, hint = p->hint, mf = p->mapped_frees, bf = p->busy_frees;
    samos_err_t e1 = pmm_free_frame(x);
    bool n3_i1_forged_refs_free_refused = e1 == SAMOS_EBUSY;
    KTEST_CHECK(n3_i1_forged_refs_free_refused);
    bool n3_i1_refusal_without_effect = desc(x) == dx && p->free_count == fc && p->hint == hint &&
                                        p->mapped_frees == mf + 1u && p->busy_frees == bf;
    KTEST_CHECK(n3_i1_refusal_without_effect);
    /* N3-I2 : réutilisation NATURELLE (sans forcer hint) : 70 000 allocations
     * et libérations légitimes en glissement ; avant K1.0, le même cadre
     * revenait au bout de 32 267 (reproduction de conception). */
    uint32_t prev = 0, n = 0;
    bool reused = false;
    for (n = 0; n < 70000u; n++) {
        uint32_t y = pmm_alloc_frame();
        if (prev != 0) {
            (void)pmm_free_frame(prev);
        }
        if (y == 0 || y == x) {
            reused = y == x;
            prev = 0;
            break;
        }
        prev = y;
    }
    if (prev != 0) {
        (void)pmm_free_frame(prev);
    }
    bool n3_i2_never_redistributed = !reused && n == 70000u;
    KTEST_CHECK(n3_i2_never_redistributed);
    bool n3_i2_old_mapping_isolated = holds(VA1, 0xA11CE000u);
    KTEST_CHECK(n3_i2_old_mapping_isolated);
    kprintf("[TEST] N3 R1: free=%d, %u alloc/free cycles, frame reused: %d\n", (int)e1, n, (int)reused);

    /* --- R1b / N3-I4 : la projection incohérente (refs = 0) se retire --- */
    uint32_t an0 = vmm_unmap_ref_anomalies();
    samos_err_t u1 = vmm_unmap(VA1, 0);
    bool n3_i4_inconsistent_mapping_removed = u1 == SAMOS_OK && !vmm_translate(VA1, 0, 0) &&
                                              !vmm_frame_dynamically_mapped(x);
    KTEST_CHECK(n3_i4_inconsistent_mapping_removed);
    bool n3_i4_no_meta_write = desc(x) == dx && vmm_unmap_ref_anomalies() == an0 + 1u;
    KTEST_CHECK(n3_i4_no_meta_write);
    bool n3_i1_free_after_removal = pmm_free_frame(x) == SAMOS_OK && pmm_core_type(p, x >> 12) == FRAME_FREE;
    KTEST_CHECK(n3_i1_free_after_removal);
    KTEST_CHECK(audit(&a) && pmm_core_check(p) == 0);

    /* --- Plusieurs projections, déficit (refs 1 pour 2 projections) --- */
    uint32_t m = pmm_alloc_frame();
    KTEST_CHECK(m != 0 && vmm_map(VA2, m, PTE_WRITE) == SAMOS_OK && vmm_map(VA3, m, 0) == SAMOS_OK &&
                pmm_refs(m) == 2 && vmm_dynamic_mappings(m) == 2);
    forge(m, FRAME_ALLOCATED, 1);
    an0 = vmm_unmap_ref_anomalies();
    samos_err_t u2 = vmm_unmap(VA2, 0);
    /* La projection restante (VA3) garde sa référence : refs reste 1. */
    bool n3_i4_other_ref_kept = u2 == SAMOS_OK && !vmm_translate(VA2, 0, 0) && vmm_translate(VA3, 0, 0) &&
                                pmm_refs(m) == 1 && vmm_unmap_ref_anomalies() == an0 + 1u;
    KTEST_CHECK(n3_i4_other_ref_kept);
    bool n3_i1_free_refused_while_other_mapped = pmm_free_frame(m) == SAMOS_EBUSY && pmm_refs(m) == 1;
    KTEST_CHECK(n3_i1_free_refused_while_other_mapped);
    /* Désormais cohérent (refs 1, 1 projection) : le dernier retrait rend la référence. */
    bool n3_i4_last_unmap_releases = vmm_unmap(VA3, 0) == SAMOS_OK && pmm_refs(m) == 0 &&
                                     vmm_unmap_ref_anomalies() == an0 + 1u && pmm_free_frame(m) == SAMOS_OK;
    KTEST_CHECK(n3_i4_last_unmap_releases);

    /* --- Références prises pour une autre raison (excès légitime) --- */
    uint32_t s = pmm_alloc_frame();
    KTEST_CHECK(s != 0 && vmm_map(VA4, s, PTE_WRITE) == SAMOS_OK && pmm_ref(s) == SAMOS_OK && pmm_refs(s) == 2);
    bool surplus_audit_ok = audit(&a);
    bool n3_i5_surplus_informative = surplus_audit_ok && a.ref_surplus == 1 && a.ref_mismatch == 0;
    KTEST_CHECK(n3_i5_surplus_informative);
    an0 = vmm_unmap_ref_anomalies();
    bool n3_i4_surplus_unmap_releases_one = vmm_unmap(VA4, 0) == SAMOS_OK && pmm_refs(s) == 1 &&
                                            vmm_unmap_ref_anomalies() == an0;
    KTEST_CHECK(n3_i4_surplus_unmap_releases_one);
    KTEST_CHECK(pmm_free_frame(s) == SAMOS_EBUSY && pmm_unref(s) == SAMOS_OK && pmm_free_frame(s) == SAMOS_OK);

    /* --- R2 (partie non fatale) : type falsifié FREE --- */
    uint32_t t2 = pmm_alloc_frame();
    KTEST_CHECK(t2 != 0 && vmm_map(VA1, t2, PTE_WRITE) == SAMOS_OK);
    fill(VA1, 0x5EC12E72u);
    forge(t2, FRAME_FREE, 0);
    pmm_rw()->free_count++;
    bool n3_i1_type_forged_free_refused = pmm_free_frame(t2) == SAMOS_EBUSY;
    KTEST_CHECK(n3_i1_type_forged_free_refused);
    bool r2_audit_ok = audit(&a);
    bool n3_i5_type_free_seen = !r2_audit_ok && a.mapped_type_mismatch == 1;
    KTEST_CHECK(n3_i5_type_free_seen);
    an0 = vmm_unmap_ref_anomalies();
    bool n3_r2_unmap_removes = vmm_unmap(VA1, 0) == SAMOS_OK && !vmm_translate(VA1, 0, 0) &&
                               pmm_core_type(p, t2 >> 12) == FRAME_FREE && vmm_unmap_ref_anomalies() == an0 + 1u;
    KTEST_CHECK(n3_r2_unmap_removes);
    /* Plus aucune projection : le cadre, FREE, redevient distribuable sans
     * risque (l'état falsifié est cohérent avec free_count). */
    KTEST_CHECK(pmm_core_check(p) == 0 && audit(&a));

    /* --- R4 : type KERNEL ; R5 : type PAGETABLE (cadres de données projetés) --- */
    for (int k = 0; k < 2; k++) {
        enum frame_type ft = k == 0 ? FRAME_KERNEL : FRAME_PAGETABLE;
        uint32_t r = pmm_alloc_frame();
        KTEST_CHECK(r != 0 && vmm_map(VA2, r, PTE_WRITE) == SAMOS_OK);
        forge(r, ft, 0);
        bool ok = audit(&a);
        bool type_seen = !ok && a.mapped_type_mismatch == 1;
        an0 = vmm_unmap_ref_anomalies();
        samos_err_t ur = vmm_unmap(VA2, 0);
        bool removed = ur == SAMOS_OK && !vmm_translate(VA2, 0, 0) && pmm_core_type(p, r >> 12) == ft &&
                       vmm_unmap_ref_anomalies() == an0 + 1u;
        kprintf("[TEST] N3 R%d: type %d, audit mapped_type_mismatch=%u, unmap=%d\n", 4 + k, (int)ft,
                a.mapped_type_mismatch, (int)ur);
        if (k == 0) {
            bool n3_i5_r4_type_seen = type_seen;
            KTEST_CHECK(n3_i5_r4_type_seen);
            bool n3_i4_r4_removed = removed;
            KTEST_CHECK(n3_i4_r4_removed);
        } else {
            bool n3_i5_r5_type_seen = type_seen;
            KTEST_CHECK(n3_i5_r5_type_seen);
            bool n3_i4_r5_removed = removed;
            KTEST_CHECK(n3_i4_r5_removed);
        }
        forge(r, FRAME_ALLOCATED, 0);           /* restauration : plus aucune projection */
        KTEST_CHECK(pmm_free_frame(r) == SAMOS_OK);
    }

    /* --- K1.0-N3-TEST-CLOSE : bornes de parcours de l'oracle (X9, X10) --- */
    struct n3_bound last_pte = n3_bound_probe(0xD13FF000u, 0x1A570000u);   /* PDE 0x344, PTE 1023 */
    bool n3_x9_last_pte_mapped = last_pte.mapped;
    KTEST_CHECK(n3_x9_last_pte_mapped);
    bool n3_x9_last_pte_counted = last_pte.counted;
    KTEST_CHECK(n3_x9_last_pte_counted);
    bool n3_x9_last_pte_free_refused = last_pte.free_refused;
    KTEST_CHECK(n3_x9_last_pte_free_refused);
    bool n3_x9_last_pte_not_reallocated = last_pte.not_reallocated;
    KTEST_CHECK(n3_x9_last_pte_not_reallocated);
    bool n3_x9_last_pte_data_intact = last_pte.data_intact;
    KTEST_CHECK(n3_x9_last_pte_data_intact);
    bool n3_x9_last_pte_cleanup = last_pte.cleanup;
    KTEST_CHECK(n3_x9_last_pte_cleanup);
    /* Dernière PDE (1023) : aucune réservation dans ce noyau (pas de projection
     * récursive, aucun MMIO projeté ; les adresses physiques hautes comme
     * 0xFEE00000 ne concernent pas l'espace VIRTUEL). PTE 0 de cette table,
     * pour distinguer X10 de X9 ; pas 0xFFFFF000 (dernière page de l'espace). */
    const uint32_t vlpd = 0xFFC00000u;
    const volatile uint32_t *pd = (const volatile uint32_t *)(uintptr_t)paging_directory();
    bool lpd_free_before = !(pd[vlpd >> 22] & PTE_PRESENT);
    struct n3_bound last_pde = n3_bound_probe(vlpd, 0x1A5D0000u);
    uint32_t lpde = pd[vlpd >> 22], lpt = lpde & PTE_FRAME, lpt_flags = 0, lpt_pa = 0;
    bool lpt_ro = vmm_translate(lpt, &lpt_pa, &lpt_flags) && lpt_pa == lpt && !(lpt_flags & PTE_WRITE);
    bool n3_x10_va_valid = lpd_free_before && vlpd >= top && (vlpd >> 22) == 1023u;
    KTEST_CHECK(n3_x10_va_valid);
    bool n3_x10_last_pde_mapped = last_pde.mapped;
    KTEST_CHECK(n3_x10_last_pde_mapped);
    bool n3_x10_last_pde_counted = last_pde.counted;
    KTEST_CHECK(n3_x10_last_pde_counted);
    bool n3_x10_last_pde_free_refused = last_pde.free_refused;
    KTEST_CHECK(n3_x10_last_pde_free_refused);
    bool n3_x10_last_pde_not_zeroed = last_pde.not_reallocated && last_pde.data_intact;
    KTEST_CHECK(n3_x10_last_pde_not_zeroed);
    /* P4 : PDE conforme (P | W, rien d'autre), table scellée (identité sans W),
     * jamais projetée dynamiquement ; audit sans écart. */
    bool lpd_audit = audit(&a);
    bool n3_x10_p4_protections_kept = (lpde & 0xFFFu & ~(uint32_t)PTE_ACCESSED) == (PTE_PRESENT | PTE_WRITE) &&
                                      lpt_ro && !vmm_frame_dynamically_mapped(lpt) && lpd_audit;
    KTEST_CHECK(n3_x10_p4_protections_kept);
    bool n3_x10_last_pde_cleanup = last_pde.cleanup;
    KTEST_CHECK(n3_x10_last_pde_cleanup);
    kprintf("[TEST] N3 bounds: last PTE va 0x%08x counted=%d free_refused=%d; last PDE va 0x%08x (PDE 0x%08x) "
            "counted=%d free_refused=%d\n", 0xD13FF000u, (int)last_pte.counted, (int)last_pte.free_refused,
            vlpd, lpde, (int)last_pde.counted, (int)last_pde.free_refused);

    /* --- K1.0-N3-TEST-CLOSE (X4) : trois projections, un déficit, UNE anomalie --- */
    const uint32_t VX[3] = { 0xD1010000u, 0xD1011000u, 0xD1012000u };
    uint32_t tr = pmm_alloc_frame();
    bool x4_free_vas = !vmm_translate(VX[0], 0, 0) && !vmm_translate(VX[1], 0, 0) && !vmm_translate(VX[2], 0, 0);
    bool x4_mapped = tr != 0 && x4_free_vas && vmm_map(VX[0], tr, PTE_WRITE) == SAMOS_OK &&
                     vmm_map(VX[1], tr, 0) == SAMOS_OK && vmm_map(VX[2], tr, 0) == SAMOS_OK &&
                     vmm_dynamic_mappings(tr) == 3 && pmm_refs(tr) == 3;
    KTEST_CHECK(x4_mapped);
    fill(VX[0], 0x7B1E0000u);
    bool x4_clean_before = audit(&a) && a.ref_mismatch == 0;   /* 3 projections, 3 références */
    forge(tr, FRAME_ALLOCATED, 1);                             /* déficit : 1 pour 3 */
    bool x4_ok = audit(&a);
    uint32_t x4_rm = a.ref_mismatch, x4_tm = a.mapped_type_mismatch, x4_sp = a.ref_surplus;
    kprintf("[TEST] N3 X4: 3 mappings, refs 1: ref_mismatch=%u mapped_type_mismatch=%u ref_surplus=%u\n",
            x4_rm, x4_tm, x4_sp);
    bool n3_i5_ref_mismatch_per_frame = x4_clean_before && !x4_ok && x4_rm == 1 && x4_tm == 0 && x4_sp == 0;
    KTEST_CHECK(n3_i5_ref_mismatch_per_frame);
    /* Aucun effet destructif : audit en lecture seule, données et descripteur intacts. */
    bool n3_x4_audit_non_destructive = holds(VX[0], 0x7B1E0000u) && pmm_refs(tr) == 1 &&
                                       vmm_dynamic_mappings(tr) == 3;
    KTEST_CHECK(n3_x4_audit_non_destructive);
    an0 = vmm_unmap_ref_anomalies();
    bool x4_cleanup = vmm_unmap(VX[0], 0) == SAMOS_OK && vmm_unmap(VX[1], 0) == SAMOS_OK &&
                      vmm_unmap(VX[2], 0) == SAMOS_OK && pmm_refs(tr) == 0 &&
                      vmm_unmap_ref_anomalies() == an0 + 2u && pmm_free_frame(tr) == SAMOS_OK;
    KTEST_CHECK(x4_cleanup);

    /* --- État final cohérent --- */
    bool final_ok = audit(&a);
    bool n3_audit_clean_at_end = final_ok && a.ref_mismatch == 0 && a.mapped_type_mismatch == 0 &&
                                 a.ref_surplus == 0 && pmm_core_check(p) == 0;
    KTEST_CHECK(n3_audit_clean_at_end);
    kprintf("[TEST] N3 summary: mapped_frees=%u unmap_ref_anomalies=%u\n", p->mapped_frees, vmm_unmap_ref_anomalies());
}

/*
 * Modes destructifs n3type (R2) et n3table (R6). Le cadre x, projeté à
 * 0xD1008000 et rempli d'un motif, est marqué FREE et désigné comme prochain
 * cadre. Le diagnostic attendu vient du PMM (alloc_zeroed), AVANT la remise
 * à zéro ; pour n3table, AVANT le contrôle D1 de table_create.
 */
void ktest_n3_steal(const char *mode)
{
    struct pmm *pw = pmm_rw();
    bool table = mode[2] == 't' && mode[3] == 'a';   /* "n3table" */
    const uint32_t va = 0xD1008000u, nv = 0xCC000000u;
    uint32_t z = pmm_alloc_frame();
    /* La table de la région de va doit exister avant d'allouer x. */
    if (z == 0 || vmm_map(0xD1000000u, z, PTE_WRITE) != SAMOS_OK) {
        kprintf("[TEST] %s: preconditions not met\n", mode);
        return;
    }
    uint32_t d = pmm_alloc_frame();             /* cadre de données pour vmm_map(nv) */
    uint32_t x = pmm_alloc_frame();
    if (d == 0 || x == 0 || vmm_map(va, x, PTE_WRITE) != SAMOS_OK ||
        (((const volatile uint32_t *)(uintptr_t)paging_directory())[nv >> 22] & PTE_PRESENT)) {
        kprintf("[TEST] %s: preconditions not met\n", mode);
        return;
    }
    fill(va, 0xDA7A0000u);
    forge(x, FRAME_FREE, 0);
    pw->free_count++;
    pw->hint = x >> 12;
    bool lies = pmm_core_type(pw, x >> 12) == FRAME_FREE;
    kprintf("[TEST] %s: mapped frame 0x%08x at 0x%08x marked FREE (PMM lies: %d), page checksum 0x%08x, "
            "first word 0x%08x; %s\n", mode, x, va, (int)lies, page_checksum(x), word(x)[0],
            table ? "creating a page table" : "allocating a data frame");
    if (table) {
        samos_err_t e = vmm_map(nv, d, PTE_WRITE);
        kprintf("[TEST] mapped frame handed out NOT trapped (vmm_map returned %d)\n", (int)e);
    } else {
        uint32_t g = pmm_alloc_frame();
        kprintf("[TEST] mapped frame handed out NOT trapped (data frame 0x%08x, first word 0x%08x)\n", g,
                g != 0 ? word(g)[0] : 0u);
    }
}
