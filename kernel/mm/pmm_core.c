/*
 * SAMOS / AtlasOS — kernel/mm/pmm_core.c
 * Cœur pur de l'allocateur de cadres par bitmap. Voir pmm.h et Details.md §34.
 */
#include <samos/multiboot.h>
#include <samos/pmm.h>

static inline void bit_set(uint32_t *bm, uint32_t f)   { bm[f >> 5] |= (1u << (f & 31u)); }
static inline void bit_clear(uint32_t *bm, uint32_t f) { bm[f >> 5] &= ~(1u << (f & 31u)); }
static inline bool bit_test(const uint32_t *bm, uint32_t f) { return (bm[f >> 5] >> (f & 31u)) & 1u; }

/* Convertit [base, base+len) en cadres. outward = arrondi vers l'extérieur
 * (réservation : tout cadre touché), sinon vers l'intérieur (disponible :
 * seuls les cadres entièrement contenus). Le résultat est borné à nframes. */
static bool to_frames(uint64_t base, uint64_t len, bool outward, uint32_t nframes,
                      uint32_t *first, uint32_t *end)
{
    if (len == 0 || len - 1u > UINT64_MAX - base) {
        return false;
    }
    uint64_t last = base + (len - 1u);                       /* dernier octet inclus */
    uint64_t f0, f1;                                         /* [f0, f1) en cadres */
    if (outward) {
        f0 = base >> PAGE_SHIFT;
        f1 = (last >> PAGE_SHIFT) + 1u;
    } else {
        if (base > UINT64_MAX - (PAGE_SIZE - 1u)) {
            return false;                                    /* l'arrondi déborderait */
        }
        f0 = (base + PAGE_SIZE - 1u) >> PAGE_SHIFT;          /* arrondi supérieur */
        /* Fin exclue = last + 1, arrondie à l'inférieur ; last + 1 déborde
         * seulement si last == 2^64 - 1, cas traité à part. */
        f1 = (last == UINT64_MAX) ? ((uint64_t)1 << (64 - PAGE_SHIFT))
                                  : ((last + 1u) >> PAGE_SHIFT);
    }
    if (f0 >= nframes || f1 <= f0) {
        return false;
    }
    *first = (uint32_t)f0;
    *end = (f1 > nframes) ? nframes : (uint32_t)f1;
    return true;
}

static void mark_range(struct pmm *p, uint32_t first, uint32_t end, bool used)
{
    for (uint32_t f = first; f < end; f++) {
        bool was = bit_test(p->bitmap, f);
        if (used && !was) {
            bit_set(p->bitmap, f);
            p->free_count--;
        } else if (!used && was) {
            bit_clear(p->bitmap, f);
            p->free_count++;
        }
    }
}

samos_err_t pmm_core_reserve(struct pmm *p, uint64_t base, uint64_t len, const char *name)
{
    uint32_t first, end;
    if (!to_frames(base, len, true, p->nframes, &first, &end)) {
        return SAMOS_OK;                   /* hors de la mémoire gérée : rien à faire */
    }
    if (p->nreserved >= PMM_MAX_RESERVED) {
        return SAMOS_ENOSPC;
    }
    mark_range(p, first, end, true);
    p->reserved[p->nreserved++] = (struct pmm_range){ first, end, name };
    return SAMOS_OK;
}

