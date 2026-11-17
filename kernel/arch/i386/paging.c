/*
 * SAMOS / AtlasOS — kernel/arch/i386/paging.c
 * Pagination 32 bits et gestionnaire de fautes de page. Voir paging.h et
 * Details.md §35-36.
 *
 * Toutes les tables sont accédées par leur adresse physique, valable aussi
 * après activation car la RAM gérée est projetée en identité (va == pa) :
 * l'allocateur physique ne distribue que des cadres situés sous idmap_top.
 */
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/interrupt.h>
#include <arch/paging.h>
#include <samos/kernel.h>
#include <samos/log.h>
#include <samos/panic.h>
#include <samos/pmm.h>

#define PD_INDEX(va) ((va) >> 22)
#define PT_INDEX(va) (((va) >> 12) & 0x3FFu)

extern char mem_probe_read_insn[];
extern char mem_probe_write_insn[];
extern char mem_probe_fixup[];

static uint32_t *g_pd;            /* répertoire de pages (physique = virtuel) */
static uint32_t g_tables;
static uint32_t g_idmap_top;
static volatile uint32_t g_probe_cr2;
static volatile uint32_t g_probe_err;

static bool paging_enabled(void)
{
    return (cpu_read_cr0() & CR0_PG) != 0;
}

static uint32_t *get_table(uint32_t va, bool create)
{
    uint32_t pde = g_pd[PD_INDEX(va)];
    if (pde & PTE_PRESENT) {
        return (uint32_t *)(uintptr_t)(pde & PTE_FRAME);
    }
    if (!create) {
        return 0;
    }
    uint32_t pt = pmm_alloc_frame();           /* remise à zéro : 1024 PTE non présentes */
    if (pt == 0) {
        return 0;
    }
    /* PDE permissive (P | W) : les droits effectifs sont l'INTERSECTION des
     * droits du PDE et du PTE (SDM vol. 3A §4.6) ; c'est le PTE qui décide. */
    g_pd[PD_INDEX(va)] = pt | PTE_PRESENT | PTE_WRITE;
    g_tables++;
    return (uint32_t *)(uintptr_t)pt;
}

samos_err_t vmm_map(uint32_t va, uint32_t pa, uint32_t flags)
{
    if ((va | pa) & 0xFFFu || (flags & ~0xFFFu) != 0) {
        return SAMOS_EINVAL;
    }
    uint32_t *pt = get_table(va, true);
    if (pt == 0) {
        return SAMOS_ENOMEM;
    }
    if (pt[PT_INDEX(va)] & PTE_PRESENT) {
        return SAMOS_EEXIST;                   /* jamais d'écrasement silencieux */
    }
    pt[PT_INDEX(va)] = pa | flags | PTE_PRESENT;
    if (paging_enabled()) {
        cpu_invlpg((const void *)(uintptr_t)va);   /* pas d'entrée périmée « non présente » dans le TLB */
    }
    return SAMOS_OK;
}

samos_err_t vmm_unmap(uint32_t va, uint32_t *pa)
{
    uint32_t *pt = get_table(va & PTE_FRAME, false);
    if (pt == 0 || !(pt[PT_INDEX(va)] & PTE_PRESENT)) {
        return SAMOS_ENOENT;
    }
    if (pa != 0) {
        *pa = pt[PT_INDEX(va)] & PTE_FRAME;
    }
    pt[PT_INDEX(va)] = 0;
    /* Indispensable : sans INVLPG, le TLB peut conserver l'ancienne
     * traduction et l'accès continuerait de réussir (vérifié par autotest). */
    cpu_invlpg((const void *)(uintptr_t)(va & PTE_FRAME));
    return SAMOS_OK;
}

bool vmm_translate(uint32_t va, uint32_t *pa, uint32_t *flags)
{
    uint32_t *pt = get_table(va, false);
    if (pt == 0) {
        return false;
    }
    uint32_t pte = pt[PT_INDEX(va)];
    if (!(pte & PTE_PRESENT)) {
        return false;
    }
    if (pa != 0) {
        *pa = (pte & PTE_FRAME) | (va & 0xFFFu);
    }
    if (flags != 0) {
        *flags = pte & 0xFFFu;
    }
    return true;
}

