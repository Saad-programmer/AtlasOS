/*
 * SAMOS / AtlasOS — kernel/lib/format.c
 * Formatage printf indépendant. Voir format.h et Details.md §19.
 *
 * Structure : un « curseur de sortie » (struct out) mémorise le tampon, sa
 * capacité et le nombre de caractères PRODUITS (qui peut dépasser la capacité).
 * Chaque caractère passe par out_char(), seul endroit qui écrit dans le
 * tampon, et qui vérifie la borne : c'est l'invariant de sécurité central.
 */
#include <stdint.h>
#include <samos/format.h>
#include <samos/math64.h>
#include <samos/string.h>

struct out {
    char *buf;
    size_t cap;   /* capacité totale, terminateur compris */
    size_t len;   /* caractères produits (peut dépasser cap - 1) */
};

static void out_char(struct out *o, char c)
{
    if (o->cap != 0 && o->len < o->cap - 1) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_repeat(struct out *o, char c, int n)
{
    while (n-- > 0) {
        out_char(o, c);
    }
}

/* Spécification d'une conversion analysée. */
struct spec {
    int left;       /* drapeau '-' */
    int zero;       /* drapeau '0' */
    int width;      /* largeur minimale, 0 si absente */
    int precision;  /* -1 si absente */
};

/* Écrit `s` (longueur n) avec remplissage selon la spec. */
static void out_padded(struct out *o, const struct spec *sp, const char *s, size_t n, char pad)
{
    int padding = (sp->width > (int)n) ? sp->width - (int)n : 0;
    if (!sp->left) {
        out_repeat(o, pad, padding);
    }
    for (size_t i = 0; i < n; i++) {
        out_char(o, s[i]);
    }
    if (sp->left) {
        out_repeat(o, ' ', padding);
    }
}

/* Convertit v en chiffres dans la base donnée ; retourne la longueur.
 * buf doit contenir au moins 64 caractères (base 2, pire cas). */
static size_t u64_to_digits(uint64_t v, unsigned base, int upper, char *buf)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[64];
    size_t n = 0;
    do {
        uint32_t r;
        if (base == 16) {             /* base puissance de 2 : décalages, pas de division */
            r = (uint32_t)(v & 0xFu);
            v >>= 4;
        } else {
            v = u64_divmod_u32(v, base, &r);
        }
        tmp[n++] = digits[r];
    } while (v != 0 && n < sizeof(tmp));
    for (size_t i = 0; i < n; i++) {   /* inversion : chiffres de poids fort en premier */
        buf[i] = tmp[n - 1 - i];
    }
    return n;
}

/*
 * Écrit un entier : [espaces] [signe] [préfixe] [zéros] chiffres [espaces].
 * Précision (C11 §7.21.6.1) : nombre MINIMAL de chiffres ; une précision
 * explicite annule le drapeau '0' ; précision 0 et valeur 0 => aucun chiffre.
 */
