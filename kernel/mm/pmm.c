/*
 * SAMOS / AtlasOS — kernel/mm/pmm.c
 * Instance noyau de l'allocateur de cadres. Voir pmm.h et Details.md §34.
 */
#include <arch/paging.h>
#include <samos/bootinfo.h>
#include <samos/kernel.h>
#include <samos/log.h>
#include <samos/multiboot.h>
#include <samos/panic.h>
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
/* K0.7-P4f-D — Carte mémoire du démarrage (g_memmap de mm.c, jamais modifiée
 * après mm_init) : seconde source, DISTINCTE de g_meta, pour reconnaître la
 * mémoire réservée par le microprogramme. */
static const struct memmap *g_boot_map;

samos_err_t pmm_init(const struct memmap *m, uint32_t limit_bytes)
{
    samos_err_t e = pmm_core_init(&g_pmm, g_meta, PMM_MAX_FRAMES, limit_bytes >> PAGE_SHIFT, m);
    g_boot_map = m;
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

/*
 * K0.7-P4f-D (N1, audit P4f-C) — Cadres JAMAIS distribuables, quel que soit
 * leur descripteur. Une falsification de g_meta pouvait faire distribuer une
 * page de l'image (GDT/TSS, .data, pile), REMISE À ZÉRO ci-dessous puis
 * publiée comme table. Critères INDÉPENDANTS de g_meta, appliqués AVANT toute
 * écriture dans le cadre :
 *   1. image du noyau [__kernel_start, __kernel_end) : symboles de l'éditeur
 *      de liens (constantes) ; la table des descripteurs elle-même y est ;
 *   2. mémoire basse [0, 1 MiB) : constante (IVT, BDA, EBDA, VGA, ROM) ;
 *   3. hors d'une région DISPONIBLE de la carte du démarrage, ou recouvrant
 *      une région non disponible (même règle que pmm_core_init : toute autre
 *      région l'emporte). Source : g_memmap, distincte de g_meta mais non
 *      scellée (donnée RW de l'image) : critère plus faible que 1 et 2.
 * Renvoie le motif, ou 0 si le cadre est distribuable.
 */
static const char *frame_protected_reason(uint32_t phys)
{
    if (phys < 0x100000u) {
        return "low memory";
    }
    if (phys >= ((uint32_t)(uintptr_t)__kernel_start & ~(PAGE_SIZE - 1u)) &&
        phys < (uint32_t)(uintptr_t)__kernel_end) {
        return "kernel image";
    }
    if (g_boot_map != 0) {
        uint64_t lo = phys, hi = (uint64_t)phys + PAGE_SIZE;
        bool inside = false;
        for (unsigned i = 0; i < g_boot_map->count; i++) {
            const struct mem_region *r = &g_boot_map->r[i];
            bool overlap = r->base < hi && lo < r->base + r->length;
            if (!overlap) {
                continue;
            }
            if (r->type != MULTIBOOT_MEMORY_AVAILABLE) {
                return "firmware-reserved memory";
            }
            if (r->base <= lo && hi <= r->base + r->length) {
                inside = true;
            }
        }
        if (!inside) {
            return "outside available memory";
        }
    }
    return 0;
}

/* Empreinte FNV-1a de la page (lecture seule, par l'identité) : le
 * diagnostic prouve que la page refusée n'a PAS été modifiée. Page nulle :
 * jamais projetée, empreinte non calculée. */
static uint32_t page_checksum(uint32_t phys)
{
    if (phys == 0) {
        return 0;
    }
    const volatile uint8_t *b = (const volatile uint8_t *)(uintptr_t)phys;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* K1.0-N3 — Image de TEST seulement : neutralisation du contrôle N3-I2 de
 * alloc_zeroed, pour éprouver la SECONDE couche (contrôle D1 de table_create,
 * mode test=tablealias_l2). Absente de l'image de production (constante). */
#if CONFIG_KTEST
static bool g_test_skip_n3_alloc_check;
void pmm_test_skip_n3_alloc_check(bool on)
{
    g_test_skip_n3_alloc_check = on;
}
#define N3_ALLOC_CHECK_ENABLED() (!g_test_skip_n3_alloc_check)
#else
#define N3_ALLOC_CHECK_ENABLED() (true)
#endif

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
    /* K0.7-P4f-D (N1) — Contrôle AVANT la remise à zéro. Un cadre protégé ne
     * peut être distribué que si g_meta est corrompue : violation
     * d'invariant, arrêt déterministe. Le cadre n'est ni écrit ni rendu ;
     * l'empreinte affichée permet de vérifier qu'il est intact. */
    const char *why = frame_protected_reason(phys);
    if (why != 0) {
        panic("pmm: descriptor table corrupted: protected frame %p (%s) handed out as type %d; "
              "refused before zeroing, page checksum 0x%08x",
              (void *)(uintptr_t)phys, why, (int)type, page_checksum(phys));
    }
    /* Remise à zéro : un cadre libéré peut contenir des données d'un ancien
     * propriétaire (fuite d'information) ou de vieilles entrées de table de
     * pages (projection fantôme). Accès par l'identité : la mémoire gérée est
     * bornée à la zone projetée en identité (paging.c). */
    /* K1.0-N3 (N3-I2) — Jamais distribué ni remis à zéro tant qu'une
     * projection DYNAMIQUE le désigne : réponse tirée des PTE publiées
     * (vmm_frame_dynamically_mapped), indépendante du type et de `refs`. Un
     * cadre projeté n'arrive ici que si g_meta ment (type FREE falsifié, ou
     * libération obtenue par un `refs` falsifié avant K1.0) : violation
     * d'invariant. Arrêt AVANT memset (les données de l'ancien propriétaire
     * restent intactes, empreinte affichée) ; le cadre a déjà quitté FREE dans
     * pmm_core_alloc_as, et le remettre FREE le ferait redistribuer aussitôt.
     * Couvre les deux chemins de distribution : données (pmm_alloc_frame) et
     * tables (pmm_alloc_pagetable, avant le contrôle D1 de table_create). */
    if (N3_ALLOC_CHECK_ENABLED() && vmm_frame_dynamically_mapped(phys)) {
        panic("pmm: descriptor table corrupted: frame %p still mapped outside identity (%u dynamic mappings) "
              "handed out as type %d; refused before zeroing, page checksum 0x%08x",
              (void *)(uintptr_t)phys, vmm_dynamic_mappings(phys), (int)type, page_checksum(phys));
    }
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
    /* K1.0-N3 (N3-I1) — Refus d'un cadre encore projeté hors identité, AVANT
     * toute lecture de son descripteur : la décision ne dépend ni du type ni
     * de `refs` (falsifiables). Erreur ordinaire, SANS EFFET (aucune écriture
     * dans g_meta, free_count ou hint) ; compteur distinct de busy_frees : un
     * refus ici alors que refs vaudrait 0 révèle une incohérence. */
    if (vmm_frame_dynamically_mapped(phys)) {
        g_pmm.mapped_frees++;
        return SAMOS_EBUSY;
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
