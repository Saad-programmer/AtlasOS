/*
 * SAMOS / AtlasOS — kernel/arch/i386/idt.c
 * Construction et chargement de la table des descripteurs d'interruption.
 * Voir Details.md §23.
 *
 * Politique (K0.4) :
 *   - vecteurs 0..31 (exceptions) : portes d'INTERRUPTION 32 bits, DPL 0,
 *     vers les stubs de isr_stubs.asm — sauf le vecteur 8 ;
 *   - vecteur 8 (#DF) : porte de TÂCHE vers le TSS de double faute (pile
 *     dédiée : survit à une pile noyau corrompue ou épuisée) ;
 *   - vecteurs 32..47 : stubs préparés pour les IRQ, laissés NON PRÉSENTS
 *     jusqu'à K0.5 (le PIC n'est pas encore programmé) ;
 *   - vecteurs 48..255 : entrées entièrement nulles. Une interruption vers un
 *     tel vecteur déclenche #GP (type de porte invalide, vérifié AVANT le bit
 *     de présence) ; vers 32..47 (type valide, P = 0), elle déclenche #NP.
 *     Dans les deux cas le code d'erreur désigne le vecteur (bit IDT), et le
 *     gestionnaire d'exceptions le décode et le signale.
 * DPL 0 partout : une instruction INT n exécutée en ring 3 (futur) sur l'un
 * de ces vecteurs provoquera #GP au lieu d'entrer dans le noyau.
 */
#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/interrupt.h>
#include <arch/cpu.h>
#include <samos/string.h>

#define STUB_COUNT 48
extern const uint32_t isr_stub_table[STUB_COUNT];

/* K0.7-P4b — Page dédiée (.bss.p4_idt) : candidate à la lecture seule après
 * l'initialisation (P4d) ; le CPU ne fait que LIRE l'IDT. */
static struct idt_entry g_idt[IDT_ENTRIES] SAMOS_ALIGNED(8) SAMOS_SECTION(".bss.p4_idt");

static void idt_set_gate(uint8_t vector, uint32_t handler, uint16_t selector, uint8_t type_attr)
{
    g_idt[vector].offset_low  = (uint16_t)(handler & 0xFFFFu);
    g_idt[vector].selector    = selector;
    g_idt[vector].zero        = 0;
    g_idt[vector].type_attr   = type_attr;
    g_idt[vector].offset_high = (uint16_t)(handler >> 16);
}

void idt_init(void)
{
    memset(g_idt, 0, sizeof(g_idt));   /* toutes les entrées : P = 0 */

    for (unsigned v = 0; v < STUB_COUNT; v++) {
        uint8_t attr = IDT_DPL0 | IDT_TYPE_INT32;
        if (v < EXC_COUNT) {
            attr |= IDT_PRESENT;
        }
        idt_set_gate((uint8_t)v, isr_stub_table[v], GDT_KERNEL_CODE_SEL, attr);
    }
    /* #DF : porte de tâche ; l'offset est ignoré, le sélecteur désigne le TSS. */
    idt_set_gate(EXC_DF, 0, GDT_TSS_DF_SEL, IDT_PRESENT | IDT_DPL0 | IDT_TYPE_TASK);

    struct cpu_dtr idtr = { (uint16_t)(sizeof(g_idt) - 1), (uint32_t)(uintptr_t)g_idt };
    __asm__ __volatile__("lidt %0" : : "m"(idtr) : "memory");
}

void idt_set_present(uint8_t vector, bool present)
{
    if (present) {
        g_idt[vector].type_attr |= IDT_PRESENT;
    } else {
        g_idt[vector].type_attr &= (uint8_t)~IDT_PRESENT;
    }
}

const struct idt_entry *idt_table(void)
{
    return g_idt;
}
