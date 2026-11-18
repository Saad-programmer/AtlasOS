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
/* K0.7-R1f-1 — S2 est définie en NASM (entry.asm, section .dfstack) et placée
 * par linker.ld dans .stacks, alignée sur 4 KiB, entre sa page de garde et la
 * garde basse de S1 (taille vérifiée par une assertion de l'éditeur de liens). */
#define g_df_stack ((uint8_t *)(uintptr_t)df_stack_area)
SAMOS_STATIC_ASSERT(DF_SENTINEL_WORDS * 4u <= 64u, "zone sentinelle petite devant S2");

static void df_stack_sentinels_init(void);   /* K0.7-R1d, défini après gdt_init */


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
    df_stack_sentinels_init();              /* K0.7-R1d : avant la création de la tâche #DF */

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

/* Historique R1d : définies avant gdt_init, ces fonctions faisaient passer
 * l'ancien tableau g_df_stack en tête de .bss. Depuis R1f-1, S2 est dans la
 * section .stacks (linker.ld) et sa place ne dépend plus de cet ordre. */
/* K0.7-R1d, R1f-1 — Remise à zéro de S2 puis pose des sentinelles à son bas
 * (voir gdt.h). Appelée par gdt_init AVANT que la tâche #DF existe (son TSS et
 * son descripteur sont créés juste après, la porte de tâche de l'IDT encore
 * plus tard) : S2 ne peut pas encore servir. Écritures limitées à
 * [df_stack_area, df_stack_area + 4096). */
static void df_stack_sentinels_init(void)
{
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)g_df_stack;
    /* K0.7-R1f-1 : S2 n'est plus dans .bss, donc plus remise à zéro par
     * _start ; contenu déterministe avant toute utilisation. */
    for (uint32_t i = 0; i < DF_STACK_SIZE / 4u; i++) {
        w[i] = 0;
    }
    for (uint32_t i = 0; i < DF_SENTINEL_WORDS; i++) {
        w[i] = DF_SENTINEL_VALUE(i);
    }
}

/* Lecture seule, sans appel ni sortie formatée, non instrumentée : appelée
 * depuis panic_at et depuis la tâche #DF, y compris quand S2 est suspecte.
 * Seules les adresses (fixées à l'édition de liens) et les valeurs attendues
 * (constantes du code) sont utilisées ; rien n'est déduit du contenu de S2. */
SAMOS_NO_STACK_PROTECTOR
uint32_t df_stack_sentinel_damage(void)
{
    const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)g_df_stack;
    uint32_t mask = 0;
    for (uint32_t i = 0; i < DF_SENTINEL_WORDS; i++) {
        if (w[i] != DF_SENTINEL_VALUE(i)) {
            mask |= 1u << i;
        }
    }
    return mask;
}

volatile uint32_t *df_stack_sentinels(void) { return (volatile uint32_t *)(uintptr_t)g_df_stack; }
uintptr_t df_stack_bottom(void) { return (uintptr_t)g_df_stack; }
uintptr_t df_stack_top(void) { return (uintptr_t)(g_df_stack + DF_STACK_SIZE); }

const struct gdt_entry *gdt_table(void) { return g_gdt; }
struct tss *tss_main(void) { return &g_tss_main; }
struct tss *tss_double_fault(void) { return &g_tss_df; }

void gdt_set_double_fault_cr3(uint32_t cr3)
{
    g_tss_df.cr3 = cr3;
}
