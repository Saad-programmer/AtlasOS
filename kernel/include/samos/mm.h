/*
 * SAMOS / AtlasOS — kernel/include/samos/mm.h
 * Initialisation du sous-système mémoire (K0.6) :
 *   carte mémoire -> allocateur physique -> pagination.
 */
#ifndef SAMOS_MM_H
#define SAMOS_MM_H

#include <samos/errno.h>
#include <samos/memmap.h>

samos_err_t mm_init(void);
const struct memmap *mm_memmap(void);

#endif /* SAMOS_MM_H */
