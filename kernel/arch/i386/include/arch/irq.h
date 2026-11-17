/*
 * SAMOS / AtlasOS — kernel/arch/i386/include/arch/irq.h
 * ---------------------------------------------------------------------------
 * Couche IRQ (K0.5) : relie les lignes du PIC aux vecteurs 0x20..0x2F, appelle
 * le gestionnaire du pilote, gère les IRQ parasites (spurious) et envoie l'EOI.
 *
 * Règles imposées aux gestionnaires d'IRQ (contexte d'interruption) :
 *   - courts, sans attente active, sans journalisation (kprintf n'est pas
 *     réentrant, Details.md §18.5) ;
 *   - ne pas envoyer l'EOI eux-mêmes (irq_dispatch s'en charge) ;
 *   - communiquer avec le reste du noyau par structures SPSC (ringbuf) ou
 *     compteurs.
 */
#ifndef SAMOS_ARCH_IRQ_H
#define SAMOS_ARCH_IRQ_H

#include <stdbool.h>
#include <stdint.h>
#include <arch/interrupt.h>

typedef void (*irq_handler_t)(void);

/* Enregistre le gestionnaire, rend la porte IDT présente et démasque l'IRQ.
 * Retourne false si irq >= 16, si le gestionnaire est nul ou déjà défini. */
bool irq_register(uint8_t irq, irq_handler_t handler);
void irq_unregister(uint8_t irq);

/* Appelé par isr_dispatch pour les vecteurs 0x20..0x2F. */
void irq_dispatch(struct interrupt_frame *frame);

/* Statistiques (lecture seule, pour diagnostic et tests). */
uint32_t irq_count(uint8_t irq);
uint32_t irq_spurious_count(void);
uint32_t irq_unhandled_count(void);

#endif /* SAMOS_ARCH_IRQ_H */
