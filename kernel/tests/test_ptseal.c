/*
 * SAMOS / AtlasOS — kernel/tests/test_ptseal.c
 * ---------------------------------------------------------------------------
 * K0.7-P4e-2 — Autotests du scellement des structures de pagination :
 * répertoire et tables de pages en LECTURE SEULE dans l'identité, écritures
 * légitimes par le passage pt_gate_write (paging.c).
 *
 * Sûreté : toute écriture directe tentée sur une table réécrit la valeur
 * ACTUELLE de l'entrée. Si une mutation laissait la table inscriptible,
 * l'écriture serait acceptée sans rien changer : l'échec reste une
 * assertion, jamais une table détruite.
 */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pmm.h>
#include <samos/string.h>

static const volatile uint32_t *pdir(void)
{
    return (const volatile uint32_t *)(uintptr_t)paging_directory();
}

/* PTE d'identité du cadre `frame` (0 si sa table est absente). */
static uint32_t identity_pte(uint32_t frame)
{
    uint32_t pde = pdir()[frame >> 22];
    if (!(pde & PTE_PRESENT)) {
        return 0;
    }
    return ((const volatile uint32_t *)(uintptr_t)(pde & PTE_FRAME))[(frame >> 12) & 0x3FFu];
}

/* Alias d'identité présent, exact, sans W ni U. */
static bool frame_sealed(uint32_t frame)
{
    uint32_t pte = identity_pte(frame);
    return (pte & PTE_PRESENT) && (pte & PTE_FRAME) == frame && !(pte & PTE_WRITE) && !(pte & PTE_USER);
}

/* Écriture DIRECTE (sonde) de la valeur actuelle : doit fauter, code 3
 * (présente, écriture, superviseur), CR2 exact, contenu inchangé. */
static bool direct_write_faults(uint32_t addr)
{
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)addr;
    uint32_t before = *w, cr2 = 0, err = 0;
    int r = mem_probe_write((void *)(uintptr_t)w, before);
    mem_probe_last_fault(&cr2, &err);
    return r == -1 && cr2 == addr && err == 3u && *w == before;
}

/* Nombre de tables publiées dont l'alias d'identité n'est pas scellé. */
static uint32_t published_tables_writable(void)
{
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 1024u; i++) {
        uint32_t pde = pdir()[i];
        if ((pde & PTE_PRESENT) && !frame_sealed(pde & PTE_FRAME)) {
            bad++;
        }
    }
    return bad;
}

static bool audit_clean(void)
{
    struct paging_audit a;
    bool ok = paging_audit(&a);
    return ok && a.writable == 0 && a.aliases == 0 && a.bad_pdes == 0 && a.unpublished == 0 &&
           a.tables == paging_table_count();
}