static void out_number(struct out *o, const struct spec *sp, uint64_t mag, int negative,
                       unsigned base, int upper, const char *prefix)
{
    char digits[64];
    size_t nd = u64_to_digits(mag, base, upper, digits);
    if (sp->precision == 0 && mag == 0) {
        nd = 0;
    }
    size_t prec_zeros = (sp->precision > (int)nd) ? (size_t)sp->precision - nd : 0;
    size_t np = prefix ? strlen(prefix) : 0;
    size_t total = nd + prec_zeros + np + (negative ? 1u : 0u);
    int padding = (sp->width > (int)total) ? sp->width - (int)total : 0;
    int zero_pad = sp->zero && !sp->left && sp->precision < 0;

    if (!sp->left && !zero_pad) {
        out_repeat(o, ' ', padding);
    }
    if (negative) {
        out_char(o, '-');
    }
    for (size_t i = 0; i < np; i++) {
        out_char(o, prefix[i]);
    }
    if (zero_pad) {                    /* zéros entre le signe/préfixe et les chiffres */
        out_repeat(o, '0', padding);
    }
    out_repeat(o, '0', (int)prec_zeros);
    for (size_t i = 0; i < nd; i++) {
        out_char(o, digits[i]);
    }
    if (sp->left) {
        out_repeat(o, ' ', padding);
    }
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            out_char(&o, *p);
            continue;
        }
        const char *start = p;   /* pour recopier une conversion inconnue */
        p++;

        struct spec sp = { 0, 0, 0, -1 };
        for (;; p++) {           /* drapeaux */
            if (*p == '-') {
                sp.left = 1;
            } else if (*p == '0') {
                sp.zero = 1;
            } else {
                break;
            }
        }
        if (*p == '*') {         /* largeur */
            sp.width = va_arg(ap, int);
            if (sp.width < 0) {  /* C11 : largeur négative = '-' + |largeur| */
                sp.left = 1;
                sp.width = (sp.width == INT32_MIN) ? 0 : -sp.width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                if (sp.width < 100000) {           /* borne : pas de débordement d'int */
                    sp.width = sp.width * 10 + (*p - '0');
                }
                p++;
            }
        }
        if (*p == '.') {         /* précision */
            p++;
            sp.precision = 0;
            if (*p == '*') {
                sp.precision = va_arg(ap, int);
                if (sp.precision < 0) {   /* C11 : précision négative = absente */
                    sp.precision = -1;
                }
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    if (sp.precision < 100000) {
                        sp.precision = sp.precision * 10 + (*p - '0');
                    }
                    p++;
                }
            }
        }
        int lng = 0;             /* 0 = int, 1 = long, 2 = long long, 3 = size_t */
        if (*p == 'l') {
            lng = 1;
            p++;
            if (*p == 'l') {
                lng = 2;
                p++;
            }
        } else if (*p == 'z') {
            lng = 3;
            p++;
        }

        switch (*p) {
        case 'c': {
            char c = (char)va_arg(ap, int);
            out_padded(&o, &sp, &c, 1, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            size_t n = (sp.precision >= 0) ? strnlen(s, (size_t)sp.precision) : strlen(s);
            out_padded(&o, &sp, s, n, ' ');
            break;
        }
        case 'd':
        case 'i': {
            int64_t v;
            if (lng == 2) {
                v = va_arg(ap, long long);
            } else if (lng == 1) {
                v = va_arg(ap, long);
            } else if (lng == 3) {
                v = (int64_t)va_arg(ap, size_t);
            } else {
                v = va_arg(ap, int);
            }
            /* Magnitude calculée en non signé : -INT64_MIN déborderait en signé. */
            uint64_t mag = (v < 0) ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            out_number(&o, &sp, mag, v < 0, 10, 0, NULL);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v;
            if (lng == 2) {
                v = va_arg(ap, unsigned long long);
            } else if (lng == 1) {
                v = va_arg(ap, unsigned long);
            } else if (lng == 3) {
                v = va_arg(ap, size_t);
            } else {
                v = va_arg(ap, unsigned int);
            }
            out_number(&o, &sp, v, 0, (*p == 'u') ? 10u : 16u, *p == 'X', NULL);
            break;
        }
        case 'p': {
            /* SAMOS : toujours 0x suivi de 8 chiffres sur i386 (format fixe,
             * différent de la glibc qui supprime les zéros de tête). */
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            struct spec ps = { sp.left, 1, (int)(2 + 2 * sizeof(uintptr_t)), -1 };
            out_number(&o, &ps, v, 0, 16, 0, "0x");
            break;
        }
        case '%':
            out_char(&o, '%');
            break;
        default:
            /* Conversion inconnue (ou fin de chaîne après '%') : recopier le
             * texte brut. Ne consomme aucun argument. */
            for (const char *q = start; q <= p && *q != '\0'; q++) {
                out_char(&o, *q);
            }
            if (*p == '\0') {
                p--;             /* la boucle externe ne doit pas dépasser la fin */
            }
            break;
        }
    }

    if (size != 0) {
        o.buf[(o.len < size) ? o.len : size - 1] = '\0';
    }
    /* La longueur produite tient dans un int pour toute utilisation réaliste ;
     * on sature par sécurité. */
    return (o.len > (size_t)INT32_MAX) ? INT32_MAX : (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
