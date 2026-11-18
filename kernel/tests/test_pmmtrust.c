/*
 * SAMOS / AtlasOS — kernel/tests/test_pmmtrust.c
 * ---------------------------------------------------------------------------
 * K0.7-P4f-B1 — Contre-épreuves de la frontière de confiance du PMM (défaut
 * D1, audit P4f-A). Mode ISOLÉ « test=pmmtrust » : ces tests FALSIFIENT des
 * descripteurs PMM (écriture directe de meta[], comme le ferait un défaut ou
 * un attaquant) ; ils ne s'exécutent jamais pendant les autotests normaux
 * (« test »), et l'image de production n'en contient rien.
 *
 * Chaque scénario :
 *   1. falsifie le descripteur (et vérifie que la falsification a eu lieu) ;
 *   2. vérifie que le PMM ment (type ALLOCATED, pmm_core_check sans anomalie) ;
 *   3. vérifie que le VMM REFUSE malgré tout (vmm_map RW et RO, passage) ;
 *   4. vérifie l'absence d'effet : PDE/PTE, tables, cadres libres,
 *      références, fenêtres du passage inchangés ;
 *   5. restaure le descripteur, puis vérifie que le cadre reste RO et sans
 *      alias (audit, y compris ses compteurs indépendants du PMM).
 * Si une mutation faisait accepter un alias, il est retiré AVANT la
 * restauration du descripteur : l'échec reste une assertion nommée.
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

/* Accès en écriture aux descripteurs : SEULEMENT pour falsifier (test). */
static frame_desc_t *meta_rw(void)
{
    return (frame_desc_t *)(uintptr_t)pmm_state()->meta;
}

static bool identity_ro(uint32_t frame)
{
    uint32_t pte = entry_pte(frame);
    return (pte & PTE_PRESENT) && (pte & PTE_FRAME) == frame && !(pte & PTE_WRITE);
}

static bool audit_clean(void)
{
    struct paging_audit a;
    return paging_audit(&a) && a.foreign_aliases == 0 && a.struct_writable == 0;
}

/*
 * Scénario D1 sur `frame`, alias tenté à `alias` (région à table existante)
 * et à 0xF4000000 (région SANS table : un refus ne doit créer aucune table).
 * `ro` : le cadre doit rester RO dans l'identité (structures et pages RO de
 * l'image ; faux pour la GDT, RW par conception).
 */
static bool d1_refused(uint32_t frame, uint32_t alias, bool ro, const char *what)
{
    const struct pmm *p = pmm_state();
    const uint32_t notable = 0xF4000000u;
    frame_desc_t saved = meta_rw()[frame >> 12];
    meta_rw()[frame >> 12] = frame_desc_make(FRAME_ALLOCATED, 0);
    bool corrupted = meta_rw()[frame >> 12] != saved;
    bool pmm_lies = pmm_core_type(p, frame >> 12) == FRAME_ALLOCATED && pmm_core_check(p) == 0;

    uint32_t pde_a = pdir()[alias >> 22], pte_a = entry_pte(alias), pde_n = pdir()[notable >> 22];
    uint32_t tables = paging_table_count(), freec = p->free_count, refs = pmm_refs(frame);
    uint32_t w0 = 0, r0 = 0, w1 = 0, r1 = 0;
    paging_gate_stats(&w0, &r0);

    samos_err_t e_rw = vmm_map(alias, frame, PTE_WRITE);
    if (e_rw == SAMOS_OK) {
        (void)vmm_unmap(alias, 0);              /* mutation : retirer l'alias */
    }
    samos_err_t e_ro = vmm_map(alias, frame, 0);
    if (e_ro == SAMOS_OK) {
        (void)vmm_unmap(alias, 0);
    }
    samos_err_t e_nt = vmm_map(notable, frame, PTE_WRITE);
    if (e_nt == SAMOS_OK) {
        (void)vmm_unmap(notable, 0);
    }
    samos_err_t e_gate = paging_gate_test_write(1, alias, frame | PTE_PRESENT | PTE_WRITE);
    if (e_gate == SAMOS_OK) {
        (void)paging_gate_test_write(1, alias, 0);
        cpu_invlpg((const void *)(uintptr_t)alias);
    }
    paging_gate_stats(&w1, &r1);
    bool refused = e_rw == SAMOS_EPERM && e_ro == SAMOS_EPERM && e_nt == SAMOS_EPERM;
    bool gate_refused = e_gate == SAMOS_EPERM;
    bool unchanged = pdir()[alias >> 22] == pde_a && entry_pte(alias) == pte_a &&
                     pdir()[notable >> 22] == pde_n && w1 == w0;
    bool no_table_created = paging_table_count() == tables;
    bool counters = p->free_count == freec && pmm_refs(frame) == refs;

    meta_rw()[frame >> 12] = saved;             /* restauration */
    bool restored = pmm_core_type(p, frame >> 12) == frame_desc_type(saved) && pmm_core_check(p) == 0;
    bool still_ro = !ro || identity_ro(frame);
    bool clean = audit_clean();
    kprintf("[TEST] D1 %s frame=0x%08x meta 0x%04x->0x%04x: vmm_map RW=%d RO=%d no-table=%d gate=%d\n",
            what, frame, saved, frame_desc_make(FRAME_ALLOCATED, 0), (int)e_rw, (int)e_ro, (int)e_nt, (int)e_gate);
    KTEST_CHECK(corrupted && pmm_lies);
    KTEST_CHECK(refused);
    KTEST_CHECK(gate_refused);
    KTEST_CHECK(unchanged);
    KTEST_CHECK(no_table_created);
    KTEST_CHECK(counters);
    KTEST_CHECK(restored && still_ro && clean);
    return corrupted && pmm_lies && refused && gate_refused && unchanged && no_table_created && counters &&
           restored && still_ro && clean;
}

