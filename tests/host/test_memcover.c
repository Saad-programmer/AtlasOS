/*
 * SAMOS / AtlasOS — tests/host/test_memcover.c        (K0.7-E820-FIX)
 * Cartes E820 synthétiques : chaque cas est construit en octets, analysé par
 * le VRAI memmap_parse (kernel/mm/memmap.c), puis jugé par memcover.c, le code
 * utilisé par l'autotest test_memmap. Chaque ligne « [CASE] » donne le
 * résultat attendu et le résultat observé des deux plages Multiboot.
 */
#include <samos/memmap.h>
#include <samos/multiboot.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "../../kernel/tests/memcover.h"


static uint8_t g_buf[2048];
static uint32_t g_len;

static void reset(void) { g_len = 0; }

static void put(uint64_t base, uint64_t len, uint32_t type)
{
    uint32_t size = 20;
    memcpy(g_buf + g_len, &size, 4);
    memcpy(g_buf + g_len + 4, &base, 8);
    memcpy(g_buf + g_len + 12, &len, 8);
    memcpy(g_buf + g_len + 20, &type, 4);
    g_len += 24;
}

#define AV MULTIBOOT_MEMORY_AVAILABLE
#define RS MULTIBOOT_MEMORY_RESERVED
#define MIB 0x100000ull

/* Juge la carte courante pour (mem_lower, mem_upper) en KiB et compare. */
static void judge(const char *name, uint32_t lower_kib, uint32_t upper_kib, bool want_lower, bool want_upper)
{
    struct memmap m;
    memmap_parse(&m, g_buf, g_len);
    bool lo = memcover_range_usable(&m, 0, (uint64_t)lower_kib * 1024u);
    bool up = memcover_range_usable(&m, MIB, (uint64_t)upper_kib * 1024u);
    printf("[CASE] %-58s lower: want %-4s got %-4s | upper: want %-4s got %-4s %s\n", name,
           want_lower ? "PASS" : "FAIL", lo ? "PASS" : "FAIL", want_upper ? "PASS" : "FAIL", up ? "PASS" : "FAIL",
           (lo == want_lower && up == want_upper) ? "ok" : "MISMATCH");
    CHECK(lo == want_lower);
    CHECK(up == want_upper);
}

/* Disposition QEMU mesurée (0 - EBDA - ROM - RAM haute - réservé haut). */
static void qemu_layout(uint64_t upper_end, bool above4g)
{
    reset();
    put(0, 0x9fc00, AV);
    put(0x9fc00, 0x400, RS);
    put(0xf0000, 0x10000, RS);
    put(MIB, upper_end - MIB, AV);
    put(upper_end, 0x20000, RS);
    put(0xfffc0000ull, 0x40000, RS);
    if (above4g) {
        put(0x100000000ull, 0x40000000ull, AV);
    }
}

