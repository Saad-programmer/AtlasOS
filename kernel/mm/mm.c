/*
 * SAMOS / AtlasOS — kernel/mm/mm.c
 * Orchestration de l'initialisation mémoire. Voir Details.md §33-36.
 */
#include <arch/paging.h>
#include <samos/bootinfo.h>
#include <samos/log.h>
#include <samos/mm.h>
#include <samos/multiboot.h>
#include <samos/pmm.h>

static struct memmap g_memmap;

const struct memmap *mm_memmap(void)
{
    return &g_memmap;
}

samos_err_t mm_init(void)
{
    const struct bootinfo *bi = bootinfo_get();

    /* 1. Carte mémoire : détaillée (E820 via Multiboot) si disponible et
     *    validée par bootinfo, sinon minimale (mem_lower/mem_upper). */
    if (bi->flags & MBI_FLAG_MMAP) {
        memmap_parse(&g_memmap, (const void *)(uintptr_t)bi->mmap_addr, bi->mmap_length);
    } else if (bi->flags & MBI_FLAG_MEM) {
        klog(LOG_WARN, "mm: no detailed memory map, using mem_lower/mem_upper\n");
        memmap_from_basic(&g_memmap, bi->mem_lower_kib, bi->mem_upper_kib);
    } else {
        return SAMOS_ENOENT;
    }
    if (g_memmap.malformed || g_memmap.skipped_overflow || g_memmap.dropped_full) {
        klog(LOG_WARN, "mm: memory map anomalies: malformed=%d overflow=%u dropped=%u\n",
             (int)g_memmap.malformed, g_memmap.skipped_overflow, g_memmap.dropped_full);
    }
    for (unsigned i = 0; i < g_memmap.count; i++) {
        const struct mem_region *r = &g_memmap.r[i];
        klog(LOG_INFO, "mm: 0x%09llx-0x%09llx %s\n", (unsigned long long)r->base,
             (unsigned long long)(r->base + r->length - 1u), memmap_type_name(r->type));
    }
    uint64_t avail = memmap_total(&g_memmap, MULTIBOOT_MEMORY_AVAILABLE);
    klog(LOG_INFO, "mm: %u regions, %llu KiB available\n", g_memmap.count,
         (unsigned long long)(avail >> 10));

    /* 2. Allocateur physique, borné à la zone qui sera projetée en identité. */
    samos_err_t e = pmm_init(&g_memmap, PAGING_IDMAP_LIMIT);
    if (e != SAMOS_OK) {
        return e;
    }
    pmm_print();
    uint64_t managed = (uint64_t)pmm_state()->nframes << PAGE_SHIFT;
    uint64_t highest = 0;
    for (unsigned i = 0; i < g_memmap.count; i++) {
        const struct mem_region *r = &g_memmap.r[i];
        if (r->type == MULTIBOOT_MEMORY_AVAILABLE && r->base + r->length > highest) {
            highest = r->base + r->length;
        }
    }
    if (highest > managed) {
        /* Limite assumée de K0 : seule la RAM projetée en identité (<= 1 GiB)
         * est gérée ; le reste est ignoré, jamais utilisé à tort. */
        klog(LOG_WARN, "mm: RAM above %u MiB is not managed in K0 (identity-map limit), "
             "%llu MiB ignored\n", (unsigned)(managed >> 20),
             (unsigned long long)((highest - managed) >> 20));
    }

    /* 3. Pagination : identité sur toute la mémoire gérée. */
    uint32_t idmap_top = pmm_state()->nframes << PAGE_SHIFT;
    e = paging_init(idmap_top);
    if (e != SAMOS_OK) {
        return e;
    }
    klog(LOG_INFO, "paging: enabled (CR0.PG|WP), identity map 0x00001000-%p, %u page tables, "
         "null page and stack guard unmapped, .text/.rodata read-only\n",
         (void *)(uintptr_t)idmap_top, paging_table_count());
    pmm_print();
    return SAMOS_OK;
}
