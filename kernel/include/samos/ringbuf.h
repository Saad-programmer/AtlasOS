/*
 * SAMOS / AtlasOS — kernel/include/samos/ringbuf.h
 * ---------------------------------------------------------------------------
 * File circulaire d'octets à un producteur et un consommateur (SPSC), sans
 * verrou (K0.5). Usage type : le gestionnaire d'IRQ clavier PRODUIT des
 * codes de touche, le code normal du noyau les CONSOMME.
 *
 * Principe : indices « libres » (free-running) head et tail sur 32 bits, qui
 * ne sont jamais ramenés dans [0, cap) ; seule l'indexation du tableau utilise
 * (indice & (cap - 1)). Avec cap puissance de 2 :
 *     nombre d'éléments = head - tail   (arithmétique non signée modulo 2^32)
 * ce qui reste exact même après débordement des compteurs, et distingue
 * « vide » (head == tail) de « plein » (head - tail == cap) sans case perdue.
 *
 * Règle de propriété (garantit l'absence de verrou) :
 *   - seul le producteur écrit head (et le tableau à la position head) ;
 *   - seul le consommateur écrit tail.
 * Sur un monoprocesseur x86, une barrière de COMPILATION suffit pour ordonner
 * « écrire la donnée » avant « publier head » (le CPU ne réordonne pas deux
 * écritures, et l'ISR s'exécute sur le même cœur). Voir Details.md §29.
 */
#ifndef SAMOS_RINGBUF_H
#define SAMOS_RINGBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ringbuf {
    uint8_t *data;
    uint32_t cap;                 /* puissance de 2, >= 2 */
    volatile uint32_t head;       /* écrit par le producteur uniquement */
    volatile uint32_t tail;       /* écrit par le consommateur uniquement */
    volatile uint32_t dropped;    /* octets refusés car la file était pleine (producteur) */
};

/* Retourne false si cap n'est pas une puissance de 2 >= 2 ou data == NULL. */
bool ringbuf_init(struct ringbuf *rb, uint8_t *storage, uint32_t cap);

/* Producteur. Retourne false (et compte une perte) si la file est pleine :
 * on ne bloque JAMAIS dans un gestionnaire d'interruption. */
bool ringbuf_push(struct ringbuf *rb, uint8_t byte);

/* Consommateur. Retourne false si la file est vide. */
bool ringbuf_pop(struct ringbuf *rb, uint8_t *out);

uint32_t ringbuf_count(const struct ringbuf *rb);

#endif /* SAMOS_RINGBUF_H */