void ktest_suite_pmm_trust(void)
{
    ktest_suite("K0.7 P4f-B1 PMM trust boundary (isolated)");
    const struct pmm *p = pmm_state();
    uint32_t ifl = cpu_read_eflags() & EFLAGS_IF;

    /* Préparation : une table existante pour la région d'alias (0xD0000000)
     * et une table publiée par ce test (0xC9000000). */
    uint32_t g = pmm_alloc_frame(), h = pmm_alloc_frame();
    KTEST_CHECK(g != 0 && h != 0);
    bool prep = g != 0 && h != 0 && vmm_map(0xD0000000u, g, PTE_WRITE) == SAMOS_OK &&
                vmm_map(0xC9000000u, h, PTE_WRITE) == SAMOS_OK;
    KTEST_CHECK(prep);
    if (!prep) {
        return;                                 /* ne jamais falsifier sans préconditions */
    }
    KTEST_CHECK(audit_clean());

    uint32_t pd = paging_directory();
    uint32_t pt0 = pdir()[0] & PTE_FRAME;
    uint32_t pt1 = pdir()[1] & PTE_FRAME;
    uint32_t newt = pdir()[0xC9000000u >> 22] & PTE_FRAME;
    KTEST_CHECK((pdir()[1] & PTE_PRESENT) && pt1 != pt0 && newt != 0);

    /* Structures de pagination (critère : répertoire actif et PDE présents). */
    bool first_table_refused = d1_refused(pt0, 0xD0001000u, true, "first table");
    KTEST_CHECK(first_table_refused);
    bool other_table_refused = d1_refused(pt1, 0xD0002000u, true, "other active table");
    KTEST_CHECK(other_table_refused);
    bool directory_refused = d1_refused(pd, 0xD0003000u, true, "page directory");
    KTEST_CHECK(directory_refused);
    bool new_table_refused = d1_refused(newt, 0xD0004000u, true, "newly published table");
    KTEST_CHECK(new_table_refused);
    /* Image du noyau (critère : [__kernel_start, __kernel_end)). */
    bool text_refused = d1_refused((uint32_t)(uintptr_t)&kmain & PTE_FRAME, 0xD0005000u, true, ".text");
    KTEST_CHECK(text_refused);
    bool idt_refused = d1_refused((uint32_t)(uintptr_t)__kro_idt_start, 0xD0006000u, true, "IDT (P4d)");
    KTEST_CHECK(idt_refused);
    bool gdt_refused = d1_refused((uint32_t)(uintptr_t)__kcpu_start, 0xD0007000u, false, "GDT/TSS");
    KTEST_CHECK(gdt_refused);

    /* Variante V2 : cadre déjà projeté, rendu « libre » par falsification,
     * puis distribué par le PMM comme table neuve. La table ne doit pas être
     * publiée. */
    struct pmm *pw = (struct pmm *)(uintptr_t)p;
    uint32_t y = pmm_alloc_frame();             /* cadre de données (AVANT x) */
    uint32_t x = pmm_alloc_frame();             /* next-fit : hint = x */
    const uint32_t a2 = 0xD0008000u, nv = 0xCA000000u;
    bool v2_prep = y != 0 && x != 0 && vmm_map(a2, x, PTE_WRITE) == SAMOS_OK &&
                   !(pdir()[nv >> 22] & PTE_PRESENT);
    KTEST_CHECK(v2_prep);
    if (v2_prep) {
        uint32_t tables = paging_table_count(), freec = p->free_count;
        meta_rw()[x >> 12] = frame_desc_make(FRAME_FREE, 0);
        pw->free_count++;                       /* compteur RW, falsifié de même */
        samos_err_t ev2 = vmm_map(nv, y, PTE_WRITE);
        bool stolen = pmm_core_type(p, x >> 12) == FRAME_PAGETABLE;    /* le PMM a bien donné x */
        bool v2_refused = ev2 == SAMOS_EPERM && !(pdir()[nv >> 22] & PTE_PRESENT) &&
                          paging_table_count() == tables && pmm_refs(y) == 0;
        kprintf("[TEST] D1 V2 aliased frame x=0x%08x handed out as table: %d, vmm_map=%d\n", x, (int)stolen, (int)ev2);
        KTEST_CHECK(stolen);
        KTEST_CHECK(v2_refused);
        if ((pdir()[nv >> 22] & PTE_PRESENT) && (pdir()[nv >> 22] & PTE_FRAME) == x) {
            /* Mutation : x a été PUBLIÉE comme table. Elle le reste (une table
             * n'est jamais libérée) : retirer la projection et l'alias, sans
             * rendre x au PMM. */
            (void)vmm_unmap(nv, 0);
            (void)vmm_unmap(a2, 0);
            (void)pmm_free_frame(y);
        } else {
            /* Restauration de l'état réel de x : ALLOCATED, projeté une fois. */
            meta_rw()[x >> 12] = frame_desc_make(FRAME_ALLOCATED, 1);
            KTEST_CHECK(p->free_count == freec && pmm_core_check(p) == 0);
            KTEST_CHECK(vmm_unmap(a2, 0) == SAMOS_OK && pmm_free_frame(x) == SAMOS_OK &&
                        pmm_free_frame(y) == SAMOS_OK);
        }
    }
    KTEST_CHECK(audit_clean());
    KTEST_CHECK((cpu_read_cr0() & CR0_WP) && (cpu_read_eflags() & EFLAGS_IF) == ifl);

    /* Nettoyage, puis contre-épreuve de l'audit (en DERNIER : irréversible).
     * Le passage accepte encore un PDE qui désigne une table déjà publiée
     * (défaut D3, non corrigé ici) : la région 0xF8000000 devient un alias
     * de la table 0. L'audit indépendant du PMM doit le signaler. */
    KTEST_CHECK(vmm_unmap(0xD0000000u, 0) == SAMOS_OK && vmm_unmap(0xC9000000u, 0) == SAMOS_OK &&
                pmm_free_frame(g) == SAMOS_OK && pmm_free_frame(h) == SAMOS_OK);
    KTEST_CHECK(pmm_core_check(p) == 0 && audit_clean());
    samos_err_t ed3 = paging_gate_test_write(0, 0xF8000000u, pt0 | PTE_PRESENT | PTE_WRITE);
    struct paging_audit a;
    bool ok = paging_audit(&a);
    bool audit_sees_foreign_alias = ed3 == SAMOS_OK && !ok && a.foreign_aliases > 0;
    kprintf("[TEST] D1 audit after D3-style alias: foreign_aliases=%u aliases=%u\n", a.foreign_aliases, a.aliases);
    KTEST_CHECK(audit_sees_foreign_alias);
}
