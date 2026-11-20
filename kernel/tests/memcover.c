/*
 * SAMOS / AtlasOS — kernel/tests/memcover.c          (K0.7-E820-FIX, tests)
 * Voir memcover.h. Bornes inclusives partout : aucune addition ne peut
 * déborder (voir chaque calcul).
 */
#include "memcover.h"

#include <samos/multiboot.h>

/* Dernier octet d'une région de longueur non nulle. memmap.c garantit
 * base + length - 1 <= 2^64 - 1 (add_region refuse le contraire) ; on le
 * revérifie ici, puisque ce module ne fait pas confiance à son entrée. */
static bool region_last(const struct mem_region *r, uint64_t *last)
{
    if (r->length == 0 || r->length - 1u > UINT64_MAX - r->base) {
        return false;                           /* vide, ou débordement : ignorée */
    }
    *last = r->base + (r->length - 1u);
    return true;
}

/*
 * Union des entrées DISPONIBLES couvrant [first, last] : balayage glouton.
 * À chaque étape, parmi les régions disponibles qui contiennent pos, on
 * retient celle qui va le plus loin ; s'il n'y en a aucune, pos est un trou.
 * Les doublons et les chevauchements n'ajoutent rien (aucune somme). Chaque
 * étape réussie dépasse strictement le pos précédent et la fin d'une région
 * distincte : au plus m->count étapes.
 */
bool memcover_range_covered(const struct memmap *m, uint64_t first, uint64_t last)
{
    uint64_t pos = first;
    for (unsigned step = 0; step <= m->count; step++) {
        bool found = false;
        uint64_t reach = 0;
        for (unsigned i = 0; i < m->count; i++) {
            uint64_t rl;
            if (m->r[i].type != MULTIBOOT_MEMORY_AVAILABLE || !region_last(&m->r[i], &rl)) {
                continue;
            }
            if (m->r[i].base <= pos && pos <= rl && (!found || rl > reach)) {
                found = true;
                reach = rl;
            }
        }
        if (!found) {
            return false;                       /* trou à pos */
        }
        if (reach >= last) {
            return true;
        }
        pos = reach + 1u;                       /* reach < last <= 2^64 - 1 : pas de débordement */
    }
    return false;                               /* inatteignable si la carte est finie */
}

/* Aucune entrée NON disponible (réservée, ACPI, NVS, défectueuse...) ne
 * touche [first, last] : règle de priorité de pmm_core_init. */
bool memcover_range_unreserved(const struct memmap *m, uint64_t first, uint64_t last)
{
    for (unsigned i = 0; i < m->count; i++) {
        uint64_t rl;
        if (m->r[i].type == MULTIBOOT_MEMORY_AVAILABLE || !region_last(&m->r[i], &rl)) {
            continue;
        }
        if (m->r[i].base <= last && first <= rl) {
            return false;
        }
    }
    return true;
}

bool memcover_range_usable(const struct memmap *m, uint64_t first, uint64_t len)
{
    if (len == 0) {
        return true;
    }
    if (len - 1u > UINT64_MAX - first) {
        return false;                           /* l'intervalle lui-même dépasserait 2^64 */
    }
    uint64_t last = first + (len - 1u);
    return memcover_range_covered(m, first, last) && memcover_range_unreserved(m, first, last);
}

bool memcover_sources_independent(uint32_t mbi_flags)
{
    return (mbi_flags & MBI_FLAG_MEM) && (mbi_flags & MBI_FLAG_MMAP);
}
