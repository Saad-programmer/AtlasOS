/*
 * SAMOS / AtlasOS — kernel/mm/pmm.c
 * Instance noyau de l'allocateur de cadres. Voir pmm.h et Details.md §34.
 */
#include <arch/paging.h>
#include <samos/bootinfo.h>
#include <samos/kernel.h>
#include <samos/log.h>
#include <samos/multiboot.h>
#include <samos/pmm.h>
#include <samos/string.h>

/* K0.7-M2.2 — Table statique des descripteurs (remplace la bitmap de K0.6).
 * Capacité : 2^18 cadres = 1 GiB, exactement la mémoire que le noyau peut
 * gérer (limite de la projection en identité, PAGING_IDMAP_LIMIT) ; taille :
 * 2^18 * 2 octets = 512 KiB dans .bss.
 * Placement : dans l'image du noyau, donc dans la plage réservée FRAME_KERNEL
 * ci-dessous : la table ne peut jamais être allouée ni libérée, et aucune
 * allocation n'est nécessaire pour initialiser l'allocateur (pas de
 * dépendance récursive). */
#define PMM_MAX_FRAMES (1u << 18)
SAMOS_STATIC_ASSERT((PAGING_IDMAP_LIMIT >> PAGE_SHIFT) <= PMM_MAX_FRAMES,
                    "la table de descripteurs doit couvrir toute la RAM projetable");
static frame_desc_t g_meta[PMM_MAX_FRAMES];
static struct pmm g_pmm;

samos_err_t pmm_init(const struct memmap *m, uint32_t limit_bytes)
{
    samos_err_t e = pmm_core_init(&g_pmm, g_meta, PMM_MAX_FRAMES, limit_bytes >> PAGE_SHIFT, m);
    if (e != SAMOS_OK) {
        return e;
    }
    /* Mémoire basse (< 1 MiB) : IVT, BDA, EBDA, VGA, ROM, et en pratique les
     * structures Multiboot de QEMU. Réservée en bloc (approche prudente). */
    e = pmm_core_reserve(&g_pmm, 0, 0x100000, "low memory (<1 MiB)");
    /* Image du noyau, table des descripteurs du PMM comprise (elle est dans .bss). */
    if (e == SAMOS_OK) {
        e = pmm_core_reserve_as(&g_pmm, (uintptr_t)__kernel_start,
                                (uintptr_t)__kernel_end - (uintptr_t)__kernel_start,
                                FRAME_KERNEL, "kernel image (incl. PMM descriptor table)");
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

/* K0.7-M2.3 : allocation typée commune ; le type est fixé AVANT que le cadre
 * ne quitte le PMM, il n'existe donc aucun instant où une table de pages
 * serait vue comme un cadre ALLOCATED libérable. */
static uint32_t alloc_zeroed(enum frame_type type)
{
    uint32_t f = pmm_core_alloc_as(&g_pmm, type);
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

uint32_t pmm_alloc_frame(void)
{
    return alloc_zeroed(FRAME_ALLOCATED);
}

/* Réservé au VMM (paging.c). Le cadre reçoit le type FRAME_PAGETABLE :
 * pmm_free_frame() le refusera toujours (SAMOS_EINVAL). */
uint32_t pmm_alloc_pagetable(void)
{
    return alloc_zeroed(FRAME_PAGETABLE);
}

samos_err_t pmm_free_frame(uint32_t phys)
{
    if ((phys & (PAGE_SIZE - 1u)) != 0) {
        g_pmm.invalid_frees++;
        return SAMOS_EINVAL;                 /* adresse non alignée : erreur de l'appelant */
    }
    return pmm_core_free(&g_pmm, phys >> PAGE_SHIFT);
}

/* K0.7-M2.4b — Références pour le VMM, par adresse physique alignée.
 * Pas encore appelées par vmm_map()/vmm_unmap() (M2.4c). */
samos_err_t pmm_ref(uint32_t phys)
{
    if ((phys & (PAGE_SIZE - 1u)) != 0) {
        g_pmm.ref_errors++;
        return SAMOS_EINVAL;
    }
    return pmm_core_ref(&g_pmm, phys >> PAGE_SHIFT);
}

samos_err_t pmm_unref(uint32_t phys)
{
    if ((phys & (PAGE_SIZE - 1u)) != 0) {
        g_pmm.ref_errors++;
        return SAMOS_EINVAL;
    }
    return pmm_core_unref(&g_pmm, phys >> PAGE_SHIFT);
}

uint32_t pmm_refs(uint32_t phys)
{
    return pmm_core_refs(&g_pmm, phys >> PAGE_SHIFT);
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
