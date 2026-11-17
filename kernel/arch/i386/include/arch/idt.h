/*
 * SAMOS / AtlasOS — kernel/arch/i386/include/arch/idt.h
 * Format binaire des entrées de l'IDT (SDM vol. 3A §6.11, figure 6-2).
 */
#ifndef SAMOS_ARCH_IDT_H
#define SAMOS_ARCH_IDT_H

#include <stdint.h>
#include <samos/compiler.h>

struct idt_entry {
    uint16_t offset_low;    /* adresse du gestionnaire, bits 0..15 (0 pour une porte de tâche) */
    uint16_t selector;      /* sélecteur de code (ou de TSS pour une porte de tâche) */
    uint8_t  zero;
    uint8_t  type_attr;     /* P | DPL(2) | 0 | type(4) */
    uint16_t offset_high;   /* bits 16..31 */
} SAMOS_PACKED;
SAMOS_STATIC_ASSERT(sizeof(struct idt_entry) == 8, "porte IDT = 8 octets");

#define IDT_PRESENT        0x80u
#define IDT_DPL0           0x00u
#define IDT_TYPE_INT32     0x0Eu   /* porte d'interruption 32 bits : IF <- 0 */
#define IDT_TYPE_TRAP32    0x0Fu   /* porte de trappe 32 bits : IF inchangé */
#define IDT_TYPE_TASK      0x05u   /* porte de tâche */

const struct idt_entry *idt_table(void);

#endif /* SAMOS_ARCH_IDT_H */
