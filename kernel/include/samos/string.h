/*
 * SAMOS / AtlasOS — kernel/include/samos/string.h
 * Fonctions mémoire/chaînes du noyau, implémentées dans kernel/lib/string.c.
 *
 * memset/memcpy/memmove/memcmp portent leurs noms standard car le compilateur
 * est autorisé à générer lui-même des appels à ces fonctions (copie de
 * structures, initialisation de tableaux), même en mode freestanding.
 */
#ifndef SAMOS_STRING_H
#define SAMOS_STRING_H

#include <stddef.h>

void  *memset(void *dst, int value, size_t n);
void  *memcpy(void *restrict dst, const void *restrict src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
/* Longueur bornée : ne lit jamais plus de `max` octets. À utiliser pour toute
 * chaîne provenant d'une source non fiable (chargeur, matériel). */
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
/* Copie bornée, toujours terminée par '\0' si cap > 0. Retourne strlen(src). */
size_t strlcpy(char *dst, const char *src, size_t cap);

#endif /* SAMOS_STRING_H */
