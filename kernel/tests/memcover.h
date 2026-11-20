/*
 * SAMOS / AtlasOS — kernel/tests/memcover.h          (K0.7-E820-FIX, tests)
 * ---------------------------------------------------------------------------
 * Couverture d'un INTERVALLE physique par la carte mémoire, selon la règle de
 * normalisation du noyau (pmm_core_init) : un octet est utilisable s'il est
 * couvert par AU MOINS une entrée disponible et par AUCUNE entrée d'un autre
 * type (« toute autre région l'emporte en cas de chevauchement »).
 *
 * Code de TEST seulement : compilé dans l'image de test (test_mm.c) et dans
 * les tests hébergés (tests/host/test_memcover.c), jamais dans l'image de
 * production. Ne modifie pas la carte.
 *
 * Arithmétique : toutes les bornes sont INCLUSIVES (premier et dernier
 * octet). Une région [base, base + length) qui finit exactement à 2^64 a pour
 * dernier octet 2^64 - 1, représentable ; base + length, lui, ne l'est pas
 * (défaut F1 de la version précédente).
 */
#ifndef SAMOS_TESTS_MEMCOVER_H
#define SAMOS_TESTS_MEMCOVER_H

#include <samos/memmap.h>
#include <stdbool.h>
#include <stdint.h>

/* [first, first + len) est-il entièrement utilisable ? len = 0 : vrai
 * (intervalle vide). Refuse si first + len dépasse 2^64. */
bool memcover_range_usable(const struct memmap *m, uint64_t first, uint64_t len);

/* Les deux parties de la décision, exposées pour les tests et le diagnostic. */
bool memcover_range_covered(const struct memmap *m, uint64_t first, uint64_t last);
bool memcover_range_unreserved(const struct memmap *m, uint64_t first, uint64_t last);

/* Comparaison avec mem_lower / mem_upper possible seulement si les deux
 * sources sont présentes : sans carte E820, mm.c construit la carte À PARTIR
 * de mem_lower / mem_upper (memmap_from_basic), la comparaison serait
 * circulaire. */
bool memcover_sources_independent(uint32_t mbi_flags);

#endif