uint32_t paging_directory(void) { return (uint32_t)(uintptr_t)g_pd; }
uint32_t paging_table_count(void) { return g_tables; }

void mem_probe_last_fault(uint32_t *cr2, uint32_t *err)
{
    *cr2 = g_probe_cr2;
    *err = g_probe_err;
}

static bool in_range(uint32_t a, const char *lo, const char *hi)
{
    return a >= (uint32_t)(uintptr_t)lo && a < (uint32_t)(uintptr_t)hi;
}

const char *paging_classify(uint32_t addr, uint32_t err)
{
    bool protection = (err & 1u) != 0;
    bool write = (err & 2u) != 0;
    if (addr < PAGE_SIZE) {
        return "NULL pointer dereference (page 0 is never mapped)";
    }
    if (in_range(addr, __stack_guard_start, __stack_guard_end)) {
        return "kernel stack overflow (guard page below the boot stack)";
    }
    if (protection && write && in_range(addr, __text_start, __rodata_start)) {
        return "write to read-only kernel code (.text)";
    }
    if (protection && write && in_range(addr, __rodata_start, __data_start)) {
        return "write to read-only kernel data (.rodata)";
    }
    if (!protection && addr >= g_idmap_top) {
        return "access to an unmapped address (outside identity-mapped RAM)";
    }
    return protection ? "protection violation" : "access to an unmapped page";
}

bool paging_is_probe_fault(const struct interrupt_frame *f)
{
    return f->eip == (uint32_t)(uintptr_t)mem_probe_read_insn ||
           f->eip == (uint32_t)(uintptr_t)mem_probe_write_insn;
}

static void page_fault_handler(struct interrupt_frame *f)
{
    uint32_t cr2 = cpu_read_cr2();
    /* Faute attendue dans une sonde : correction et reprise (probe.asm). */
    if (paging_is_probe_fault(f)) {
        g_probe_cr2 = cr2;
        g_probe_err = f->err_code;
        f->eip = (uint32_t)(uintptr_t)mem_probe_fixup;
        return;
    }
    exception_report(f);
    const char *why = paging_classify(cr2, f->err_code);
    kprintf("    diagnosis: %s\n", why);
    panic("page fault at %p: %s", (void *)(uintptr_t)cr2, why);
}

samos_err_t paging_init(uint32_t idmap_top)
{
    uint32_t kend = (uint32_t)(uintptr_t)__kernel_end;
    if (idmap_top < kend) {
        idmap_top = kend;                      /* l'image du noyau doit être projetée */
    }
    idmap_top &= PTE_FRAME;
    g_idmap_top = idmap_top;

    uint32_t pd = pmm_alloc_frame();
    if (pd == 0) {
        return SAMOS_ENOMEM;
    }
    g_pd = (uint32_t *)(uintptr_t)pd;

    uint32_t ro_start = (uint32_t)(uintptr_t)__text_start;
    uint32_t ro_end = (uint32_t)(uintptr_t)__data_start;      /* .text + .rodata */
    uint32_t guard_start = (uint32_t)(uintptr_t)__stack_guard_start;
    uint32_t guard_end = (uint32_t)(uintptr_t)__stack_guard_end;

    /* Page 0 : jamais projetée (on commence à 4 KiB). */
    for (uint32_t va = PAGE_SIZE; va < idmap_top; va += PAGE_SIZE) {
        if (va >= guard_start && va < guard_end) {
            continue;                          /* page de garde : NON présente */
        }
        uint32_t flags = (va >= ro_start && va < ro_end) ? 0u : PTE_WRITE;
        samos_err_t e = vmm_map(va, va, flags);
        if (e != SAMOS_OK) {
            return e;
        }
    }

    if (!interrupt_register(EXC_PF, page_fault_handler)) {
        return SAMOS_EBUSY;
    }
    /* La tâche de double faute doit voir les mêmes projections. */
    gdt_set_double_fault_cr3(pd);

    cpu_write_cr3(pd);
    /* PG : active la traduction. WP : les pages en lecture seule le sont aussi
     * pour le ring 0 (sinon le noyau pourrait écrire dans .text sans faute). */
    cpu_write_cr0(cpu_read_cr0() | CR0_PG | CR0_WP);
    return SAMOS_OK;
}
