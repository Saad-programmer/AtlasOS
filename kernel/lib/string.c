/*
 * SAMOS / AtlasOS — kernel/lib/string.c
 * ---------------------------------------------------------------------------
 * Implémentations simples et lisibles (octet par octet). La performance n'est
 * pas un objectif en K0 ; la correction et l'explicabilité le sont.
 *
 * Ce fichier ne dépend que de <stddef.h> : il est compilé aussi bien dans le
 * noyau que dans les tests hébergés (tests/host) sous AddressSanitizer.
 *
 * Note : le noyau est compilé avec -ffreestanding, qui implique -fno-builtin.
 * Cela empêche Clang de reconnaître la boucle de memset comme « un memset » et
 * de la remplacer par un appel récursif à memset (vérifié par désassemblage).
 */
#include <samos/string.h>

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    while (n--) {
        *d++ = (unsigned char)value;
    }
    return dst;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/* memmove gère le chevauchement : si la destination est après la source,
 * on copie depuis la fin pour ne pas écraser les octets pas encore lus. */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a;
    const unsigned char *q = b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) {
            return (int)p[i] - (int)q[i];
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        if (ca == '\0') {
            return 0;
        }
    }
    return 0;
}

size_t strlcpy(char *dst, const char *src, size_t cap)
{
    size_t len = strlen(src);
    if (cap > 0) {
        size_t n = (len < cap - 1) ? len : cap - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
