/*
 * SAMOS / AtlasOS — kernel/include/samos/errno.h
 * Codes d'erreur internes du noyau. Convention : 0 = succès, valeur négative
 * = erreur. Les fonctions faillibles retournent un `int` (samos_err_t).
 */
#ifndef SAMOS_ERRNO_H
#define SAMOS_ERRNO_H

typedef int samos_err_t;

enum {
    SAMOS_OK       = 0,
    SAMOS_EINVAL   = -1,   /* argument ou donnée invalide */
    SAMOS_ENOMEM   = -2,   /* plus de mémoire */
    SAMOS_ERANGE   = -3,   /* valeur hors limites */
    SAMOS_ENOENT   = -4,   /* élément absent */
    SAMOS_EEXIST   = -5,   /* élément déjà présent */
    SAMOS_EFAULT   = -6,   /* adresse invalide */
    SAMOS_ENOSPC   = -7,   /* plus de place (tampon plein) */
    SAMOS_EAGAIN   = -8,   /* ressource temporairement indisponible (tampon vide) */
    SAMOS_EBUSY    = -9,   /* ressource occupée */
};

#endif /* SAMOS_ERRNO_H */
