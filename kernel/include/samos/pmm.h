/*
 * SAMOS / AtlasOS — kernel/include/samos/pmm.h
 * ---------------------------------------------------------------------------
 * Gestionnaire de mémoire PHYSIQUE : allocateur de cadres (frames) de 4 KiB.
 * K0.6 : bitmap (1 bit par cadre). K0.7-M2.2 : un DESCRIPTEUR de 16 bits par
 * cadre (samos/frame.h) : type d'utilisation + références (non comptées
 * avant M4). Conception : docs/design/K0.7-M2-frame-tracking.md.
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
 *   I4  free_count == nombre de descripteurs FRAME_FREE (G1 de la conception),
 *       vérifiable par pmm_core_check().
 * Correspondance avec la conception M2 : I1 = G2, I4 = G1.
 */
#ifndef SAMOS_PMM_H
#define SAMOS_PMM_H

#include <stdbool.h>
#include <stdint.h>
#include <samos/errno.h>
#include <samos/frame.h>
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

/* K0.7-M2.2 : la bitmap (1 bit par cadre) est remplacée par une table de
 * descripteurs de 16 bits (samos/frame.h, docs/design/K0.7-M2-frame-tracking.md).
 * Le descripteur est désormais la SEULE source de vérité sur l'état d'un cadre ;
 * la liste `reserved` ne sert plus qu'au diagnostic. */
struct pmm {
    frame_desc_t *meta;    /* un descripteur par cadre géré */
    uint32_t nframes;      /* cadres gérés : [0, nframes) */
    uint32_t free_count;   /* G1 : nombre de descripteurs FRAME_FREE */
    uint32_t hint;         /* cadre de départ de la prochaine recherche (next-fit) */
    struct pmm_range reserved[PMM_MAX_RESERVED];
    unsigned nreserved;
    uint32_t double_frees;         /* libération d'un cadre déjà libre */
    uint32_t invalid_frees;        /* libération d'un cadre non alloué par le PMM */
    uint32_t invalid_transitions;  /* réservation refusée sur un cadre déjà distribué */
    uint32_t ref_errors;           /* K0.7-M2.4a : opérations de référence refusées */
};

/* Initialise : tout RESERVED, puis FREE pour les cadres entièrement contenus
 * dans une région « available », puis RESERVED (prioritaire) pour tout cadre
 * touché par une région d'un autre type, puis le cadre 0. La mémoire gérée est
 * bornée par `limit_frames` et par `meta_entries` (taille de la table). */
samos_err_t pmm_core_init(struct pmm *p, frame_desc_t *meta, uint32_t meta_entries,
                          uint32_t limit_frames, const struct memmap *m);

/* Réserve [base, base+len) arrondi vers l'EXTÉRIEUR aux pages, avec le type
 * FRAME_RESERVED ou FRAME_KERNEL. Refuse (SAMOS_EBUSY, compté dans
 * invalid_transitions) de toucher un cadre déjà ALLOCATED ou PAGETABLE :
 * on ne retire jamais un cadre à son propriétaire. */
samos_err_t pmm_core_reserve_as(struct pmm *p, uint64_t base, uint64_t len,
                                enum frame_type type, const char *name);
/* Équivalent à pmm_core_reserve_as(..., FRAME_RESERVED, ...). */
samos_err_t pmm_core_reserve(struct pmm *p, uint64_t base, uint64_t len, const char *name);

uint32_t pmm_core_alloc(struct pmm *p);                 /* FRAME_ALLOCATED ; numéro de cadre ou PMM_NO_FRAME */
/* K0.7-M2.3 : type = FRAME_ALLOCATED ou FRAME_PAGETABLE (sinon PMM_NO_FRAME). */
uint32_t pmm_core_alloc_as(struct pmm *p, enum frame_type type);
samos_err_t pmm_core_free(struct pmm *p, uint32_t frame);
bool pmm_core_is_used(const struct pmm *p, uint32_t frame);       /* type != FREE, ou hors limites */
bool pmm_core_is_reserved(const struct pmm *p, uint32_t frame);   /* RESERVED ou KERNEL */
/* Type d'un cadre ; FRAME_RESERVED pour un cadre hors de la mémoire gérée. */
enum frame_type pmm_core_type(const struct pmm *p, uint32_t frame);
/* Recompte les descripteurs et vérifie G1 et F1-F3 sur toute la table.
 * Retourne le nombre d'anomalies (0 = cohérent). Coût O(nframes). */
uint32_t pmm_core_check(const struct pmm *p);
/* K0.7-M2.4a : compteur de références d'un cadre ALLOCATED.
 * Refus sans modification du descripteur : SAMOS_EPERM (type), SAMOS_ERANGE
 * (borne FRAME_REFS_MAX ou zéro). */
samos_err_t pmm_core_ref(struct pmm *p, uint32_t frame);
samos_err_t pmm_core_unref(struct pmm *p, uint32_t frame);
uint32_t pmm_core_refs(const struct pmm *p, uint32_t frame);

/* --- Interface du noyau --- */
samos_err_t pmm_init(const struct memmap *m, uint32_t limit_bytes);
/* Retourne l'adresse physique d'un cadre REMIS À ZÉRO, ou 0 si la mémoire est
 * épuisée (le cadre 0 étant toujours réservé, 0 n'est jamais un cadre valide). */
uint32_t pmm_alloc_frame(void);
/* K0.7-M2.3 : cadre FRAME_PAGETABLE remis à zéro, pour le VMM uniquement.
 * Jamais libérable par pmm_free_frame(). Retourne 0 si épuisé. */
uint32_t pmm_alloc_pagetable(void);
samos_err_t pmm_free_frame(uint32_t phys);
const struct pmm *pmm_state(void);
void pmm_print(void);

#endif /* SAMOS_PMM_H */
