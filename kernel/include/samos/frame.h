/*
 * SAMOS / AtlasOS — kernel/include/samos/frame.h
 * ---------------------------------------------------------------------------
 * K0.7-M2.1 — Représentation des métadonnées d'un cadre physique de 4 KiB.
 * Conception : docs/design/K0.7-M2-frame-tracking.md.
 *
 * Ce fichier ne contient QUE des définitions (aucun état, aucun code appelé
 * par le noyau à ce stade). Il sera branché sur le PMM en M2.2.
 *
 * Trois notions distinctes, à ne pas confondre :
 *   - TYPE d'utilisation : ce que contient le cadre (stocké) ;
 *   - PROPRIÉTÉ          : qui a le droit de changer son état (déduite du
 *                          type en K0 : un seul espace d'adressage noyau) ;
 *   - RÉFÉRENCES         : combien de projections DYNAMIQUES le désignent
 *                          (stocké ; la projection en identité n'est pas comptée).
 *
 * Descripteur de 16 bits :
 *      15                         4  3   2      0
 *     ┌────────────────────────────┬───┬────────┐
 *     │ références (12 bits)       │ 0 │ type   │
 *     └────────────────────────────┴───┴────────┘
 *   bit 3 réservé (doit valoir 0).
 */
#ifndef SAMOS_FRAME_H
#define SAMOS_FRAME_H

#include <stdbool.h>
#include <stdint.h>
#include <samos/compiler.h>

typedef uint16_t frame_desc_t;

/* Type d'utilisation. Valeurs stables (stockées dans le descripteur). */
enum frame_type {
    FRAME_FREE      = 0,   /* disponible pour l'allocation */
    FRAME_RESERVED  = 1,   /* firmware, mémoire basse, cadre nul, chargeur : jamais alloué */
    FRAME_KERNEL    = 2,   /* image du noyau : jamais libéré */
    FRAME_ALLOCATED = 3,   /* alloué par pmm_alloc_frame() à un utilisateur du noyau */
    FRAME_PAGETABLE = 4,   /* répertoire ou table de pages actif (propriété du VMM) */
};
#define FRAME_TYPE_COUNT 5

/* Propriétaire, déduit du type (non stocké en K0). */
enum frame_owner {
    FRAME_OWNER_PMM,       /* FREE : seul le PMM peut le donner */
    FRAME_OWNER_NONE,      /* RESERVED : personne, aucune transition permise */
    FRAME_OWNER_IMAGE,     /* KERNEL : l'image, aucune transition permise */
    FRAME_OWNER_CALLER,    /* ALLOCATED : l'appelant de pmm_alloc_frame() */
    FRAME_OWNER_VMM,       /* PAGETABLE : uniquement le VMM */
};

#define FRAME_TYPE_MASK   0x0007u
#define FRAME_RSVD_BIT    0x0008u
#define FRAME_REFS_SHIFT  4u
#define FRAME_REFS_MAX    0x0FFFu   /* 4095 projections dynamiques au plus */

SAMOS_STATIC_ASSERT(FRAME_TYPE_COUNT - 1 <= FRAME_TYPE_MASK, "le type tient dans 3 bits");
SAMOS_STATIC_ASSERT((FRAME_REFS_MAX << FRAME_REFS_SHIFT) <= 0xFFFFu, "les références tiennent dans 12 bits");

static inline frame_desc_t frame_desc_make(enum frame_type t, uint32_t refs)
{
    return (frame_desc_t)((refs << FRAME_REFS_SHIFT) | ((uint32_t)t & FRAME_TYPE_MASK));
}

static inline enum frame_type frame_desc_type(frame_desc_t d)
{
    return (enum frame_type)(d & FRAME_TYPE_MASK);
}

static inline uint32_t frame_desc_refs(frame_desc_t d)
{
    return (uint32_t)d >> FRAME_REFS_SHIFT;
}

static inline enum frame_owner frame_type_owner(enum frame_type t)
{
    switch (t) {
    case FRAME_FREE:      return FRAME_OWNER_PMM;
    case FRAME_KERNEL:    return FRAME_OWNER_IMAGE;
    case FRAME_ALLOCATED: return FRAME_OWNER_CALLER;
    case FRAME_PAGETABLE: return FRAME_OWNER_VMM;
    default:              return FRAME_OWNER_NONE;
    }
}

/*
 * Cohérence d'un descripteur (invariants F1-F3 du document de conception) :
 *   F1 type connu, bit réservé nul ;
 *   F2 références > 0 seulement pour un cadre ALLOCATED (un cadre libre,
 *      réservé, du noyau ou de pagination n'est jamais projeté dynamiquement) ;
 *   F3 références <= FRAME_REFS_MAX (garanti par l'encodage).
 */
static inline bool frame_desc_valid(frame_desc_t d)
{
    if ((d & FRAME_RSVD_BIT) != 0 || frame_desc_type(d) >= FRAME_TYPE_COUNT) {
        return false;
    }
    return frame_desc_refs(d) == 0 || frame_desc_type(d) == FRAME_ALLOCATED;
}

#endif /* SAMOS_FRAME_H */
