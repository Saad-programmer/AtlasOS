/*
 * SAMOS / AtlasOS — kernel/mm/memmap.c
 * Analyse défensive de la carte mémoire. Voir memmap.h et Details.md §33.
 */
#include <samos/memmap.h>
#include <samos/multiboot.h>
#include <samos/string.h>

#define ENTRY_MIN_SIZE 20u   /* base (8) + length (8) + type (4) : champ size exclu */

static void insert_sorted(struct memmap *m, const struct mem_region *e)
{
    /* Tri par insertion : n <= 64, O(n^2) négligeable, stable, sans allocation. */
    unsigned i = m->count;
    while (i > 0 && m->r[i - 1].base > e->base) {
        m->r[i] = m->r[i - 1];
        i--;
    }
    m->r[i] = *e;
    m->count++;
}

static void add_region(struct memmap *m, uint64_t base, uint64_t length, uint32_t type)
{
    if (length == 0) {
        m->skipped_empty++;
        return;
    }
    if (length - 1u > UINT64_MAX - base) {   /* base + length - 1 > 2^64 - 1 */
        m->skipped_overflow++;
        return;
    }
    if (m->count >= MEMMAP_MAX) {
        m->dropped_full++;
        return;
    }
    struct mem_region e = { base, length, type };
    insert_sorted(m, &e);
}

unsigned memmap_parse(struct memmap *m, const void *buf, uint32_t len)
{
    memset(m, 0, sizeof(*m));
    const uint8_t *p = buf;
    uint32_t off = 0;

    while (len - off >= 4u) {                 /* place pour le champ size ? */
        uint32_t size;
        memcpy(&size, p + off, 4);           /* memcpy : lecture non alignée sûre */
        if (size < ENTRY_MIN_SIZE || size > len - off - 4u) {
            /* Élément trop petit ou dépassant le tampon : on ne peut plus
             * trouver l'élément suivant de façon fiable => arrêt. */
            m->malformed = true;
            break;
        }
        uint64_t base, length;
        uint32_t type;
        memcpy(&base, p + off + 4, 8);
        memcpy(&length, p + off + 12, 8);
        memcpy(&type, p + off + 20, 4);
        add_region(m, base, length, type);
        off += 4u + size;                    /* pas de débordement : size <= len - off - 4 */
    }
    if (off != len && !m->malformed) {
        m->malformed = true;                 /* octets résiduels (< 4) en fin de tampon */
    }
    return m->count;
}

unsigned memmap_from_basic(struct memmap *m, uint32_t lower_kib, uint32_t upper_kib)
{
    memset(m, 0, sizeof(*m));
    add_region(m, 0, (uint64_t)lower_kib * 1024u, MULTIBOOT_MEMORY_AVAILABLE);
    add_region(m, 0x100000u, (uint64_t)upper_kib * 1024u, MULTIBOOT_MEMORY_AVAILABLE);
    return m->count;
}

uint64_t memmap_total(const struct memmap *m, uint32_t type)
{
    uint64_t t = 0;
    for (unsigned i = 0; i < m->count; i++) {
        if (m->r[i].type == type) {
            t += m->r[i].length;
        }
    }
    return t;
}

const char *memmap_type_name(uint32_t type)
{
    switch (type) {
    case MULTIBOOT_MEMORY_AVAILABLE:        return "available";
    case MULTIBOOT_MEMORY_RESERVED:         return "reserved";
    case MULTIBOOT_MEMORY_ACPI_RECLAIMABLE: return "ACPI reclaimable";
    case MULTIBOOT_MEMORY_NVS:              return "ACPI NVS";
    case MULTIBOOT_MEMORY_BADRAM:           return "bad RAM";
    default:                                return "unknown";
    }
}