void ktest_suite_pt_seal(void)
{
    ktest_suite("K0.7 P4e-2 page-table sealing");
    const struct pmm *p = pmm_state();
    uint32_t ifl = cpu_read_eflags() & EFLAGS_IF;
    uint32_t pd = paging_directory();
    uint32_t t0 = pdir()[pd >> 22] & PTE_FRAME;            /* table de l'alias du répertoire */

    /* --- État scellé au démarrage (paging_init) --- */
    KTEST_CHECK(audit_clean());
    bool pd_ro = frame_sealed(pd);
    KTEST_CHECK(pd_ro);
    bool t0_ro = frame_sealed(t0);
    KTEST_CHECK(t0_ro);
    uint32_t tables_rw = published_tables_writable();
    KTEST_CHECK(tables_rw == 0);
    KTEST_CHECK((cpu_read_cr0() & CR0_WP) != 0);

    /* Écritures directes : répertoire (entrée absente de 0xF4000000), table
     * 0 (entrée de la page nulle, absente), table T0. */
    KTEST_CHECK(!(pdir()[0xF4000000u >> 22] & PTE_PRESENT));
    bool pd_write_faults = direct_write_faults(pd + (0xF4000000u >> 22) * 4u);
    KTEST_CHECK(pd_write_faults);
    uint32_t pt0 = pdir()[0] & PTE_FRAME;
    bool pt0_write_faults = direct_write_faults(pt0);
    KTEST_CHECK(pt0_write_faults);
    KTEST_CHECK(direct_write_faults(t0 + 4092u));

    /* Le passage refuse de rendre W à un cadre de pagination (scellement à
     * sens unique) ; vmm_map refuse tout alias d'un cadre de pagination. */
    uint32_t pd_pte = identity_pte(pd), t0_pte = identity_pte(t0);
    samos_err_t ew = paging_gate_test_write(1, pd, pd_pte | PTE_WRITE);
    if (ew == SAMOS_OK) {
        (void)paging_gate_test_write(1, pd, pd_pte);       /* mutation : rétablir */
        cpu_invlpg((const void *)(uintptr_t)pd);
    }
    bool gate_keeps_pd_ro = ew == SAMOS_EPERM && identity_pte(pd) == pd_pte;
    KTEST_CHECK(gate_keeps_pd_ro);
    samos_err_t et = paging_gate_test_write(1, t0, t0_pte | PTE_WRITE);
    if (et == SAMOS_OK) {
        (void)paging_gate_test_write(1, t0, t0_pte);
        cpu_invlpg((const void *)(uintptr_t)t0);
    }
    KTEST_CHECK(et == SAMOS_EPERM && identity_pte(t0) == t0_pte);
    KTEST_CHECK(vmm_map(0xD00B0000u, pd, 0) == SAMOS_EPERM && vmm_map(0xD00B0000u, t0, PTE_WRITE) == SAMOS_EPERM);
    KTEST_CHECK(!vmm_translate(0xD00B0000u, 0, 0));

    /* --- Cycle de vie d'une table créée APRÈS le scellement --- */
    const uint32_t va = 0xCC000000u;
    KTEST_CHECK(!(pdir()[va >> 22] & PTE_PRESENT));
    uint32_t tables0 = paging_table_count(), free0 = p->free_count;
    uint32_t f = pmm_alloc_frame();
    KTEST_CHECK(f != 0);
    bool mapped = f != 0 && vmm_map(va, f, PTE_WRITE) == SAMOS_OK;        /* 1-5 */
    KTEST_CHECK(mapped && pmm_refs(f) == 1);
    uint32_t pde = pdir()[va >> 22], nt = (pde & PTE_PRESENT) ? (pde & PTE_FRAME) : 0;
    KTEST_CHECK(nt != 0 && paging_table_count() == tables0 + 1u);
    /* 1, 3. Cadre typé PAGETABLE, jamais libérable. */
    KTEST_CHECK(nt != 0 && pmm_core_type(p, nt >> 12) == FRAME_PAGETABLE);
    /* 2. Remise à zéro : seule l'entrée de va est présente. */
    uint32_t nonzero = 0;
    for (uint32_t i = 0; nt != 0 && i < 1024u; i++) {
        nonzero += (i != ((va >> 12) & 0x3FFu) && ((const volatile uint32_t *)(uintptr_t)nt)[i] != 0) ? 1u : 0u;
    }
    KTEST_CHECK(nonzero == 0);
    /* 4-6. Publiée (PDE exactement P | W, + A) ET scellée. */
    KTEST_CHECK((pde & 0xFFFu & ~(uint32_t)PTE_ACCESSED) == (PTE_PRESENT | PTE_WRITE));
    bool new_table_ro = nt != 0 && frame_sealed(nt);
    KTEST_CHECK(new_table_ro);
    /* Écriture directe sur la table neuve : doit fauter. Limite : sous QEMU,
     * tout changement de CR0.WP vide le TLB (cpu_x86_update_cr0), donc la
     * fenêtre du passage masque un INVLPG oublié ; cet INVLPG est vérifié sur
     * le code machine (check_elf.sh, mutation seal_invlpg_removed). */
    bool new_table_write_faults = nt != 0 && direct_write_faults(nt + 4u);
    KTEST_CHECK(new_table_write_faults);
    /* 7. Utilisation : accès réel par va ; bits A/D posés par le MMU dans une
     * table scellée (accès physiques, non soumis aux droits de l'alias). */
    if (mapped) {
        *(volatile uint32_t *)(uintptr_t)va = 0x5EA1ED02u;
    }
    uint32_t pte = nt != 0 ? ((const volatile uint32_t *)(uintptr_t)nt)[(va >> 12) & 0x3FFu] : 0;
    KTEST_CHECK((pte & (PTE_ACCESSED | PTE_DIRTY)) == (PTE_ACCESSED | PTE_DIRTY));
    KTEST_CHECK((pdir()[va >> 22] & PTE_ACCESSED) != 0);
    KTEST_CHECK(mapped && *(volatile uint32_t *)(uintptr_t)f == 0x5EA1ED02u);
    /* Modification d'une PTE par le passage dans la table scellée. */
    uint32_t w0 = 0, r0 = 0, w1 = 0, r1 = 0;
    paging_gate_stats(&w0, &r0);
    KTEST_CHECK(vmm_unmap(va, 0) == SAMOS_OK && pmm_refs(f) == 0 && !vmm_translate(va, 0, 0));
    KTEST_CHECK(f != 0 && vmm_map(va + 0x1000u, f, 0) == SAMOS_OK && pmm_refs(f) == 1);
    paging_gate_stats(&w1, &r1);
    KTEST_CHECK(w1 == w0 + 2u && r1 == r0);
    KTEST_CHECK(new_table_ro && frame_sealed(nt) && (cpu_read_cr0() & CR0_WP) &&
                (cpu_read_eflags() & EFLAGS_IF) == ifl);
    /* 8. Retrait : la table vide reste publiée, scellée, non libérable. */
    KTEST_CHECK(vmm_unmap(va + 0x1000u, 0) == SAMOS_OK && pmm_free_frame(f) == SAMOS_OK);
    KTEST_CHECK(nt != 0 && pmm_free_frame(nt) == SAMOS_EINVAL && frame_sealed(nt));
    KTEST_CHECK(p->free_count == free0 - 1u);              /* le cadre de table, définitivement */

    /* État global inchangé : audit, gardes absentes, PMM cohérent. */
    KTEST_CHECK(audit_clean() && published_tables_writable() == 0);
    KTEST_CHECK(!vmm_translate((uint32_t)(uintptr_t)__stack_top_guard_start, 0, 0) &&
                !vmm_translate((uint32_t)(uintptr_t)__df_guard_start, 0, 0) &&
                !vmm_translate((uint32_t)(uintptr_t)__stack_guard_start, 0, 0));
    KTEST_CHECK(pmm_core_check(p) == 0);
}

