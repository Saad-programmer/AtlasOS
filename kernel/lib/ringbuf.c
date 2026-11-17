/*
 * SAMOS / AtlasOS — kernel/lib/ringbuf.c
 * File circulaire SPSC. Voir ringbuf.h et Details.md §29.
 * Code portable : testé sur l'hôte (ASan/UBSan) contre un modèle de référence.
 */
#include <samos/ringbuf.h>

/* Barrière de compilation (aucune instruction émise). */
#define RB_BARRIER() __asm__ __volatile__("" ::: "memory")

bool ringbuf_init(struct ringbuf *rb, uint8_t *storage, uint32_t cap)
{
    if (rb == NULL || storage == NULL || cap < 2 || (cap & (cap - 1)) != 0) {
        return false;
    }
    rb->data = storage;
    rb->cap = cap;
    rb->head = 0;
    rb->tail = 0;
    rb->dropped = 0;
    return true;
}

uint32_t ringbuf_count(const struct ringbuf *rb)
{
    return rb->head - rb->tail;   /* modulo 2^32 : exact même après débordement */
}

bool ringbuf_push(struct ringbuf *rb, uint8_t byte)
{
    uint32_t head = rb->head;
    if (head - rb->tail == rb->cap) {
        rb->dropped++;
        return false;
    }
    rb->data[head & (rb->cap - 1)] = byte;
    RB_BARRIER();                 /* la donnée est écrite AVANT la publication */
    rb->head = head + 1;
    return true;
}

bool ringbuf_pop(struct ringbuf *rb, uint8_t *out)
{
    uint32_t tail = rb->tail;
    if (rb->head == tail) {
        return false;
    }
    RB_BARRIER();                 /* lire head AVANT la donnée qu'il publie */
    *out = rb->data[tail & (rb->cap - 1)];
    RB_BARRIER();                 /* la donnée est lue AVANT de libérer la case */
    rb->tail = tail + 1;
    return true;
}
