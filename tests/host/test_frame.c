/*
 * SAMOS / AtlasOS — tests/host/test_frame.c
 * K0.7-M2.1 : encodage du descripteur de cadre (samos/frame.h).
 * Vérifie l'aller-retour type/références, les bornes et les invariants F1-F3,
 * de façon exhaustive sur les 65 536 valeurs possibles d'un descripteur.
 */
#include <samos/frame.h>
#include "check.h"

int main(void)
{
    /* Aller-retour pour chaque type, aux bornes du compteur */
    for (unsigned t = 0; t < FRAME_TYPE_COUNT; t++) {
        frame_desc_t d0 = frame_desc_make((enum frame_type)t, 0);
        frame_desc_t dm = frame_desc_make((enum frame_type)t, FRAME_REFS_MAX);
        CHECK((unsigned)frame_desc_type(d0) == t && frame_desc_refs(d0) == 0);
        CHECK((unsigned)frame_desc_type(dm) == t && frame_desc_refs(dm) == FRAME_REFS_MAX);
    }
    /* F2 : seules les projections d'un cadre ALLOCATED sont admises */
    CHECK(frame_desc_valid(frame_desc_make(FRAME_ALLOCATED, 7)));
    CHECK(!frame_desc_valid(frame_desc_make(FRAME_FREE, 1)));
    CHECK(!frame_desc_valid(frame_desc_make(FRAME_RESERVED, 1)));
    CHECK(!frame_desc_valid(frame_desc_make(FRAME_KERNEL, 1)));
    CHECK(!frame_desc_valid(frame_desc_make(FRAME_PAGETABLE, 1)));
    /* F1 : bit réservé et types inconnus rejetés */
    CHECK(!frame_desc_valid((frame_desc_t)(FRAME_FREE | FRAME_RSVD_BIT)));
    CHECK(!frame_desc_valid((frame_desc_t)5) && !frame_desc_valid((frame_desc_t)7));
    /* Propriété déduite du type */
    CHECK(frame_type_owner(FRAME_FREE) == FRAME_OWNER_PMM);
    CHECK(frame_type_owner(FRAME_RESERVED) == FRAME_OWNER_NONE);
    CHECK(frame_type_owner(FRAME_KERNEL) == FRAME_OWNER_IMAGE);
    CHECK(frame_type_owner(FRAME_ALLOCATED) == FRAME_OWNER_CALLER);
    CHECK(frame_type_owner(FRAME_PAGETABLE) == FRAME_OWNER_VMM);
    /* Exhaustif : un descripteur valide se ré-encode à l'identique, et
     * exactement 5 + 4095 descripteurs sont valides (5 types sans référence,
     * plus ALLOCATED avec 1..4095 références). */
    unsigned valid = 0, mismatch = 0;
    for (uint32_t v = 0; v <= 0xFFFFu; v++) {
        frame_desc_t d = (frame_desc_t)v;
        if (frame_desc_valid(d)) {
            valid++;
            if (frame_desc_make(frame_desc_type(d), frame_desc_refs(d)) != d) mismatch++;
        }
    }
    CHECK(valid == FRAME_TYPE_COUNT + FRAME_REFS_MAX);
    CHECK(mismatch == 0);
    return CHECK_REPORT("host frame descriptor");
}
