/*
 * SAMOS / AtlasOS — kernel/arch/i386/gdt.c
 * Construction de la GDT et des TSS. Voir gdt.h et Details.md §22.
 */
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/interrupt.h>
#include <samos/kernel.h>
#include <samos/string.h>

/* Octet d'accès (SDM vol. 3A §3.4.5) */
#define ACC_PRESENT   0x80u
#define ACC_DPL0      0x00u
#define ACC_CODEDATA  0x10u   /* S = 1 : segment de code ou de données */
#define ACC_CODE_RX   0x0Au   /* type 1010 : code, exécution + lecture */
#define ACC_DATA_RW   0x02u   /* type 0010 : données, lecture + écriture */
#define ACC_TSS32     0x09u   /* S = 0, type 1001 : TSS 32 bits disponible */
/* Bit « Accessed » (type bit 0) : le CPU le positionne lui-même, en ÉCRIVANT
 * dans la GDT, au premier chargement d'un sélecteur (constaté en K0.4 : 0x92
 * devenait 0x93). On le pré-positionne pour que le processeur n'ait jamais à
 * écrire dans la table : elle pourra ainsi être projetée en lecture seule. */
#define ACC_ACCESSED  0x01u

/* Drapeaux (quartet haut de limit_flags) */
#define FLG_GRAN_4K   0x80u   /* G = 1 : limite exprimée en pages de 4 KiB */
#define FLG_32BIT     0x40u   /* D/B = 1 : segment 32 bits */

#define DF_STACK_SIZE 4096

static struct gdt_entry g_gdt[GDT_ENTRIES] SAMOS_ALIGNED(8);
static struct tss g_tss_main SAMOS_ALIGNED(16);
static struct tss g_tss_df SAMOS_ALIGNED(16);
static uint8_t g_df_stack[DF_STACK_SIZE] SAMOS_ALIGNED(16);


static void gdt_set(unsigned i, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    g_gdt[i].limit_low   = (uint16_t)(limit & 0xFFFFu);
    g_gdt[i].base_low    = (uint16_t)(base & 0xFFFFu);
    g_gdt[i].base_mid    = (uint8_t)((base >> 16) & 0xFFu);
    g_gdt[i].access      = access;
    g_gdt[i].limit_flags = (uint8_t)((flags & 0xF0u) | ((limit >> 16) & 0x0Fu));
    g_gdt[i].base_high   = (uint8_t)((base >> 24) & 0xFFu);
}

void gdt_init(void)
{
    memset(g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss_main, 0, sizeof(g_tss_main));
    memset(&g_tss_df, 0, sizeof(g_tss_df));

    /* 0 : nul. 1-2 : segments plats. Limite 0xFFFFF en unités de 4 KiB
     * (G = 1) = 0xFFFFF * 4096 + 4095 = 0xFFFFFFFF : tout l'espace 4 GiB. */
    gdt_set(1, 0, 0xFFFFFu, ACC_PRESENT | ACC_DPL0 | ACC_CODEDATA | ACC_CODE_RX | ACC_ACCESSED, FLG_GRAN_4K | FLG_32BIT);
    gdt_set(2, 0, 0xFFFFFu, ACC_PRESENT | ACC_DPL0 | ACC_CODEDATA | ACC_DATA_RW | ACC_ACCESSED, FLG_GRAN_4K | FLG_32BIT);

    /* TSS principal : sert de zone de sauvegarde lors d'une commutation de
     * tâche matérielle (double faute). ss0/esp0 seraient utilisés lors d'un
     * passage ring 3 -> ring 0 (K1) ; on les renseigne pour être cohérent.
     * iomap_base = taille du TSS : pas de bitmap d'E/S => tout accès aux
     * ports depuis un ring > IOPL sera refusé. */
    g_tss_main.ss0 = GDT_KERNEL_DATA_SEL;
    g_tss_main.esp0 = (uint32_t)(uintptr_t)boot_stack_top;
    g_tss_main.iomap_base = sizeof(struct tss);

    /* TSS de double faute : état initial complet de la tâche dédiée.
     * ESP = sommet de sa pile : le CPU y empile le code d'erreur (0) de #DF,
     * qui occupe alors la place d'une « adresse de retour » ; ESP vaut
     * sommet - 4 à l'entrée, ce qui respecte (ESP + 4) % 16 == 0. */
    g_tss_df.eip = (uint32_t)(uintptr_t)&double_fault_task;
    g_tss_df.esp = (uint32_t)(uintptr_t)(g_df_stack + DF_STACK_SIZE);
    g_tss_df.ss0 = GDT_KERNEL_DATA_SEL;
    g_tss_df.esp0 = g_tss_df.esp;
    g_tss_df.cs = GDT_KERNEL_CODE_SEL;
    g_tss_df.ds = g_tss_df.es = g_tss_df.fs = g_tss_df.gs = g_tss_df.ss = GDT_KERNEL_DATA_SEL;
    g_tss_df.eflags = EFLAGS_RSV1;          /* IF = 0, DF = 0 */
    g_tss_df.cr3 = cpu_read_cr3();
    g_tss_df.iomap_base = sizeof(struct tss);

    /* Limite d'un TSS = taille - 1 ; G = 0 (octets). */
    gdt_set(3, (uint32_t)(uintptr_t)&g_tss_main, sizeof(struct tss) - 1, ACC_PRESENT | ACC_DPL0 | ACC_TSS32, 0);
    gdt_set(4, (uint32_t)(uintptr_t)&g_tss_df, sizeof(struct tss) - 1, ACC_PRESENT | ACC_DPL0 | ACC_TSS32, 0);

    struct cpu_dtr gdtr = { (uint16_t)(sizeof(g_gdt) - 1), (uint32_t)(uintptr_t)g_gdt };
    gdt_load(&gdtr);
    tss_load(GDT_TSS_MAIN_SEL);
}

const struct gdt_entry *gdt_table(void) { return g_gdt; }
struct tss *tss_main(void) { return &g_tss_main; }
struct tss *tss_double_fault(void) { return &g_tss_df; }

void gdt_set_double_fault_cr3(uint32_t cr3)
{
    g_tss_df.cr3 = cr3;
}
