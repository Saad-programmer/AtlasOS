/*
 * SAMOS / AtlasOS — kernel/mm/pmm_core.c
 * Cœur pur de l'allocateur de cadres. K0.6 : bitmap ; K0.7-M2.2 : table de
 * descripteurs (samos/frame.h). Voir pmm.h, Details.md §34 et
 * docs/design/K0.7-M2-frame-tracking.md.
 */
#include <samos/multiboot.h>
#include <samos/pmm.h>

/* K0.7-M2.2 : SEUL endroit où le type d'un cadre change, et donc SEUL endroit
 * où free_count est modifié (invariant G1). Les références sont remises à 0 :
 * en M2.2 elles ne sont jamais incrémentées (comptage prévu en M4). */
static void set_type(struct pmm *p, uint32_t f, enum frame_type t)
{
    enum frame_type old = frame_desc_type(p->meta[f]);
    if (old == FRAME_FREE && t != FRAME_FREE) {
        p->free_count--;
    } else if (old != FRAME_FREE && t == FRAME_FREE) {
        p->free_count++;
    }
    p->meta[f] = frame_desc_make(t, 0);
}

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

samos_err_t pmm_core_reserve_as(struct pmm *p, uint64_t base, uint64_t len,
                                enum frame_type type, const char *name)
{
    if (type != FRAME_RESERVED && type != FRAME_KERNEL) {
        return SAMOS_EINVAL;
    }
    uint32_t first, end;
    if (!to_frames(base, len, true, p->nframes, &first, &end)) {
        return SAMOS_OK;                   /* hors de la mémoire gérée : rien à faire */
    }
    /* Vérification complète AVANT toute modification : une réservation
     * refusée ne laisse aucun cadre à moitié modifié. */
    for (uint32_t f = first; f < end; f++) {
        enum frame_type t = frame_desc_type(p->meta[f]);
        if (t == FRAME_ALLOCATED || t == FRAME_PAGETABLE) {
            p->invalid_transitions++;
            return SAMOS_EBUSY;
        }
    }
    if (p->nreserved >= PMM_MAX_RESERVED) {
        return SAMOS_ENOSPC;
    }
    for (uint32_t f = first; f < end; f++) {
        /* Un cadre KERNEL garde son type : une réservation RESERVED qui le
         * chevauche (zone du firmware, structure du chargeur) ne doit pas
         * effacer l'information « appartient à l'image du noyau ». */
        if (type == FRAME_RESERVED && frame_desc_type(p->meta[f]) == FRAME_KERNEL) {
            continue;
        }
        set_type(p, f, type);
    }
    p->reserved[p->nreserved++] = (struct pmm_range){ first, end, name };
    return SAMOS_OK;
}

samos_err_t pmm_core_reserve(struct pmm *p, uint64_t base, uint64_t len, const char *name)
{
    return pmm_core_reserve_as(p, base, len, FRAME_RESERVED, name);
}