int main(void)
{
    /* 1-3. Cartes saines (relevés QEMU). */
    qemu_layout(0xfe0000, false);
    judge("sane 16 MiB", 639, 15232, true, true);
    qemu_layout(0x7fe0000, false);
    judge("sane 128 MiB", 639, 129920, true, true);
    qemu_layout(0xbffe0000ull, true);
    judge("sane 4 GiB (memory above 4 GiB, not in mem_upper)", 639, 3144576, true, true);

    /* 4. Région basse manquante. */
    reset(); put(MIB, 0xee0000, AV);
    judge("low region missing", 639, 15232, false, true);
    /* 5. ... compensée par une région haute (au-delà de 4 GiB). */
    reset(); put(MIB, 0xee0000, AV); put(0x100000000ull, 0x100000, AV);
    judge("low region missing, compensated above 4 GiB (old F2)", 639, 15232, false, true);
    /* 6. Région haute dupliquée : sans perte, rien à reprocher ; avec perte de
     *    la région basse, la duplication ne doit RIEN compenser. */
    reset(); put(0, 0x9fc00, AV); put(MIB, 0xee0000, AV); put(MIB, 0xee0000, AV);
    judge("upper region duplicated (no coverage lost)", 639, 15232, true, true);
    reset(); put(MIB, 0xee0000, AV); put(MIB, 0xee0000, AV);
    judge("upper duplicated, low missing (old F2)", 639, 15232, false, true);
    /* 7. Trou d'une page dans la plage haute. */
    reset(); put(0, 0x9fc00, AV); put(MIB, 0x700000, AV); put(0x801000, 0xfe0000 - 0x801000, AV);
    judge("one-page hole at 8 MiB in upper range", 639, 15232, true, false);
    /* 8. Régions contiguës dont l'union couvre la plage. */
    reset(); put(0, 0x50000, AV); put(0x50000, 0x4fc00, AV); put(MIB, 0x700000, AV); put(0x800000, 0x7e0000, AV);
    judge("contiguous regions (lower 2 parts, upper 2 parts)", 639, 15232, true, true);
    /* 9. Régions chevauchantes. */
    reset(); put(0, 0x9fc00, AV); put(MIB, 0x900000, AV); put(0x800000, 0x7e0000, AV);
    judge("overlapping available regions", 639, 15232, true, true);
    /* 10. Région de longueur nulle : supprimée par memmap_parse ; et, carte
     *     construite à la main, jamais comptée comme couverture. */
    reset(); put(0, 0x9fc00, AV); put(MIB, 0, AV); put(MIB, 0xee0000, AV);
    judge("zero-length entry in E820 (dropped by parser)", 639, 15232, true, true);
    {
        struct memmap z = { 0 };
        z.r[0] = (struct mem_region){ MIB, 0, AV };
        z.r[1] = (struct mem_region){ MIB, 0x1000, AV };
        z.count = 2;
        bool zero_not_coverage = !memcover_range_usable(&z, MIB, 0x2000) && memcover_range_usable(&z, MIB, 0x1000);
        printf("[CASE] %-58s want PASS got %s\n", "zero-length region never counts (hand-built map)",
               zero_not_coverage ? "PASS" : "FAIL");
        CHECK(zero_not_coverage);
    }
    /* 11. Région disponible finissant exactement à 2^64 (old F1). */
    reset(); put(0, 0x9fc00, AV); put(MIB, 0ull - MIB, AV);
    judge("available region ending exactly at 2^64 (old F1)", 639, 15232, true, true);
    {
        struct memmap f;
        memmap_parse(&f, g_buf, g_len);
        bool f1_region_ending_at_2_64_covers = memcover_range_usable(&f, MIB, 0xee0000);
        CHECK(f1_region_ending_at_2_64_covers);
    }
    /* 12. Entrée réservée chevauchant une entrée disponible : la réservée
     *     l'emporte (pmm_core_init). Dans la plage => refus ; hors plage => sans effet. */
    reset(); put(0, 0x9fc00, AV); put(MIB, 0xee0000, AV); put(0x800000, 0x1000, RS);
    judge("reserved page inside available upper range", 639, 15232, true, false);
    reset(); put(0, 0x9fc00, AV); put(0x9f000, 0x1000, RS); put(MIB, 0xee0000, AV);
    judge("reserved page overlapping end of lower range", 639, 15232, false, true);
    reset(); put(0, 0x9fc00, AV); put(MIB, 0xee0000, AV); put(0xfe0000, 0x20000, RS); put(0xe0000, 0x20000, RS);
    judge("reserved regions adjacent or outside the ranges", 639, 15232, true, true);
    /* 13. Absence de carte E820 indépendante : comparaison interdite (SKIP). */
    bool skip_no_mmap = !memcover_sources_independent(MBI_FLAG_MEM);
    bool skip_no_mem = !memcover_sources_independent(MBI_FLAG_MMAP);
    bool check_both = memcover_sources_independent(MBI_FLAG_MEM | MBI_FLAG_MMAP | MBI_FLAG_CMDLINE);
    printf("[CASE] %-58s no-mmap=%s no-mem=%s both=%s\n", "source independence (no E820 => SKIP)",
           skip_no_mmap ? "SKIP" : "CHECK", skip_no_mem ? "SKIP" : "CHECK", check_both ? "CHECK" : "SKIP");
    CHECK(skip_no_mmap && skip_no_mem && check_both);
    /* Bornes : intervalle vide ; intervalle dépassant 2^64. */
    reset(); put(0, 0x9fc00, AV);
    {
        struct memmap e;
        memmap_parse(&e, g_buf, g_len);
        CHECK(memcover_range_usable(&e, 0x123456, 0));
        CHECK(!memcover_range_usable(&e, 0xfffffffffffff000ull, 0x2000));
    }
    return CHECK_REPORT("memcover");
}
