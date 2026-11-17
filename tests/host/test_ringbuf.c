/*
 * SAMOS / AtlasOS — tests/host/test_ringbuf.c
 * Test « par modèle » de kernel/lib/ringbuf.c : 1 000 000 opérations
 * aléatoires (push/pop) sont appliquées à la fois à la file SAMOS et à un
 * modèle de référence trivial (tableau + compteurs) ; les deux doivent
 * toujours concorder. Les indices démarrent près de 2^32 pour exercer le
 * débordement des compteurs libres. Exécuté sous ASan + UBSan.
 */
#include <samos/ringbuf.h>
#include <stdint.h>
#include <string.h>
#include "check.h"

int main(void)
{
    uint8_t storage[16];
    struct ringbuf rb;
    CHECK(!ringbuf_init(&rb, storage, 0));
    CHECK(!ringbuf_init(&rb, storage, 1));
    CHECK(!ringbuf_init(&rb, storage, 12));
    CHECK(!ringbuf_init(&rb, NULL, 16));
    CHECK(ringbuf_init(&rb, storage, 16));

    /* Débordement des indices : on place head = tail = 2^32 - 5. */
    rb.head = rb.tail = UINT32_MAX - 4u;

    uint8_t model[16];
    unsigned m_head = 0, m_count = 0;      /* modèle : file circulaire classique */
    uint32_t dropped = 0;
    uint64_t x = 0x243F6A8885A308D3ull;    /* xorshift64 */
    int mismatch = 0;
    for (int i = 0; i < 1000000 && !mismatch; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        if ((x & 3u) != 0) {               /* 3/4 push, 1/4 pop : remplit souvent la file */
            uint8_t v = (uint8_t)(x >> 8);
            bool ok = ringbuf_push(&rb, v);
            bool m_ok = m_count < 16;
            if (m_ok) { model[(m_head + m_count) % 16] = v; m_count++; } else { dropped++; }
            if (ok != m_ok) mismatch = 1;
        } else {
            uint8_t v = 0;
            bool ok = ringbuf_pop(&rb, &v);
            bool m_ok = m_count > 0;
            uint8_t mv = 0;
            if (m_ok) { mv = model[m_head]; m_head = (m_head + 1) % 16; m_count--; }
            if (ok != m_ok || (ok && v != mv)) mismatch = 1;
        }
        if (ringbuf_count(&rb) != m_count) mismatch = 1;
    }
    CHECK(!mismatch);
    CHECK(rb.dropped == dropped);
    CHECK(rb.head < 1000000u);             /* les compteurs ont bien débordé 2^32 */
    return CHECK_REPORT("host ringbuf (1e6 random ops vs model)");
}
