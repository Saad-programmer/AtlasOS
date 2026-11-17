/*
 * SAMOS / AtlasOS — kernel/arch/i386/include/arch/pic.h
 * ---------------------------------------------------------------------------
 * Contrôleur d'interruptions programmable Intel 8259A, en cascade maître /
 * esclave (K0.5). Référence : Intel 8259A Programmable Interrupt Controller,
 * fiche technique (ICW1-ICW4, OCW1-OCW3).
 *
 *   maître  : ports 0x20 (commande) / 0x21 (données), IRQ 0..7
 *   esclave : ports 0xA0 / 0xA1, IRQ 8..15, relié à l'entrée IRQ 2 du maître
 *
 * Au démarrage, le BIOS laisse le maître sur les vecteurs 0x08..0x0F, qui
 * COLLISIONNENT avec les exceptions processeur (#DF = 8, #GP = 13...). On
 * remappe donc maître -> 0x20..0x27, esclave -> 0x28..0x2F.
 */
#ifndef SAMOS_ARCH_PIC_H
#define SAMOS_ARCH_PIC_H

#include <stdbool.h>
#include <stdint.h>

#define PIC_IRQ_BASE   0x20
#define PIC_IRQ_COUNT  16
#define PIC_CASCADE_IRQ 2

void pic_init(void);                 /* remappe et masque TOUTES les IRQ */
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_eoi(uint8_t irq);           /* fin d'interruption (maître, + esclave si irq >= 8) */
uint16_t pic_masks(void);            /* IMR esclave << 8 | IMR maître */
uint16_t pic_isr(void);              /* registre « In-Service » (OCW3) */
uint16_t pic_irr(void);              /* registre « Interrupt Request » (OCW3) */

#endif /* SAMOS_ARCH_PIC_H */
