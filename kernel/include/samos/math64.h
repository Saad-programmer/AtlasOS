/*
 * SAMOS / AtlasOS — kernel/include/samos/math64.h
 * Arithmétique 64 bits sans libgcc (K0.3).
 *
 * Sur i386, `uint64_t / uint32_t` en C produit un appel à __udivdi3 (libgcc).
 * SAMOS ne lie pas libgcc : on implémente la division par décalages et
 * soustractions (division longue binaire), en n'utilisant que des opérations
 * que le compilateur traduit en instructions natives (shl/shld, cmp/sbb,
 * sub/sbb). Le code est portable et testé sur l'hôte contre la division native.
 */
#ifndef SAMOS_MATH64_H
#define SAMOS_MATH64_H

#include <stdint.h>

/* Retourne n / d et écrit n % d dans *rem (si rem != NULL). d doit être != 0 ;
 * si d == 0, retourne UINT64_MAX et *rem = 0 (au lieu d'un comportement indéfini). */
uint64_t u64_divmod_u32(uint64_t n, uint32_t d, uint32_t *rem);

#endif /* SAMOS_MATH64_H */