/*
 * Modes destructifs (QEMU, statut 37) : écriture DIRECTE, hors sonde, de la
 * valeur actuelle d'une entrée (rien n'est détruit avant la faute).
 *   sealpd    : répertoire actif, entrée de 0xF4000000 (absente) ;
 *   sealpt    : table 0, entrée de la page nulle (absente) ;
 *   sealnewpt : table créée APRÈS le scellement (vmm_map à 0xCD000000),
 *               entrée 1 (absente).
 * Attendu : #PF code 0x3, diagnostic « sealed page table », CR2 = adresse
 * affichée.
 */
void ktest_pt_sealed_write(const char *what)
{
    uint32_t target = 0;
    if (strcmp(what, "sealpd") == 0) {
        target = paging_directory() + (0xF4000000u >> 22) * 4u;
    } else if (strcmp(what, "sealpt") == 0) {
        target = pdir()[0] & PTE_FRAME;
    } else {
        uint32_t f = pmm_alloc_frame();
        if (f == 0 || vmm_map(0xCD000000u, f, PTE_WRITE) != SAMOS_OK) {
            kprintf("[TEST] cannot create a page table after sealing\n");
            return;
        }
        target = (pdir()[0xCD000000u >> 22] & PTE_FRAME) + 4u;
    }
    kprintf("[TEST] writing to sealed page table (%s) at 0x%08x\n", what, target);
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)target;
    *w = *w;
    kprintf("[TEST] sealed page-table write NOT trapped\n");
}
