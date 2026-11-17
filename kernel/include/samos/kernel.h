/*
 * SAMOS / AtlasOS — kernel/include/samos/kernel.h
 * Symboles globaux du noyau : point d'entrée C et symboles définis par le
 * script de liaison (kernel/arch/i386/linker.ld) ou par entry.asm.
 *
 * Les symboles du linker n'ont PAS de valeur stockée : seule leur ADRESSE a
 * un sens. On les déclare donc comme des tableaux de char, et on utilise leur
 * nom (qui se dégrade en adresse) — jamais leur contenu.
 */
#ifndef SAMOS_KERNEL_H
#define SAMOS_KERNEL_H

#include <stdint.h>
#include <samos/compiler.h>

#define SAMOS_NAME     "SAMOS/AtlasOS"
#define SAMOS_VERSION  "0.1.0-k0"

/* Disposition de l'image du noyau (linker.ld) */
extern char __kernel_start[];
extern char __kernel_end[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];

/* Pile de démarrage (entry.asm) */
extern char boot_stack_bottom[];
extern char boot_stack_top[];

/* Point d'entrée C, appelé par _start (entry.asm) selon la convention cdecl. */
SAMOS_NORETURN void kmain(uint32_t magic, uint32_t mbi_phys);

/* Adresse du cadre de pile de kmain, mémorisée pour les autotests d'ABI. */
extern uintptr_t kmain_frame_address;

#endif /* SAMOS_KERNEL_H */
