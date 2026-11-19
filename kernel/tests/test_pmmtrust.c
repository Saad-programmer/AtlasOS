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

/* Prototype P4c (test_ptgate.c, image de test) : écriture d'une entrée par
 * sa propre fenêtre WP, SANS les contrôles de pt_gate_write. */
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
    samos_err_t e_gate = paging_gate_test_write(1, alias, frame | PTE_PRESENT | PTE_WRITE);
    if (e_gate == SAMOS_OK) {
        (void)paging_gate_test_write(1, alias, 0);
        cpu_invlpg((const void *)(uintptr_t)alias);
    }
    /* Signalés AVANT la sonde « région sans table » : depuis K0.7-P4f-B3, si
     * un défaut laissait cette requête atteindre la création d'une table
     * sous un PMM falsifié, le noyau s'arrêterait (refus fatal) ; ces
     * assertions nommées sont alors déjà dans le journal. */
    bool refused_existing_region = e_rw == SAMOS_EPERM && e_ro == SAMOS_EPERM;
    KTEST_CHECK(refused_existing_region);
    bool gate_refused = e_gate == SAMOS_EPERM;
    KTEST_CHECK(gate_refused);
    samos_err_t e_nt = vmm_map(notable, frame, PTE_WRITE);
    if (e_nt == SAMOS_OK) {
        (void)vmm_unmap(notable, 0);
    }
    paging_gate_stats(&w1, &r1);
    bool refused = refused_existing_region && e_nt == SAMOS_EPERM;
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

    /* Variante V2 (cadre déjà projeté redistribué comme table) : depuis
     * K0.7-P4f-B3, le refus est FATAL (le cadre volé ne peut être ni publié
     * ni rendu) ; elle est éprouvée par le mode destructif test=tablealias
     * (ktest_table_alias ci-dessous). */
    KTEST_CHECK(audit_clean());
    KTEST_CHECK((cpu_read_cr0() & CR0_WP) && (cpu_read_eflags() & EFLAGS_IF) == ifl);

    /* Nettoyage, puis contre-épreuve de l'audit.
     * K0.7-P4f-B2 : jusqu'à P4f-B1, l'alias était obtenu par le passage
     * lui-même (une PDE vers une table déjà publiée, défaut D3) ; le passage
     * le refuse désormais. L'alias interdit est maintenant écrit PAR UN AUTRE
     * CHEMIN, le prototype de test P4c (ptgate_write_entry : sa propre fenêtre
     * WP, sans les contrôles de pt_gate_write) : une PTE de la table de
     * 0xD0000000 désigne la table 0. L'audit, qui n'utilise pas le PMM, doit
     * le compter ; l'alias est ensuite retiré (réversible, contrairement à
     * l'ancienne version) et l'audit redevient propre. */
    KTEST_CHECK(vmm_unmap(0xD0000000u, 0) == SAMOS_OK && vmm_unmap(0xC9000000u, 0) == SAMOS_OK &&
                pmm_free_frame(g) == SAMOS_OK && pmm_free_frame(h) == SAMOS_OK);
    KTEST_CHECK(pmm_core_check(p) == 0 && audit_clean());
    const uint32_t av = 0xD0009000u;
    uint32_t td = pdir()[av >> 22] & PTE_FRAME, idx = (av >> 12) & 0x3FFu;
    bool apre = (pdir()[av >> 22] & PTE_PRESENT) && entry_pte(av) == 0;
    KTEST_CHECK(apre);
    if (apre) {
        samos_err_t ew = ptgate_write_entry(td, idx, pt0 | PTE_PRESENT | PTE_WRITE);
        struct paging_audit a;
        bool ok = paging_audit(&a);
        bool audit_sees_foreign_alias = ew == SAMOS_OK && !ok && a.foreign_aliases > 0;
        kprintf("[TEST] D1 audit after an alias written outside pt_gate_write: foreign_aliases=%u\n",
                a.foreign_aliases);
        (void)ptgate_write_entry(td, idx, 0);
        cpu_invlpg((const void *)(uintptr_t)av);
        KTEST_CHECK(audit_sees_foreign_alias);
        bool audit_clean_after_removal = entry_pte(av) == 0 && audit_clean();
        KTEST_CHECK(audit_clean_after_removal);
    }
}

/*
 * K0.7-P4f-B3 — Mode destructif test=tablealias (statut 37) : variante V2 de
 * D1. Un cadre x, projeté à 0xD0008000, est marqué FREE par falsification ;
 * le PMM le distribue comme table neuve.
 * K1.0-N3 : le PMM l'arrête désormais lui-même, AVANT la remise à zéro
 * (« still mapped outside identity … handed out as type 4 », N3-I2). Le
 * contrôle D1 de table_create (« frame already mapped elsewhere ») devient une
 * seconde couche, éprouvée par test=tablealias_l2 qui neutralise la première
 * (image de test seulement). Même cadre, même scénario dans les deux modes.
 */
void ktest_table_alias(void)
{
    struct pmm *pw = (struct pmm *)(uintptr_t)pmm_state();
    const uint32_t a2 = 0xD0008000u, nv = 0xCA000000u;
    /* La table de la région de a2 doit exister AVANT d'allouer x : sinon sa
     * création déplacerait le curseur du PMM au-delà de x. */
    uint32_t z = pmm_alloc_frame();
    if (z == 0 || vmm_map(0xD0000000u, z, PTE_WRITE) != SAMOS_OK) {
        kprintf("[TEST] tablealias: preconditions not met\n");
        return;
    }
    uint32_t y = pmm_alloc_frame();             /* cadre de données (AVANT x) */
    uint32_t x = pmm_alloc_frame();             /* next-fit : hint = x */
    if (y == 0 || x == 0 || vmm_map(a2, x, PTE_WRITE) != SAMOS_OK || (pdir()[nv >> 22] & PTE_PRESENT)) {
        kprintf("[TEST] tablealias: preconditions not met\n");
        return;
    }
    meta_rw()[x >> 12] = frame_desc_make(FRAME_FREE, 0);
    pw->free_count++;
    kprintf("[TEST] tablealias: frame 0x%08x mapped at 0x%08x, marked FREE; creating a table\n", x, a2);
    samos_err_t e = vmm_map(nv, y, PTE_WRITE);
    kprintf("[TEST] internal violation NOT fatal (vmm_map returned %d)\n", (int)e);
}

void ktest_table_alias_l2(void)
{
    pmm_test_skip_n3_alloc_check(true);         /* première couche (PMM) neutralisée */
    kprintf("[TEST] tablealias_l2: PMM mapped-frame check disabled (test image only)\n");
    ktest_table_alias();
}
