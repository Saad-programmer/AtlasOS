/*
 * SAMOS / AtlasOS — tests/host/test_memmap.c
 * Tests du parseur de carte mémoire avec des cartes valides ET hostiles.
 * Chaque tampon est alloué à sa taille EXACTE (malloc) : sous ASan, la
 * moindre lecture au-delà de la fin fait échouer le test.
 */
#include <samos/memmap.h>
#include <samos/multiboot.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

static uint8_t g_buf[4096];
static uint32_t g_len;

static void put_entry(uint32_t size, uint64_t base, uint64_t len, uint32_t type)
{
    memcpy(g_buf + g_len, &size, 4);
    memcpy(g_buf + g_len + 4, &base, 8);
    memcpy(g_buf + g_len + 12, &len, 8);
    memcpy(g_buf + g_len + 20, &type, 4);
    g_len += 4 + size;   /* size peut dépasser 20 : octets supplémentaires ignorés */
}

static unsigned parse_exact(struct memmap *m, uint32_t len)
{
    uint8_t *copy = malloc(len ? len : 1);
    memcpy(copy, g_buf, len);
    unsigned n = memmap_parse(m, copy, len);
    free(copy);
    return n;
}

int main(void)
{
    static struct memmap m;

    /* 1. Carte typique (QEMU 128 MiB), volontairement NON triée */
    g_len = 0;
    put_entry(20, 0x100000, 0x7EE0000, MULTIBOOT_MEMORY_AVAILABLE);
    put_entry(20, 0x0, 0x9FC00, MULTIBOOT_MEMORY_AVAILABLE);
    put_entry(20, 0x9FC00, 0x400, MULTIBOOT_MEMORY_RESERVED);
    put_entry(20, 0xF0000, 0x10000, MULTIBOOT_MEMORY_RESERVED);
    put_entry(20, 0xFFFC0000, 0x40000, MULTIBOOT_MEMORY_RESERVED);
    CHECK(parse_exact(&m, g_len) == 5 && !m.malformed);
    CHECK(m.r[0].base == 0 && m.r[1].base == 0x9FC00 && m.r[4].base == 0xFFFC0000);   /* trié */
    CHECK(memmap_total(&m, MULTIBOOT_MEMORY_AVAILABLE) == 0x9FC00 + 0x7EE0000);

    /* 2. Élément plus grand que 20 octets (autorisé) : le suivant est trouvé via size */
    g_len = 0;
    put_entry(28, 0x1000, 0x1000, MULTIBOOT_MEMORY_AVAILABLE);
    put_entry(20, 0x5000, 0x1000, MULTIBOOT_MEMORY_RESERVED);
    CHECK(parse_exact(&m, g_len) == 2 && !m.malformed && m.r[1].base == 0x5000);

    /* 3. size < 20 : arrêt, aucune lecture hasardeuse */
    g_len = 0;
    put_entry(20, 0x1000, 0x1000, MULTIBOOT_MEMORY_AVAILABLE);
    put_entry(8, 0x2000, 0x1000, MULTIBOOT_MEMORY_AVAILABLE);
    CHECK(parse_exact(&m, g_len) == 1 && m.malformed);

    /* 4. Dernier élément tronqué (longueur totale coupée au milieu) */
    g_len = 0;
    put_entry(20, 0x1000, 0x1000, MULTIBOOT_MEMORY_AVAILABLE);
    put_entry(20, 0x2000, 0x1000, MULTIBOOT_MEMORY_AVAILABLE);
    CHECK(parse_exact(&m, g_len - 7) == 1 && m.malformed);

    /* 5. size gigantesque (0xFFFFFFFF) : pas de débordement de off + size */
    g_len = 0;
    put_entry(20, 0x1000, 0x1000, MULTIBOOT_MEMORY_AVAILABLE);
    { uint32_t huge = 0xFFFFFFFFu; memcpy(g_buf + g_len, &huge, 4); }
    CHECK(parse_exact(&m, g_len + 24) == 1 && m.malformed);

    /* 6. Longueur nulle et débordement 64 bits ignorés ; régions > 4 GiB conservées */
    g_len = 0;
    put_entry(20, 0x1000, 0, MULTIBOOT_MEMORY_AVAILABLE);
    put_entry(20, 0xFFFFFFFFFFFFF000ull, 0x2000, MULTIBOOT_MEMORY_AVAILABLE);
    put_entry(20, 0x100000000ull, 0x40000000, MULTIBOOT_MEMORY_AVAILABLE);
    put_entry(20, 0xFFFFFFFFFFFFF000ull, 0x1000, MULTIBOOT_MEMORY_RESERVED);   /* fin exacte : valide */
    CHECK(parse_exact(&m, g_len) == 2);
    CHECK(m.skipped_empty == 1 && m.skipped_overflow == 1);

    /* 7. Plus de MEMMAP_MAX éléments : surplus compté, pas d'écriture hors tableau */
    g_len = 0;
    for (int i = 0; i < MEMMAP_MAX + 10; i++) put_entry(20, (uint64_t)i << 20, 0x1000, 1);
    CHECK(parse_exact(&m, g_len) == MEMMAP_MAX && m.dropped_full == 10);

    /* 8. Tampons vides ou minuscules */
    CHECK(parse_exact(&m, 0) == 0 && !m.malformed);
    g_len = 0; put_entry(20, 0x1000, 0x1000, 1);
    CHECK(parse_exact(&m, 3) == 0 && m.malformed);

    /* 9. Carte minimale depuis mem_lower/mem_upper */
    CHECK(memmap_from_basic(&m, 639, 130048) == 2);
    CHECK(m.r[0].length == 639u * 1024 && m.r[1].base == 0x100000);

    /* 10. Fuzzing : 20 000 tampons aléatoires de tailles variées */
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int it = 0; it < 20000; it++) {
        uint32_t len = 0;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        len = (uint32_t)(x % 400);
        for (uint32_t i = 0; i < len; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            g_buf[i] = (uint8_t)x;
            if ((x & 0xF00) == 0 && i % 24 == 0) g_buf[i] = 20;   /* quelques size plausibles */
        }
        unsigned n = parse_exact(&m, len);
        for (unsigned k = 1; k < n; k++) {
            if (m.r[k - 1].base > m.r[k].base) { CHECK(0); break; }   /* toujours trié */
        }
        if (n > MEMMAP_MAX) { CHECK(0); break; }
    }
    CHECK(1);
    return CHECK_REPORT("host memmap (incl. 20000 fuzzed maps)");
}
