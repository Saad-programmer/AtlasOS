/*
 * SAMOS / AtlasOS — kernel/include/samos/stack_protector.h
 * ---------------------------------------------------------------------------
 * K0.7-R1b — Canaris de fonctions (instrumentation -fstack-protector-*).
 * Référence : docs/design/K0.7-R1-stack-canaries.md, section 3.
 *
 * Contrat avec Clang 18 (cible i686-unknown-none-elf, vérifié par
 * désassemblage) : chaque fonction instrumentée copie __stack_chk_guard dans
 * son cadre au prologue, la compare à l'épilogue et appelle __stack_chk_fail
 * si elles diffèrent.
 */
#ifndef SAMOS_STACK_PROTECTOR_H
#define SAMOS_STACK_PROTECTOR_H

#include <stdint.h>
#include <samos/compiler.h>

/* Valeur initiale dans l'image (doit rester égale à STACK_GUARD_DEFAULT,
 * entry.asm). Remplacée par _start avant tout code C ; ne reste en place que
 * si le mélange RDTSC donnait 0. */
#define STACK_GUARD_DEFAULT 0xE3A9C500u

/* Défini en NASM dans .rodata (entry.asm), lecture seule après la pagination.
 * Volontairement NON const : le compilateur ne doit jamais remplacer sa
 * lecture par la valeur initiale de l'image. */
extern uint32_t __stack_chk_guard;

/* Appelée par le code instrumenté lorsqu'un canari local a été modifié.
 * Ne retourne jamais : arrêt contrôlé par panic(). */
SAMOS_NORETURN void __stack_chk_fail(void);

#endif /* SAMOS_STACK_PROTECTOR_H */
