/*
 * SAMOS / AtlasOS — tests/host/test_pmm_busy.c
 * K0.7-M2.4b : un cadre ALLOCATED référencé ne peut pas être libéré, et
 * pmm_core_check() accepte des références valides (et seulement celles-là).
 */
#include <samos/memmap.h>
#include <samos/multiboot.h>
#include <samos/pmm.h>
#include <string.h>
#include "check.h"

int main(void)
{
    static struct memmap m;
    static struct pmm p;
    static frame_desc_t meta[1024];
    memset(&m, 0, sizeof m);
    m.r[0] = (struct mem_region){ 0x0, 0x400000, MULTIBOOT_MEMORY_AVAILABLE };
    m.count = 1;
    CHECK(pmm_core_init(&p, meta, 1024, 1024, &m) == SAMOS_OK);

    /* 0 référence : libération possible */
    uint32_t a = pmm_core_alloc(&p);
    CHECK(pmm_core_free(&p, a) == SAMOS_OK);

    /* 1 référence : refusée, rien ne change */
    uint32_t b = pmm_core_alloc(&p);
    CHECK(pmm_core_ref(&p, b) == SAMOS_OK);
    frame_desc_t d = meta[b];
    uint32_t fc = p.free_count, inv = p.invalid_frees, dbl = p.double_frees;
    CHECK(pmm_core_free(&p, b) == SAMOS_EBUSY);
    CHECK(meta[b] == d && p.free_count == fc && p.busy_frees == 1);
    CHECK(p.invalid_frees == inv && p.double_frees == dbl);   /* compteur dédié seulement */
    CHECK(pmm_core_check(&p) == 0);                           /* référence valide acceptée */

    /* plusieurs références : refusée */
    CHECK(pmm_core_ref(&p, b) == SAMOS_OK && pmm_core_refs(&p, b) == 2);
    d = meta[b];
    CHECK(pmm_core_free(&p, b) == SAMOS_EBUSY && meta[b] == d && p.busy_frees == 2);
    CHECK(pmm_core_check(&p) == 0);

    /* une seule retirée : toujours refusée ; toutes retirées : possible */
    CHECK(pmm_core_unref(&p, b) == SAMOS_OK);
    CHECK(pmm_core_free(&p, b) == SAMOS_EBUSY);
    CHECK(pmm_core_unref(&p, b) == SAMOS_OK);
    CHECK(pmm_core_free(&p, b) == SAMOS_OK && pmm_core_type(&p, b) == FRAME_FREE);
    CHECK(p.free_count == fc + 1);

    /* F2 conservé : références sur un autre type => anomalie */
    uint32_t t = pmm_core_alloc_as(&p, FRAME_PAGETABLE);
    meta[t] = frame_desc_make(FRAME_PAGETABLE, 1);
    CHECK(pmm_core_check(&p) != 0);
    meta[t] = frame_desc_make(FRAME_PAGETABLE, 0);
    meta[0x300] = frame_desc_make(FRAME_FREE, 2);
    CHECK(pmm_core_check(&p) != 0);
    meta[0x300] = frame_desc_make(FRAME_FREE, 0);
    CHECK(pmm_core_check(&p) == 0);
    CHECK(pmm_core_free(&p, t) == SAMOS_EINVAL);              /* E4 toujours refusé */
    return CHECK_REPORT("host pmm busy");
}
