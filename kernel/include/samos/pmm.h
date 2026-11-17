/*
 * SAMOS / AtlasOS — kernel/include/samos/pmm.h
 * ---------------------------------------------------------------------------
 * Gestionnaire de mémoire PHYSIQUE (K0.6) : allocateur de cadres (frames) de
 * 4 KiB par bitmap — 1 bit par cadre, 1 = occupé.
 *
 * Deux niveaux :
 *   - « cœur » pur (struct pmm, fonctions pmm_core_*) : aucune E/S ni
 *     variable globale, testé sur l'hôte contre un modèle ;
 *   - interface du noyau (pmm_init, pmm_alloc_frame, pmm_free_frame) : une
 *     instance globale initialisée depuis la carte mémoire et les zones
 *     occupées par le noyau.
 *
 * Invariants de sécurité :
 *   I1  un cadre réservé (mémoire basse, image du noyau, structures du
 *       chargeur, région non « available ») n'est JAMAIS alloué ni libérable ;
 *   I2  un cadre n'est jamais alloué deux fois sans libération intermédiaire ;
 *   I3  une double libération est détectée et refusée (et comptée) ;
 *   I4  free_count == nombre de bits à 0 dans la bitmap.
 */
#ifndef SAMOS_PMM_H
#define SAMOS_PMM_H

#include <stdbool.h>
#include <stdint.h>
#include <samos/errno.h>
#include <samos/memmap.h>

#define PAGE_SIZE        4096u
#define PAGE_SHIFT       12u
#define PMM_NO_FRAME     UINT32_MAX
#define PMM_MAX_RESERVED 96

struct pmm_range {
    uint32_t first;        /* premier cadre */
    uint32_t end;          /* cadre de fin (exclu) */
    const char *name;
};

struct pmm {
    uint32_t *bitmap;
    uint32_t nframes;      /* cadres gérés : [0, nframes) */
    uint32_t free_count;
    uint32_t hint;         /* mot de départ de la prochaine recherche (next-fit) */
    struct pmm_range reserved[PMM_MAX_RESERVED];
    unsigned nreserved;
    uint32_t double_frees;
    uint32_t invalid_frees;
};

/* Initialise : tout occupé, puis libère les régions « available » (cadres
 * entièrement contenus), puis réoccupe et enregistre comme réservée toute
 * région d'un autre type (cadres même partiellement touchés). `limit_frames`
 * borne la mémoire gérée (bitmap_words * 32 au maximum). */
samos_err_t pmm_core_init(struct pmm *p, uint32_t *bitmap, uint32_t bitmap_words,
                          uint32_t limit_frames, const struct memmap *m);

/* Réserve [base, base+len) arrondi vers l'EXTÉRIEUR aux pages. */
samos_err_t pmm_core_reserve(struct pmm *p, uint64_t base, uint64_t len, const char *name);

uint32_t pmm_core_alloc(struct pmm *p);                 /* numéro de cadre ou PMM_NO_FRAME */
samos_err_t pmm_core_free(struct pmm *p, uint32_t frame);
bool pmm_core_is_used(const struct pmm *p, uint32_t frame);
bool pmm_core_is_reserved(const struct pmm *p, uint32_t frame);

/* --- Interface du noyau --- */
samos_err_t pmm_init(const struct memmap *m, uint32_t limit_bytes);
/* Retourne l'adresse physique d'un cadre REMIS À ZÉRO, ou 0 si la mémoire est
 * épuisée (le cadre 0 étant toujours réservé, 0 n'est jamais un cadre valide). */
uint32_t pmm_alloc_frame(void);
samos_err_t pmm_free_frame(uint32_t phys);
const struct pmm *pmm_state(void);
void pmm_print(void);

#endif /* SAMOS_PMM_H */
