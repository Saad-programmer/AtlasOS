/*
 * SAMOS / AtlasOS — kernel/tests/test_tlb.c
 * ---------------------------------------------------------------------------
 * K0.7-P4f-B4 — Preuves de cohérence du TLB (défaut D2, audit P4f-A).
 *
 * Sous QEMU (TCG), tout changement de CR0.WP vide le TLB entier
 * (cpu_x86_update_cr0). Or chaque écriture de PTE du VMM passe par la
 * fenêtre WP du passage : un INVLPG oublié y est donc invisible. Deux modes
 * ISOLÉS contournent ce masquage sans toucher à l'image de production :
 *
 *   test=tlbexp  PREUVE COMPORTEMENTALE ISOLÉE : la PTE d'une adresse de test
 *                est modifiée par un ALIAS TEMPORAIRE de sa table, écrit
 *                directement, SANS écriture de CR0. On observe la traduction
 *                périmée sans invalidation, puis la nouvelle après INVLPG,
 *                pour trois transitions (changement de cadre, retrait,
 *                retrait de W). L'alias est créé et retiré par le prototype
 *                de test P4c (sa propre fenêtre WP, avant et après
 *                l'expérience) ; il n'existe que pendant l'expérience.
 *
 *   test=tlbvmm  PREUVE D'INTÉGRATION : le VMM réel (vmm_map, vmm_unmap)
 *                doit laisser une traduction cohérente. Dans l'image de test
 *                normale, ce mode réussit même sans INVLPG (vidage par
 *                CR0.WP). Son pouvoir de détection vient du harnais
 *                tests/tools/tlb_integration.sh, qui le rejoue dans une
 *                COPIE des sources où la fenêtre du passage n'écrit plus
 *                CR0 (aucun vidage implicite).
 *
 * Ces modes ne s'exécutent jamais avec « test », et l'image de production
 * n'en contient rien.
 */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pmm.h>

samos_err_t ptgate_write_entry(uint32_t table_phys, uint32_t index, uint32_t value);

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

static uint32_t windows_now(void)
{
    uint32_t w = 0, r = 0;
    paging_gate_stats(&w, &r);
    return w;
}

/*
 * test=tlbexp — Expérience isolée. va = 0xC8000000 (projection de test),
 * table T ; alias inscriptible de T à 0xC8400000 + 0x1000 (région 0xC8400000,
 * table U). Valeurs distinctes dans les cadres A et B.
 */
