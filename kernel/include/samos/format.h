/*
 * SAMOS / AtlasOS — kernel/include/samos/format.h
 * ---------------------------------------------------------------------------
 * Formatage de texte de type printf, indépendant (K0.3).
 *
 * Sous-ensemble supporté (comportement identique à C11 snprintf pour ces cas,
 * vérifié par test différentiel sur l'hôte) :
 *   conversions : %c %s %d %i %u %x %X %p %%
 *   drapeaux    : '-' (alignement à gauche), '0' (remplissage par zéros)
 *   largeur     : nombre ou '*'
 *   précision   : '.N' ou '.*' (chiffres minimaux pour les entiers,
 *                 longueur maximale lue pour %s ; une précision négative
 *                 passée par '*' équivaut à « absente »)
 *   longueurs   : l, ll, z
 * Une conversion inconnue est recopiée telle quelle (pas de comportement
 * indéfini, contrairement à la libc).
 *
 * Sécurité : n'écrit JAMAIS plus de `size` octets, termine toujours la chaîne
 * si size > 0, et retourne la longueur qu'aurait eue la sortie complète (comme
 * snprintf) afin que l'appelant puisse détecter la troncature.
 * %s avec NULL affiche "(null)".
 */
#ifndef SAMOS_FORMAT_H
#define SAMOS_FORMAT_H

#include <stdarg.h>
#include <stddef.h>
#include <samos/compiler.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) SAMOS_PRINTF(3, 4);

#endif /* SAMOS_FORMAT_H */