samos_err_t pmm_core_init(struct pmm *p, uint32_t *bitmap, uint32_t bitmap_words,
                          uint32_t limit_frames, const struct memmap *m)
{
    if (p == 0 || bitmap == 0 || m == 0 || bitmap_words == 0) {
        return SAMOS_EINVAL;
    }
    uint64_t cap = (uint64_t)bitmap_words * 32u;
    uint32_t nframes = (limit_frames < cap) ? limit_frames : (uint32_t)cap;

    /* La mémoire gérée s'arrête à la fin de la plus haute région disponible. */
    uint64_t top = 0;
    for (unsigned i = 0; i < m->count; i++) {
        if (m->r[i].type == MULTIBOOT_MEMORY_AVAILABLE) {
            uint64_t end_frame = (m->r[i].base + m->r[i].length) >> PAGE_SHIFT;
            if (end_frame > top) {
                top = end_frame;
            }
        }
    }
    if (top < nframes) {
        nframes = (uint32_t)top;
    }

    p->bitmap = bitmap;
    p->nframes = nframes;
    p->hint = 0;
    p->nreserved = 0;
    p->double_frees = 0;
    p->invalid_frees = 0;
    for (uint32_t w = 0; w < (nframes + 31u) / 32u; w++) {
        bitmap[w] = 0xFFFFFFFFu;             /* tout occupé par défaut */
    }
    p->free_count = 0;

    /* 1. Libérer les cadres entièrement contenus dans une région disponible. */
    for (unsigned i = 0; i < m->count; i++) {
        uint32_t first, end;
        if (m->r[i].type == MULTIBOOT_MEMORY_AVAILABLE &&
            to_frames(m->r[i].base, m->r[i].length, false, nframes, &first, &end)) {
            mark_range(p, first, end, false);
        }
    }
    /* 2. Toute autre région l'emporte en cas de chevauchement : réservée. */
    for (unsigned i = 0; i < m->count; i++) {
        if (m->r[i].type != MULTIBOOT_MEMORY_AVAILABLE) {
            samos_err_t e = pmm_core_reserve(p, m->r[i].base, m->r[i].length,
                                             memmap_type_name(m->r[i].type));
            if (e != SAMOS_OK) {
                return e;
            }
        }
    }
    /* 3. Le cadre 0 n'est jamais alloué : 0 sert de valeur d'échec et la page
     *    nulle n'est pas projetée (déréférencement de NULL => faute). */
    return pmm_core_reserve(p, 0, PAGE_SIZE, "null frame");
}

bool pmm_core_is_used(const struct pmm *p, uint32_t frame)
{
    return frame >= p->nframes || bit_test(p->bitmap, frame);
}

bool pmm_core_is_reserved(const struct pmm *p, uint32_t frame)
{
    for (unsigned i = 0; i < p->nreserved; i++) {
        if (frame >= p->reserved[i].first && frame < p->reserved[i].end) {
            return true;
        }
    }
    return false;
}

uint32_t pmm_core_alloc(struct pmm *p)
{
    if (p->free_count == 0) {
        return PMM_NO_FRAME;
    }
    uint32_t words = (p->nframes + 31u) / 32u;
    /* Next-fit par mots de 32 bits : un mot 0xFFFFFFFF (32 cadres occupés)
     * est sauté en une comparaison. Deux passes : [hint, fin) puis [0, hint). */
    for (uint32_t k = 0; k < words; k++) {
        uint32_t w = (p->hint + k) % words;
        uint32_t bits = p->bitmap[w];
        if (bits == 0xFFFFFFFFu) {
            continue;
        }
        for (uint32_t b = 0; b < 32; b++) {
            uint32_t f = w * 32u + b;
            if (f >= p->nframes) {
                break;
            }
            if (((bits >> b) & 1u) == 0) {
                bit_set(p->bitmap, f);
                p->free_count--;
                p->hint = w;
                return f;
            }
        }
    }
    return PMM_NO_FRAME;                     /* inatteignable si free_count est cohérent */
}

samos_err_t pmm_core_free(struct pmm *p, uint32_t frame)
{
    if (frame >= p->nframes || pmm_core_is_reserved(p, frame)) {
        p->invalid_frees++;
        return SAMOS_EINVAL;                 /* I1 : jamais libérer un cadre réservé */
    }
    if (!bit_test(p->bitmap, frame)) {
        p->double_frees++;
        return SAMOS_ENOENT;                 /* I3 : déjà libre => double libération */
    }
    bit_clear(p->bitmap, frame);
    p->free_count++;
    return SAMOS_OK;
}
