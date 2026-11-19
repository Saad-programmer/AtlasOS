/*
 * SAMOS / AtlasOS — kernel/tests/test_imageint.c
 * ---------------------------------------------------------------------------
 * K0.7-P4f-D — Intégrité des cadres de l'image du noyau et de la mémoire
 * basse face à des métadonnées PMM falsifiées (failles N1, N2, N5 de l'audit
 * P4f-C).
 *
 * Mode ISOLÉ « test=imageint » (statut 33) : N2 (le passage refuse une PDE
 * vers un cadre de l'image ou de la mémoire basse, même typé PAGETABLE) et N5
 * (l'audit signale une table interdite, un cadre de l'image mal typé, un
 * cadre de mémoire basse distribuable). Falsifications restaurées.
 *
 * Modes DESTRUCTIFS (statut 37) : N1. Un cadre protégé est marqué FREE et
 * désigné au PMM comme prochain cadre ; l'allocation doit s'arrêter AVANT la
 * remise à zéro. Le mode affiche l'empreinte de la page avant l'allocation ;
 * le diagnostic de panique affiche celle de la page au moment du refus : le
 * banc QEMU exige qu'elles soient égales (page intacte).
 *   n1gdt    GDT/TSS (kcpu), création de table par vmm_map
 *   n1data   première page de .data, création de table
 *   n1stack  page de la pile de démarrage, création de table
 *   n1alloc  page de test DANS l'image (page entière d'un tampon de .bss, motif connu),
 *            allocation de DONNÉES (pmm_alloc_frame). Page sans rôle pour le
 *            noyau : sous une mutation qui la laisserait remettre à zéro, le
 *            noyau reste capable de signaler le défaut (une vraie page de
 *            .data, n1data, ferait tout s'arrêter en silence).
 *   n1low    mémoire VGA 0xB8000 (mémoire basse), création de table
 */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <samos/frame.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pmm.h>
#include <samos/string.h>

samos_err_t ptgate_write_entry(uint32_t table_phys, uint32_t index, uint32_t value);

/* Tampon de l'image (.bss) sans autre usage que n1alloc : 8 KiB NON alignés,
 * qui contiennent forcément une page entière (probe_page()). Un tableau
 * aligné sur 4 KiB ferait coïncider la fin de sa section avec une page
 * réservée (garde des piles, page kcpu), ce que check_elf.sh refuse à juste
 * titre. */
static uint8_t g_image_probe_buf[8192];

static uint8_t *probe_page(void)
{
    return (uint8_t *)(((uintptr_t)g_image_probe_buf + 4095u) & ~(uintptr_t)4095u);
}

static const volatile uint32_t *pdir(void)
{
    return (const volatile uint32_t *)(uintptr_t)paging_directory();
}

static frame_desc_t *meta_rw(void)
{
    return (frame_desc_t *)(uintptr_t)pmm_state()->meta;
}