void ktest_suite_tlb_experiment(void)
{
    ktest_suite("K0.7 P4f-B4 TLB invalidation experiment (isolated)");
    const uint32_t va = 0xC8000000u, al = 0xC8401000u;
    uint32_t A = pmm_alloc_frame(), B = pmm_alloc_frame(), Z = pmm_alloc_frame();
    bool pre = A != 0 && B != 0 && Z != 0 && !(pdir()[va >> 22] & PTE_PRESENT) &&
               !(pdir()[al >> 22] & PTE_PRESENT) && (va >> 22) != (al >> 22);
    pre = pre && vmm_map(va, A, PTE_WRITE) == SAMOS_OK && vmm_map(al - 0x1000u, Z, 0) == SAMOS_OK;
    KTEST_CHECK(pre);
    if (!pre) {
        return;
    }
    *(volatile uint32_t *)(uintptr_t)A = 0xAAAA0A0Au;
    *(volatile uint32_t *)(uintptr_t)B = 0xBBBB0B0Bu;
    uint32_t T = pdir()[va >> 22] & PTE_FRAME, U = pdir()[al >> 22] & PTE_FRAME;
    uint32_t idx = (va >> 12) & 0x3FFu, ia = (al >> 12) & 0x3FFu;

    /* Alias temporaire inscriptible de T (prototype P4c : une fenêtre WP,
     * donc un vidage du TLB, AVANT l'expérience). */
    bool alias = ptgate_write_entry(U, ia, T | PTE_PRESENT | PTE_WRITE) == SAMOS_OK;
    cpu_invlpg((const void *)(uintptr_t)al);
    KTEST_CHECK(alias);
    if (!alias) {
        return;
    }
    volatile uint32_t *tw = (volatile uint32_t *)(uintptr_t)al;
    uint32_t pte_a = tw[idx];
    KTEST_CHECK((pte_a & PTE_FRAME) == A);

    uint32_t cr0_0 = cpu_read_cr0(), w0 = windows_now(), v = 0;

    /* 1. Changement de cadre : A -> B. */
    KTEST_CHECK(*(volatile uint32_t *)(uintptr_t)va == 0xAAAA0A0Au);     /* TLB réchauffé */
    tw[idx] = B | PTE_PRESENT | PTE_WRITE;                               /* écriture directe, sans CR0 */
    bool stale_after_frame_change = *(volatile uint32_t *)(uintptr_t)va == 0xAAAA0A0Au;
    KTEST_CHECK(stale_after_frame_change);
    cpu_invlpg((const void *)(uintptr_t)va);                       /* étape 1 : invalidation */
    bool new_frame_after_invlpg = *(volatile uint32_t *)(uintptr_t)va == 0xBBBB0B0Bu;
    KTEST_CHECK(new_frame_after_invlpg);

    /* 2. Retrait (présent -> absent). */
    tw[idx] = 0;
    bool stale_after_unmap = mem_probe_read((const void *)(uintptr_t)va, &v) == 0 && v == 0xBBBB0B0Bu;
    KTEST_CHECK(stale_after_unmap);
    cpu_invlpg((const void *)(uintptr_t)va);
    uint32_t cr2 = 0, err = 0;
    bool fault_after_unmap_invlpg = mem_probe_read((const void *)(uintptr_t)va, &v) == -1;
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(fault_after_unmap_invlpg && cr2 == va && err == 0u);

    /* 3. Retrait de W (CR0.WP actif). */
    tw[idx] = B | PTE_PRESENT | PTE_WRITE;
    cpu_invlpg((const void *)(uintptr_t)va);
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)va, 0xBBBB0B0Bu) == 0);   /* TLB : traduction W */
    tw[idx] = B | PTE_PRESENT;
    bool stale_write_after_ro = mem_probe_write((void *)(uintptr_t)va, 0xBBBB0B0Bu) == 0;
    KTEST_CHECK(stale_write_after_ro);
    cpu_invlpg((const void *)(uintptr_t)va);
    bool write_faults_after_invlpg = mem_probe_write((void *)(uintptr_t)va, 0xBBBB0B0Bu) == -1;
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(write_faults_after_invlpg && cr2 == va && err == 3u);

    /* Aucune écriture de CR0 ni fenêtre du passage pendant l'expérience :
     * les observations ne dépendent que d'INVLPG. */
    bool cr0_untouched = cpu_read_cr0() == cr0_0 && windows_now() == w0 && (cr0_0 & CR0_WP);
    KTEST_CHECK(cr0_untouched);

    /* Remise en état : PTE d'origine (A, W), alias retiré, état propre. */
    tw[idx] = pte_a & ~(uint32_t)(PTE_ACCESSED | PTE_DIRTY);
    cpu_invlpg((const void *)(uintptr_t)va);
    KTEST_CHECK(ptgate_write_entry(U, ia, 0) == SAMOS_OK);
    cpu_invlpg((const void *)(uintptr_t)al);
    KTEST_CHECK(entry_pte(al) == 0 && mem_probe_read((const void *)(uintptr_t)al, &v) == -1);
    KTEST_CHECK(*(volatile uint32_t *)(uintptr_t)va == 0xAAAA0A0Au);
    KTEST_CHECK(vmm_unmap(va, 0) == SAMOS_OK && vmm_unmap(al - 0x1000u, 0) == SAMOS_OK &&
                pmm_refs(A) == 0 && pmm_refs(Z) == 0);
    KTEST_CHECK(pmm_free_frame(A) == SAMOS_OK && pmm_free_frame(B) == SAMOS_OK && pmm_free_frame(Z) == SAMOS_OK);
    KTEST_CHECK(audit_ok() && pmm_core_check(pmm_state()) == 0 && (cpu_read_cr0() & CR0_WP));
}

/*
 * test=tlbvmm — Intégration : opérations normales du VMM, chaque accès
 * précédé d'un accès qui charge la traduction dans le TLB.
 */
void ktest_suite_tlb_vmm(void)
{
    ktest_suite("K0.7 P4f-B4 VMM TLB coherence (integration)");
    const uint32_t va = 0xC9000000u;
    uint32_t A = pmm_alloc_frame(), B = pmm_alloc_frame(), v = 0;
    bool pre = A != 0 && B != 0 && !(pdir()[va >> 22] & PTE_PRESENT);
    KTEST_CHECK(pre);
    if (!pre) {
        return;
    }
    *(volatile uint32_t *)(uintptr_t)A = 0xAAAA1111u;
    *(volatile uint32_t *)(uintptr_t)B = 0xBBBB2222u;
    bool mapped = vmm_map(va, A, PTE_WRITE) == SAMOS_OK;
    KTEST_CHECK(mapped && *(volatile uint32_t *)(uintptr_t)va == 0xAAAA1111u);   /* TLB réchauffé */

    /* vmm_unmap : la traduction doit disparaître (sinon : accès à un cadre
     * qui va être rendu au PMM, E5). */
    KTEST_CHECK(vmm_unmap(va, 0) == SAMOS_OK);
    bool unmap_translation_removed = mem_probe_read((const void *)(uintptr_t)va, &v) == -1;
    KTEST_CHECK(unmap_translation_removed);

    /* Nouvelle projection à la même adresse : le nouveau cadre est vu. */
    KTEST_CHECK(*(volatile uint32_t *)(uintptr_t)A == 0xAAAA1111u);
    bool remapped = vmm_map(va, B, PTE_WRITE) == SAMOS_OK;
    bool remap_sees_new_frame = remapped && mem_probe_read((const void *)(uintptr_t)va, &v) == 0 &&
                                v == 0xBBBB2222u;
    KTEST_CHECK(remap_sees_new_frame);
    KTEST_CHECK(vmm_unmap(va, 0) == SAMOS_OK);
    bool second_unmap_removed = mem_probe_read((const void *)(uintptr_t)va, &v) == -1;
    KTEST_CHECK(second_unmap_removed);

    KTEST_CHECK(pmm_free_frame(A) == SAMOS_OK && pmm_free_frame(B) == SAMOS_OK);
    KTEST_CHECK(pmm_core_check(pmm_state()) == 0);
}
