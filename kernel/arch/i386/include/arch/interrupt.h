/*
 * SAMOS / AtlasOS — kernel/arch/i386/include/arch/interrupt.h
 * ---------------------------------------------------------------------------
 * Cadre d'interruption et enregistrement des gestionnaires (K0.4).
 *
 * struct interrupt_frame décrit EXACTEMENT la pile construite par
 * isr_stubs.asm puis par le processeur, de l'adresse la plus basse (champ
 * gs, sommet de pile au moment de l'appel C) à la plus haute (eflags) :
 *
 *   empilé par isr_common : gs fs es ds | edi esi ebp esp_pusha ebx edx ecx eax
 *   empilé par le stub    : vector, err_code (0 si le CPU n'en fournit pas)
 *   empilé par le CPU     : eip cs eflags  [+ esp ss si changement de privilège]
 *
 * En K0 tout s'exécute en ring 0 : il n'y a jamais de changement de
 * privilège, donc pas de champs user_esp/user_ss (ajoutés en K1).
 */
#ifndef SAMOS_ARCH_INTERRUPT_H
#define SAMOS_ARCH_INTERRUPT_H

#include <stdbool.h>
#include <stdint.h>
#include <samos/compiler.h>

#define IDT_ENTRIES          256
#define EXC_COUNT            32      /* vecteurs 0..31 réservés par Intel */

/* Vecteurs d'exception (SDM vol. 3A, tableau 6-1) */
#define EXC_DE   0   /* Divide Error */
#define EXC_DB   1   /* Debug */
#define EXC_NMI  2   /* Non-Maskable Interrupt */
#define EXC_BP   3   /* Breakpoint (INT3) */
#define EXC_OF   4   /* Overflow (INTO) */
#define EXC_BR   5   /* BOUND Range Exceeded */
#define EXC_UD   6   /* Invalid Opcode */
#define EXC_NM   7   /* Device Not Available */
#define EXC_DF   8   /* Double Fault */
#define EXC_TS   10  /* Invalid TSS */
#define EXC_NP   11  /* Segment Not Present */
#define EXC_SS   12  /* Stack-Segment Fault */
#define EXC_GP   13  /* General Protection */
#define EXC_PF   14  /* Page Fault */
#define EXC_MF   16  /* x87 Floating-Point Error */
#define EXC_AC   17  /* Alignment Check */
#define EXC_MC   18  /* Machine Check */
#define EXC_XM   19  /* SIMD Floating-Point */
#define EXC_VE   20  /* Virtualization */
#define EXC_CP   21  /* Control Protection */

struct interrupt_frame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_pusha, ebx, edx, ecx, eax;
    uint32_t vector, err_code;
    uint32_t eip, cs, eflags;
} SAMOS_PACKED;

SAMOS_STATIC_ASSERT(sizeof(struct interrupt_frame) == 17 * 4, "cadre d'interruption");

/* Un gestionnaire reçoit le cadre et peut le MODIFIER (par exemple avancer
 * eip pour reprendre après l'instruction fautive) : les valeurs sont
 * restaurées par isr_common puis IRETD au retour. */
typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

void idt_init(void);

/* Enregistre/retire un gestionnaire. Retourne false si le vecteur est
 * invalide ou déjà occupé (register) — jamais d'écrasement silencieux. */
bool interrupt_register(uint8_t vector, interrupt_handler_t handler);
void interrupt_unregister(uint8_t vector);

/* Rend une entrée de l'IDT présente ou non présente (tests de double faute). */
void idt_set_present(uint8_t vector, bool present);

const char *exception_name(uint32_t vector);
bool exception_has_error_code(uint32_t vector);

/* ESP du code interrompu (même privilège) : juste au-dessus d'eflags. */
static inline uint32_t interrupt_frame_esp(const struct interrupt_frame *f)
{
    return (uint32_t)(uintptr_t)&f->eflags + 4u;
}

/* Rapport complet d'une exception (nom, code d'erreur décodé, registres). */
void exception_report(const struct interrupt_frame *frame);

/* Appelé par isr_common (isr_stubs.asm). */
void isr_dispatch(struct interrupt_frame *frame);

/* Point d'entrée de la tâche de double faute (porte de tâche, vecteur 8). */
SAMOS_NORETURN void double_fault_task(void);

#endif /* SAMOS_ARCH_INTERRUPT_H */
