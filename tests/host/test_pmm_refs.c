/*
 * SAMOS / AtlasOS — tests/host/test_pmm_refs.c
 * K0.7-M2.4a : opérations de références du cœur PMM (pmm_core_ref/unref).
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
    CHECK(pmm_core_reserve_as(&p, 0x100000, 0x2000, FRAME_KERNEL, "k") == SAMOS_OK);

    uint32_t a = pmm_core_alloc(&p);
    CHECK(a != PMM_NO_FRAME && pmm_core_refs(&p, a) == 0);

    /* +1 / -1 exactement */
    CHECK(pmm_core_ref(&p, a) == SAMOS_OK && pmm_core_refs(&p, a) == 1);
    CHECK(pmm_core_ref(&p, a) == SAMOS_OK && pmm_core_refs(&p, a) == 2);
    CHECK(pmm_core_unref(&p, a) == SAMOS_OK && pmm_core_refs(&p, a) == 1);
    CHECK(pmm_core_type(&p, a) == FRAME_ALLOCATED);
    CHECK(pmm_core_unref(&p, a) == SAMOS_OK && pmm_core_refs(&p, a) == 0);

    /* Sous-dépassement refusé, descripteur intact */
    frame_desc_t before = meta[a];
    CHECK(pmm_core_unref(&p, a) == SAMOS_ERANGE && meta[a] == before);

    /* Débordement : jusqu'à 4095 accepté, 4096e refusé, descripteur intact */
    uint32_t ok = 0;
    for (uint32_t i = 0; i < FRAME_REFS_MAX; i++) ok += (pmm_core_ref(&p, a) == SAMOS_OK);
    CHECK(ok == FRAME_REFS_MAX && pmm_core_refs(&p, a) == FRAME_REFS_MAX);
    before = meta[a];
    CHECK(pmm_core_ref(&p, a) == SAMOS_ERANGE && meta[a] == before);
    CHECK(pmm_core_type(&p, a) == FRAME_ALLOCATED);        /* le type n'est pas corrompu */
    for (uint32_t i = 0; i < FRAME_REFS_MAX; i++) (void)pmm_core_unref(&p, a);
    CHECK(pmm_core_refs(&p, a) == 0);

    /* Types refusés, descripteurs intacts */
    uint32_t kf = 0x100, rf = 0, ff = 0x200;
    frame_desc_t dk = meta[kf], dr = meta[rf], df = meta[ff];
    CHECK(pmm_core_type(&p, ff) == FRAME_FREE);
    CHECK(pmm_core_ref(&p, kf) == SAMOS_EPERM && meta[kf] == dk);
    CHECK(pmm_core_ref(&p, rf) == SAMOS_EPERM && meta[rf] == dr);
    CHECK(pmm_core_ref(&p, ff) == SAMOS_EPERM && meta[ff] == df);
    CHECK(pmm_core_unref(&p, kf) == SAMOS_EPERM && meta[kf] == dk);
    uint32_t t = pmm_core_alloc_as(&p, FRAME_PAGETABLE);
    frame_desc_t dt = meta[t];
    CHECK(pmm_core_ref(&p, t) == SAMOS_EPERM && meta[t] == dt);
    CHECK(pmm_core_ref(&p, 5000) == SAMOS_EPERM);          /* hors limites */
    CHECK(pmm_core_refs(&p, 5000) == 0);

    /* Compteur d'erreurs : 1 + 1 + 5 refus de type + 1 hors limites */
    CHECK(p.ref_errors == 8);

    /* Comportement existant inchangé : free_count, libération, cohérence */
    uint32_t fc = p.free_count;
    CHECK(pmm_core_free(&p, a) == SAMOS_OK && p.free_count == fc + 1);
    CHECK(pmm_core_check(&p) == 0);
    return CHECK_REPORT("host pmm refs");
}
