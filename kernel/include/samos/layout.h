/*
 * SAMOS / AtlasOS — kernel/include/samos/layout.h
 * ---------------------------------------------------------------------------
 * Disposition mémoire de l'image du noyau (jalon K0.2).
 *
 * Fournit une description tabulaire et vérifiable des régions définies par
 * linker.ld. Elle sert :
 *   - au diagnostic (affichage au démarrage),
 *   - à la gestion mémoire (K0.6) : réservation de l'image du noyau dans
 *     l'allocateur physique, puis droits de pagination par région
 *     (R-X pour .text, R-- pour .rodata, RW- pour .data/.bss/pile,
 *     non présent pour la zone de garde).
 */
#ifndef SAMOS_LAYOUT_H
#define SAMOS_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Droits prévus pour une région (appliqués par la pagination en K0.6). */
#define LAYOUT_R     (1u << 0)
#define LAYOUT_W     (1u << 1)
#define LAYOUT_X     (1u << 2)
#define LAYOUT_GUARD (1u << 3)   /* ne doit jamais être accédée */

struct kernel_region {
    const char *name;
    uintptr_t start;   /* inclus, aligné sur 4 KiB */
    uintptr_t end;     /* exclu */
    uint32_t perms;
};

/* Retourne le tableau des régions (ordre croissant d'adresses). */
size_t kernel_layout(const struct kernel_region **out);

/* Vérifie les invariants de disposition ; retourne true si tous tiennent :
 * alignement 4 KiB, ordre croissant, absence de chevauchement, image
 * contenue dans [__kernel_start, __kernel_end). */
bool kernel_layout_valid(void);

/* Affiche la disposition sur le journal du noyau. */
void kernel_layout_print(void);

/* Profondeur maximale de pile observée depuis le démarrage, en octets
 * (cases de la pile dont le motif de peinture a été écrasé). */
size_t boot_stack_high_water(void);

/* Vrai si la zone de garde n'a jamais été écrite (aucun débordement détecté
 * par logiciel). Détection a posteriori uniquement : voir Details.md §15. */
bool boot_stack_guard_intact(void);

#endif /* SAMOS_LAYOUT_H */
