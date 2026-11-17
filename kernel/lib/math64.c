/*
 * SAMOS / AtlasOS — kernel/lib/math64.c
 * Division 64 bits / 32 bits par division longue binaire. Voir math64.h et
 * Details.md §19.
 *
 * Algorithme (restauration) : on parcourt les 64 bits de n du poids fort au
 * poids faible ; le reste partiel r est décalé d'un bit vers la gauche et
 * reçoit le bit courant de n ; si r >= d, on soustrait d et le bit du
 * quotient vaut 1. Invariant : avant chaque étape, r < d, donc après le
 * décalage r < 2d <= 2^33 : r tient dans 64 bits sans débordement.
 * Coût : 64 itérations, O(nombre de bits) ; suffisant pour l'affichage.
 */
#include <samos/math64.h>

uint64_t u64_divmod_u32(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (d == 0) {
        if (rem != 0) {
            *rem = 0;
        }
        return UINT64_MAX;
    }
    /* Chemin rapide : si n tient sur 32 bits, l'instruction DIV native suffit
     * (le compilateur génère une division 32 bits, sans libcall). */
    if ((n >> 32) == 0) {
        uint32_t n32 = (uint32_t)n;
        if (rem != 0) {
            *rem = n32 % d;
        }
        return n32 / d;
    }
    uint64_t q = 0;
    uint64_t r = 0;
    for (int bit = 63; bit >= 0; bit--) {
        r = (r << 1) | ((n >> bit) & 1u);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << bit;
        }
    }
    if (rem != 0) {
        *rem = (uint32_t)r;
    }
    return q;
}
