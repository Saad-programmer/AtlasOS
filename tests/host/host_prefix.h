/*
 * SAMOS / AtlasOS — tests/host/host_prefix.h
 * Inclus de force (-include) lors de la compilation HÉBERGÉE du code noyau.
 * Renomme les fonctions du noyau qui portent des noms de la libc, afin de
 * tester NOTRE implémentation et non celle de l'hôte (et d'éviter les conflits
 * avec les intercepteurs d'AddressSanitizer).
 */
#ifndef SAMOS_HOST_PREFIX_H
#define SAMOS_HOST_PREFIX_H
#define memset  samos_memset
#define memcpy  samos_memcpy
#define memmove samos_memmove
#define memcmp  samos_memcmp
#define strlen  samos_strlen
#define strnlen samos_strnlen
#define strcmp  samos_strcmp
#define strncmp samos_strncmp
#define strlcpy samos_strlcpy
#define SAMOS_HOST_TEST 1
#endif
