/*
 * SAMOS / AtlasOS — tests/host/test_pmm.c
 * Tests du cœur de l'allocateur de cadres : invariants I1-I4 (pmm.h),
 * chevauchements de régions, épuisement, comparaison avec un modèle.
 */
#include <samos/memmap.h>
#include <samos/multiboot.h>
#include <samos/pmm.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

static int popcount_free(const struct pmm *p)
{
    int n = 0;
    for (uint32_t f = 0; f < p->nframes; f++) n += !pmm_core_is_used(p, f);
    return n;
}

int main(void)
{
    static struct memmap m;
    static struct pmm p;
    static uint32_t bitmap[1024];          /* 32768 cadres = 128 MiB */

    /* Carte : [0, 0x9FC00) dispo, [0x100000, 0x800000) dispo (7 MiB),
     * [0x400000, 0x401800) réservée DANS la région dispo (chevauchement),
     * région dispo NON alignée [0x900800, 0x903800) : seuls 0x901000..0x903000 utilisables. */
    memset(&m, 0, sizeof m);
    m.r[0] = (struct mem_region){ 0x0, 0x9FC00, MULTIBOOT_MEMORY_AVAILABLE };
    m.r[1] = (struct mem_region){ 0x100000, 0x700000, MULTIBOOT_MEMORY_AVAILABLE };
    m.r[2] = (struct mem_region){ 0x400000, 0x1800, MULTIBOOT_MEMORY_RESERVED };
    m.r[3] = (struct mem_region){ 0x900800, 0x3000, MULTIBOOT_MEMORY_AVAILABLE };
    m.count = 4;

    CHECK(pmm_core_init(&p, bitmap, 1024, 1u << 20, &m) == SAMOS_OK);
    CHECK(p.nframes == 0x903);                                       /* fin de la plus haute région */
    CHECK(pmm_core_is_used(&p, 0) && pmm_core_is_reserved(&p, 0));  /* cadre nul */
    CHECK(!pmm_core_is_used(&p, 1));
    CHECK(pmm_core_is_used(&p, 0x9F));                               /* 0x9F000-0x9FFFF : partiel => occupé */
    CHECK(pmm_core_is_used(&p, 0x400) && pmm_core_is_used(&p, 0x401));  /* réservé l'emporte, arrondi extérieur */
    CHECK(!pmm_core_is_used(&p, 0x402));
    CHECK(pmm_core_is_used(&p, 0x900) && !pmm_core_is_used(&p, 0x901) && !pmm_core_is_used(&p, 0x902));
    CHECK((int)p.free_count == popcount_free(&p));                  /* I4 */
    uint32_t expected_free = (0x9F - 1) + (0x800 - 0x100 - 2) + 2;
    CHECK(p.free_count == expected_free);

    /* Réservation du « noyau » : [0x100000, 0x112345) -> cadres 0x100..0x112 */
    CHECK(pmm_core_reserve(&p, 0x100000, 0x12345, "kernel") == SAMOS_OK);
    CHECK(pmm_core_is_used(&p, 0x112) && !pmm_core_is_used(&p, 0x113));

    /* I1 : libérer un cadre réservé ou hors limites est refusé */
    CHECK(pmm_core_free(&p, 0x105) == SAMOS_EINVAL);
    CHECK(pmm_core_free(&p, 0x400) == SAMOS_EINVAL);
    CHECK(pmm_core_free(&p, 0) == SAMOS_EINVAL);
    CHECK(pmm_core_free(&p, 0x100000) == SAMOS_EINVAL);
    CHECK(p.invalid_frees == 4);

    /* Allocation / libération / double libération */
    uint32_t a = pmm_core_alloc(&p);
    CHECK(a != PMM_NO_FRAME && !pmm_core_is_reserved(&p, a) && pmm_core_is_used(&p, a));
    CHECK(pmm_core_free(&p, a) == SAMOS_OK);
    CHECK(pmm_core_free(&p, a) == SAMOS_ENOENT && p.double_frees == 1);   /* I3 */

    /* Épuisement total : tous les cadres alloués sont distincts et non réservés */
    uint32_t before = p.free_count;
    static uint8_t seen[0x903];
    memset(seen, 0, sizeof seen);
    uint32_t n = 0, f;
    int bad = 0;
    while ((f = pmm_core_alloc(&p)) != PMM_NO_FRAME) {
        if (f >= p.nframes || seen[f] || pmm_core_is_reserved(&p, f)) bad = 1;  /* I1, I2 */
        seen[f] = 1;
        n++;
    }
    CHECK(!bad && n == before && p.free_count == 0);
    CHECK(pmm_core_alloc(&p) == PMM_NO_FRAME);
    for (uint32_t k = 0; k < 0x903; k++) if (seen[k]) CHECK(pmm_core_free(&p, k) == SAMOS_OK);
    CHECK(p.free_count == before && (int)p.free_count == popcount_free(&p));

    /* Modèle aléatoire : 200 000 alloc/free, invariants vérifiés en continu */
    static uint32_t held[0x903];
    uint32_t nheld = 0;
    uint64_t x = 0xD1B54A32D192ED03ull;
    int mismatch = 0;
    for (int it = 0; it < 200000 && !mismatch; it++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        if ((x & 1) && nheld > 0) {
            uint32_t i = (uint32_t)(x >> 8) % nheld;
            if (pmm_core_free(&p, held[i]) != SAMOS_OK) mismatch = 1;
            held[i] = held[--nheld];
        } else {
            uint32_t g = pmm_core_alloc(&p);
            if (g == PMM_NO_FRAME) { if (p.free_count != 0) mismatch = 1; continue; }
            if (pmm_core_is_reserved(&p, g)) mismatch = 1;
            held[nheld++] = g;
        }
        if (p.free_count + nheld != before) mismatch = 1;
    }
    CHECK(!mismatch);
    CHECK((int)p.free_count == popcount_free(&p));

    /* Arguments invalides */
    CHECK(pmm_core_init(NULL, bitmap, 1024, 100, &m) == SAMOS_EINVAL);
    CHECK(pmm_core_init(&p, bitmap, 0, 100, &m) == SAMOS_EINVAL);
    return CHECK_REPORT("host pmm_core");
}