samos_err_t pmm_core_init(struct pmm *p, frame_desc_t *meta, uint32_t meta_entries,
                          uint32_t limit_frames, const struct memmap *m)
{
    if (p == 0 || meta == 0 || m == 0 || meta_entries == 0) {
        return SAMOS_EINVAL;
    }
    uint32_t nframes = (limit_frames < meta_entries) ? limit_frames : meta_entries;

    /* La mémoire gérée s'arrête à la fin de la plus haute région disponible.
     * K0.7-M2.2 (constat L6) : calcul à partir du DERNIER octet,
     * (base + length - 1) >> 12, puis + 1. L'ancienne forme
     * (base + length) >> 12 débordait à 0 pour une région se terminant à 2^64.
     * memmap garantit base + length - 1 <= 2^64 - 1 (pas de débordement). */
    uint64_t top = 0;
    for (unsigned i = 0; i < m->count; i++) {
        if (m->r[i].type == MULTIBOOT_MEMORY_AVAILABLE) {
            uint64_t last = m->r[i].base + (m->r[i].length - 1u);
            /* floor((last + 1) / 4096) sans calculer last + 1 (qui déborde si
             * last == 2^64 - 1) : même résultat que la formule de K0.6, qui
             * ne compte PAS un dernier cadre seulement partiellement disponible
             * (constaté par le test hébergé : 0x903 et non 0x904). */
            uint64_t end_frame = (last >> PAGE_SHIFT) +
                                 (((last & (PAGE_SIZE - 1u)) == PAGE_SIZE - 1u) ? 1u : 0u);
            if (end_frame > top) {
                top = end_frame;
            }
        }
    }
    if (top < nframes) {
        nframes = (uint32_t)top;
    }

    p->meta = meta;
    p->nframes = nframes;
    p->hint = 0;
    p->nreserved = 0;
    p->double_frees = 0;
    p->invalid_frees = 0;
    p->invalid_transitions = 0;
    /* Écriture directe (sans set_type) : état initial, free_count = 0. */
    for (uint32_t f = 0; f < nframes; f++) {
        meta[f] = frame_desc_make(FRAME_RESERVED, 0);   /* rien n'est disponible par défaut */
    }
    p->free_count = 0;

    /* 1. Libérer les cadres entièrement contenus dans une région disponible. */
    for (unsigned i = 0; i < m->count; i++) {
        uint32_t first, end;
        if (m->r[i].type == MULTIBOOT_MEMORY_AVAILABLE &&
            to_frames(m->r[i].base, m->r[i].length, false, nframes, &first, &end)) {
            for (uint32_t f = first; f < end; f++) {
                set_type(p, f, FRAME_FREE);
            }
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

enum frame_type pmm_core_type(const struct pmm *p, uint32_t frame)
{
    return (frame < p->nframes) ? frame_desc_type(p->meta[frame]) : FRAME_RESERVED;
}

bool pmm_core_is_used(const struct pmm *p, uint32_t frame)
{
    return pmm_core_type(p, frame) != FRAME_FREE;
}

/* K0.7-M2.2 : déduit du descripteur (et non plus d'une recherche dans la
 * liste des plages) : la table est la seule source de vérité. */
bool pmm_core_is_reserved(const struct pmm *p, uint32_t frame)
{
    enum frame_type t = pmm_core_type(p, frame);
    return t == FRAME_RESERVED || t == FRAME_KERNEL;
}

uint32_t pmm_core_check(const struct pmm *p)
{
    uint32_t anomalies = 0, free_seen = 0;
    for (uint32_t f = 0; f < p->nframes; f++) {
        frame_desc_t d = p->meta[f];
        if (!frame_desc_valid(d)) {
            anomalies++;                     /* F1, F2 */
        }
        if (frame_desc_refs(d) != 0) {
            anomalies++;                     /* M2.2 : aucune référence n'est encore comptée */
        }
        if (frame_desc_type(d) == FRAME_FREE) {
            free_seen++;
        }
    }
    if (free_seen != p->free_count) {
        anomalies++;                         /* G1 */
    }
    if (p->nframes != 0 && pmm_core_type(p, 0) != FRAME_RESERVED) {
        anomalies++;                         /* le cadre 0 n'est jamais distribuable */
    }
    return anomalies;
}

uint32_t pmm_core_alloc(struct pmm *p)
{
    return pmm_core_alloc_as(p, FRAME_ALLOCATED);
}

/* K0.7-M2.3 : allocation TYPÉE. Seuls deux types peuvent naître d'une
 * allocation : ALLOCATED (usage général) et PAGETABLE (structures de
 * pagination, propriété du VMM). Tout autre type est refusé. */
uint32_t pmm_core_alloc_as(struct pmm *p, enum frame_type type)
{
    if (type != FRAME_ALLOCATED && type != FRAME_PAGETABLE) {
        return PMM_NO_FRAME;
    }
    if (p->free_count == 0) {
        return PMM_NO_FRAME;
    }
    /* Next-fit : recherche circulaire à partir de la dernière allocation.
     * (La bitmap permettait de sauter 32 cadres occupés en une comparaison ;
     * le parcours est maintenant cadre par cadre. Le coût est mesuré par le
     * test d'épuisement : voir Walkthrough K0.7-M2.2.) */
    for (uint32_t k = 0; k < p->nframes; k++) {
        uint32_t f = p->hint + k;
        if (f >= p->nframes) {
            f -= p->nframes;
        }
        if (frame_desc_type(p->meta[f]) == FRAME_FREE) {
            set_type(p, f, type);
            p->hint = f;
            return f;
        }
    }
    return PMM_NO_FRAME;                     /* inatteignable si free_count est cohérent */
}

/* Seul un cadre ALLOCATED peut être libéré (G2 ; G4 dès que les tables de
 * pages auront le type PAGETABLE, en M3). Codes conservés depuis K0.6 :
 * SAMOS_ENOENT = double libération, SAMOS_EINVAL = cadre non libérable. */
samos_err_t pmm_core_free(struct pmm *p, uint32_t frame)
{
    enum frame_type t = pmm_core_type(p, frame);
    if (t == FRAME_FREE) {
        p->double_frees++;
        return SAMOS_ENOENT;
    }
    if (t != FRAME_ALLOCATED) {
        p->invalid_frees++;
        return SAMOS_EINVAL;
    }
    set_type(p, frame, FRAME_FREE);
    return SAMOS_OK;
}
