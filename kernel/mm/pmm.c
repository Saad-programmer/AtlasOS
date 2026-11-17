/*
 * SAMOS / AtlasOS — kernel/mm/pmm.c
 * Instance noyau de l'allocateur de cadres. Voir pmm.h et Details.md §34.
 */
#include <samos/bootinfo.h>
#include <samos/kernel.h>
#include <samos/log.h>
#include <samos/multiboot.h>
#include <samos/pmm.h>
#include <samos/string.h>

/* Bitmap statique couvrant 4 GiB : 2^20 cadres / 32 = 32768 mots = 128 KiB
 * dans .bss. Choix : aucune dépendance circulaire (l'allocateur n'a pas à
 * s'allouer lui-même), au prix de 128 KiB toujours réservés. */
#define BITMAP_WORDS (1u << 15)
static uint32_t g_bitmap[BITMAP_WORDS];
static struct pmm g_pmm;

samos_err_t pmm_init(const struct memmap *m, uint32_t limit_bytes)
{
    samos_err_t e = pmm_core_init(&g_pmm, g_bitmap, BITMAP_WORDS, limit_bytes >> PAGE_SHIFT, m);
    if (e != SAMOS_OK) {
        return e;
    }
    /* Mémoire basse (< 1 MiB) : IVT, BDA, EBDA, VGA, ROM, et en pratique les
     * structures Multiboot de QEMU. Réservée en bloc (approche prudente). */
    e = pmm_core_reserve(&g_pmm, 0, 0x100000, "low memory (<1 MiB)");
    /* Image du noyau, bitmap comprise (elle est dans .bss). */
    if (e == SAMOS_OK) {
        e = pmm_core_reserve(&g_pmm, (uintptr_t)__kernel_start,
                             (uintptr_t)__kernel_end - (uintptr_t)__kernel_start, "kernel image");
    }
    /* Structures du chargeur : un autre chargeur (GRUB) peut les placer
     * au-dessus de 1 MiB. Déjà copiées par bootinfo/memmap, mais réservées par
     * défense en profondeur. */
    const struct bootinfo *bi = bootinfo_get();
    if (e == SAMOS_OK) {
        e = pmm_core_reserve(&g_pmm, bi->mbi_phys, sizeof(struct multiboot_info), "multiboot info");
    }
    if (e == SAMOS_OK && bi->mmap_length != 0) {
        e = pmm_core_reserve(&g_pmm, bi->mmap_addr, bi->mmap_length, "multiboot mmap");
    }
    return e;
}

uint32_t pmm_alloc_frame(void)
{
    uint32_t f = pmm_core_alloc(&g_pmm);
    if (f == PMM_NO_FRAME) {
        return 0;
    }
    uint32_t phys = f << PAGE_SHIFT;
    /* Remise à zéro : un cadre libéré peut contenir des données d'un ancien
     * propriétaire (fuite d'information) ou de vieilles entrées de table de
     * pages (projection fantôme). Accès par l'identité : la mémoire gérée est
     * bornée à la zone projetée en identité (paging.c). */
    memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
    return phys;
}

samos_err_t pmm_free_frame(uint32_t phys)
{
    if ((phys & (PAGE_SIZE - 1u)) != 0) {
        g_pmm.invalid_frees++;
        return SAMOS_EINVAL;                 /* adresse non alignée : erreur de l'appelant */
    }
    return pmm_core_free(&g_pmm, phys >> PAGE_SHIFT);
}

const struct pmm *pmm_state(void)
{
    return &g_pmm;
}

void pmm_print(void)
{
    const struct pmm *p = &g_pmm;
    klog(LOG_INFO, "pmm: %u frames managed (%u KiB), %u free (%u KiB), %u reserved ranges\n",
         p->nframes, p->nframes * 4u, p->free_count, p->free_count * 4u, p->nreserved);
}
