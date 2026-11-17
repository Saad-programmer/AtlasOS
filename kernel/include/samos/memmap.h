/*
 * SAMOS / AtlasOS — kernel/include/samos/memmap.h
 * ---------------------------------------------------------------------------
 * Carte de la mémoire physique (K0.6).
 *
 * Source : la carte Multiboot (champ mmap_*), elle-même issue en général de
 * l'appel BIOS INT 15h / EAX = E820h. Elle décrit des régions physiques
 * [base, base + length) et leur type (disponible, réservée, ACPI...).
 *
 * Le chargeur et le firmware sont NON FIABLES : la carte peut contenir des
 * éléments malformés, des longueurs nulles, des chevauchements, des régions
 * au-delà de 4 GiB ou dont base + length déborde 64 bits. Le parseur est une
 * fonction PURE (tampon -> structure), testée sur l'hôte avec des cartes
 * délibérément corrompues.
 */
#ifndef SAMOS_MEMMAP_H
#define SAMOS_MEMMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEMMAP_MAX 64

struct mem_region {
    uint64_t base;
    uint64_t length;    /* > 0 ; base + length ne déborde pas (vérifié) */
    uint32_t type;      /* MULTIBOOT_MEMORY_* */
};

struct memmap {
    struct mem_region r[MEMMAP_MAX];
    unsigned count;
    unsigned skipped_empty;     /* éléments de longueur nulle ignorés */
    unsigned skipped_overflow;  /* base + length > 2^64 : ignorés */
    unsigned dropped_full;      /* éléments au-delà de MEMMAP_MAX */
    bool malformed;             /* parcours interrompu : élément invalide ou tronqué */
};

/*
 * Analyse `len` octets d'une carte au format Multiboot. Ne lit JAMAIS au-delà
 * de buf[len - 1]. Les éléments sont copiés puis triés par adresse de base.
 * Retourne le nombre de régions retenues.
 */
unsigned memmap_parse(struct memmap *m, const void *buf, uint32_t len);

/* Construit une carte minimale à partir de mem_lower/mem_upper (KiB), pour
 * un chargeur qui ne fournit pas de carte détaillée. */
unsigned memmap_from_basic(struct memmap *m, uint32_t lower_kib, uint32_t upper_kib);

uint64_t memmap_total(const struct memmap *m, uint32_t type);
const char *memmap_type_name(uint32_t type);

#endif /* SAMOS_MEMMAP_H */