static uint32_t windows_now(void)
{
    uint32_t w = 0, r = 0;
    paging_gate_stats(&w, &r);
    return w;
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

/* Le passage refuse-t-il une PDE (absente) vers `frame` typé PAGETABLE ? */
static bool pde_target_refused(uint32_t frame)
{
    const uint32_t va = 0xF8000000u;
    frame_desc_t saved = meta_rw()[frame >> 12];
    meta_rw()[frame >> 12] = frame_desc_make(FRAME_PAGETABLE, 0);
    bool lies = pmm_core_type(pmm_state(), frame >> 12) == FRAME_PAGETABLE;
    uint32_t pde0 = pdir()[va >> 22], w0 = windows_now();
    samos_err_t e = paging_gate_test_write(0, va, frame | PTE_PRESENT | PTE_WRITE);
    if (e == SAMOS_OK) {
        (void)ptgate_write_entry(paging_directory(), va >> 22, 0);   /* mutation : retirer avant tout accès */
    }
    meta_rw()[frame >> 12] = saved;
    kprintf("[TEST] N2 PDE -> frame 0x%08x typed PAGETABLE: gate=%d\n", frame, (int)e);
    return lies && e == SAMOS_EPERM && pdir()[va >> 22] == pde0 && !(pde0 & PTE_PRESENT) &&
           windows_now() == w0 && (cpu_read_cr0() & CR0_WP);
}

void ktest_suite_image_integrity(void)
{
    ktest_suite("K0.7 P4f-D kernel image integrity (isolated)");
    const struct pmm *p = pmm_state();
    struct paging_audit a;
    KTEST_CHECK(paging_audit(&a) && a.forbidden_tables == 0 && a.image_type_mismatch == 0 &&
                a.lowmem_allocatable == 0);
    KTEST_CHECK(!(pdir()[0xF8000000u >> 22] & PTE_PRESENT));

    /* --- N2 : cibles de PDE interdites, même typées PAGETABLE --- */
    bool text_pde_refused = pde_target_refused((uint32_t)(uintptr_t)&kmain & PTE_FRAME);
    KTEST_CHECK(text_pde_refused);
    bool idt_pde_refused = pde_target_refused((uint32_t)(uintptr_t)__kro_idt_start);
    KTEST_CHECK(idt_pde_refused);
    bool gdt_pde_refused = pde_target_refused((uint32_t)(uintptr_t)__kcpu_start);
    KTEST_CHECK(gdt_pde_refused);
    bool lowmem_pde_refused = pde_target_refused(0xB8000u);
    KTEST_CHECK(lowmem_pde_refused);
    KTEST_CHECK(pmm_core_check(p) == 0);

    /* --- N5 : audit --- */
    /* (i) table interdite publiée HORS du passage (prototype P4c) : une PDE
     * vers une page de .text ; retirée aussitôt, sans accès. */
    uint32_t tp = (uint32_t)(uintptr_t)&kmain & PTE_FRAME;
    samos_err_t ew = ptgate_write_entry(paging_directory(), 0xF8000000u >> 22, tp | PTE_PRESENT | PTE_WRITE);
    bool ok1 = paging_audit(&a);
    uint32_t forb = a.forbidden_tables;
    (void)ptgate_write_entry(paging_directory(), 0xF8000000u >> 22, 0);
    bool audit_sees_forbidden_table = ew == SAMOS_OK && !ok1 && forb >= 1;
    KTEST_CHECK(audit_sees_forbidden_table);
    /* (ii) cadre de l'image marqué FREE (aucune allocation). */
    frame_desc_t sv = meta_rw()[tp >> 12];
    meta_rw()[tp >> 12] = frame_desc_make(FRAME_FREE, 0);
    bool ok2 = paging_audit(&a);
    uint32_t mism = a.image_type_mismatch;
    meta_rw()[tp >> 12] = sv;
    bool audit_sees_image_type_mismatch = !ok2 && mism == 1;
    KTEST_CHECK(audit_sees_image_type_mismatch);
    /* (iii) cadre de la mémoire basse marqué FREE. */
    sv = meta_rw()[0xB8000u >> 12];
    meta_rw()[0xB8000u >> 12] = frame_desc_make(FRAME_FREE, 0);
    bool ok3 = paging_audit(&a);
    uint32_t lowa = a.lowmem_allocatable;
    meta_rw()[0xB8000u >> 12] = sv;
    bool audit_sees_lowmem_allocatable = !ok3 && lowa == 1;
    KTEST_CHECK(audit_sees_lowmem_allocatable);
    kprintf("[TEST] N5 audit: forbidden_tables=%u image_type_mismatch=%u lowmem_allocatable=%u\n", forb, mism, lowa);

    /* État final : audit propre, PMM cohérent, WP. */
    KTEST_CHECK(paging_audit(&a) && pmm_core_check(p) == 0 && (cpu_read_cr0() & CR0_WP));
    KTEST_CHECK(!(pdir()[0xF8000000u >> 22] & PTE_PRESENT));
}

/* Modes destructifs N1 (voir l'en-tête). */
void ktest_image_steal(const char *mode)
{
    struct pmm *pw = (struct pmm *)(uintptr_t)pmm_state();
    bool data_alloc = strcmp(mode, "n1alloc") == 0;
    uint32_t f = strcmp(mode, "n1gdt") == 0 ? (uint32_t)(uintptr_t)__kcpu_start
               : strcmp(mode, "n1stack") == 0 ? (((uint32_t)(uintptr_t)boot_stack_top - 4096u) & PTE_FRAME)
               : strcmp(mode, "n1low") == 0 ? 0xB8000u
               : data_alloc ? (uint32_t)(uintptr_t)probe_page()
               : ((uint32_t)(uintptr_t)__data_start & PTE_FRAME);
    if (data_alloc) {
        for (uint32_t i = 0; i < 4096u; i++) {
            probe_page()[i] = (uint8_t)(0xA5u ^ i);
        }
    }
    uint32_t d = pmm_alloc_frame();
    const uint32_t va = 0xCB000000u;
    if (d == 0 || (pdir()[va >> 22] & PTE_PRESENT)) {
        kprintf("[TEST] %s: preconditions not met\n", mode);
        return;
    }
    enum frame_type t0 = pmm_core_type(pw, f >> 12);
    meta_rw()[f >> 12] = frame_desc_make(FRAME_FREE, 0);
    pw->free_count++;
    pw->hint = f >> 12;
    bool lies = pmm_core_type(pw, f >> 12) == FRAME_FREE;
    kprintf("[TEST] %s: protected frame 0x%08x (type %d) marked FREE (PMM lies: %d), page checksum 0x%08x, "
            "PDE 0x%08x, CR0 0x%08x; %s\n", mode, f, (int)t0, (int)lies, page_checksum(f), pdir()[va >> 22],
            cpu_read_cr0(), data_alloc ? "allocating a data frame" : "creating a page table");
    if (data_alloc) {
        uint32_t g = pmm_alloc_frame();
        kprintf("[TEST] protected frame handed out NOT trapped (data frame 0x%08x)\n", g);
    } else {
        samos_err_t e = vmm_map(va, d, PTE_WRITE);
        kprintf("[TEST] protected frame handed out NOT trapped (vmm_map returned %d)\n", (int)e);
    }
}
