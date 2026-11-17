/*
 * SAMOS / AtlasOS — tests/host/test_format.c
 * Test DIFFÉRENTIEL de kernel/lib/format.c : pour chaque cas, la sortie de
 * ksnprintf doit être identique, octet par octet et en valeur de retour, à
 * celle du snprintf de la libc hôte (référence C11). Plus des cas propres à
 * SAMOS (%p, NULL, conversions inconnues, troncature). Exécuté sous ASan+UBSan.
 */
#include <samos/format.h>
#include <samos/math64.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "check.h"

/* Ce fichier teste VOLONTAIREMENT des formats inhabituels ou invalides (drapeaux
 * redondants, conversion inconnue, '%' final) : on désactive ici les
 * avertissements de format, qui sont actifs partout ailleurs. */
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-extra-args"
#pragma GCC diagnostic ignored "-Wformat-overflow"

static int g_diff;

#define SAME(fmt, ...) do {                                                   \
        char a[128], b[128];                                                  \
        int ra = ksnprintf(a, sizeof a, fmt, __VA_ARGS__);                     \
        int rb = snprintf(b, sizeof b, fmt, __VA_ARGS__);                      \
        g_host_checks++;                                                      \
        if (ra != rb || strcmp(a, b) != 0) {                                  \
            g_host_failures++; g_diff++;                                      \
            fprintf(stderr, "[FAIL] fmt \"%s\": samos=\"%s\"(%d) libc=\"%s\"(%d)\n", \
                    fmt, a, ra, b, rb);                                       \
        }                                                                     \
    } while (0)

int main(void)
{
    /* Entiers signés, extrêmes compris */
    SAME("%d", 0); SAME("%d", 42); SAME("%d", -42);
    SAME("%d", INT32_MAX); SAME("%d", INT32_MIN);
    SAME("%i|%5d|%-5d|%05d|%-05d", 7, -3, 12, -12, 9);
    SAME("%lld", (long long)INT64_MIN); SAME("%lld", (long long)INT64_MAX);
    SAME("%ld", -123456789L);
    SAME("%.5d|%8.3d|%-8.3d|%08.3d", -42, 7, 7, 7);
    SAME("[%.0d][%.0u][%5.0x]", 0, 0u, 0u);
    /* Non signés et hexadécimal */
    SAME("%u", 4294967295u); SAME("%x", 0xdeadbeefu); SAME("%X", 0xdeadbeefu);
    SAME("%08x", 0x1234u); SAME("%-8x|", 0xabu); SAME("%llu", 18446744073709551615ull);
    SAME("%llx", 0x0123456789abcdefull); SAME("%zu", (size_t)123456);
    /* Chaînes et caractères */
    SAME("%s", "atlas"); SAME("[%10s]", "sam"); SAME("[%-10s]", "sam");
    SAME("[%.2s]", "samos"); SAME("[%*s]", 6, "x"); SAME("[%-*s]", 6, "x");
    SAME("[%*s]", -6, "x"); SAME("[%.*s]", 3, "abcdef"); SAME("[%.*s]", -1, "abc");
    SAME("%c%c%c", 'O', 'S', '!'); SAME("[%3c]", 'z'); SAME("100%%%s", ""); SAME("%s", "");

    /* Spécificités SAMOS */
    char buf[64];
    /* %p : 0x + 2 chiffres par octet de pointeur (16 sur un hôte 64 bits, 8 sur i386) */
    CHECK(ksnprintf(buf, sizeof buf, "%p", (void *)0x1234) == (int)(2 + 2 * sizeof(void *)));
    ksnprintf(buf, sizeof buf, "%p", (void *)(uintptr_t)0xabc);
    CHECK(strncmp(buf, "0x0000", 6) == 0);            /* format fixe, zéros de tête */
    CHECK(ksnprintf(buf, sizeof buf, "%s", (char *)NULL) == 6 && strcmp(buf, "(null)") == 0);
    CHECK(ksnprintf(buf, sizeof buf, "a%qb") == 4 && strcmp(buf, "a%qb") == 0);  /* inconnue */
    CHECK(ksnprintf(buf, sizeof buf, "fin%") == 4 && strcmp(buf, "fin%") == 0);  /* '%' final */

    /* Troncature : jamais d'écriture au-delà de size, toujours terminé. */
    char small[5];
    memset(small, 'Z', sizeof small);
    CHECK(ksnprintf(small, sizeof small, "%s", "abcdefgh") == 8);
    CHECK(strcmp(small, "abcd") == 0);
    char one[1] = { 'Z' };
    CHECK(ksnprintf(one, 1, "%d", 12345) == 5 && one[0] == '\0');
    CHECK(ksnprintf(NULL, 0, "%d", 12345) == 5);       /* size 0 : aucune écriture */

    /* Division 64 bits : comparaison avec la division native de l'hôte. */
    uint64_t x = 0x9E3779B97F4A7C15ull;               /* générateur pseudo-aléatoire xorshift */
    for (int i = 0; i < 100000; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint32_t d = (uint32_t)(x >> 17) | 1u;
        if (i % 3 == 0) d = 10;
        uint32_t r;
        uint64_t q = u64_divmod_u32(x, d, &r);
        if (q != x / d || r != x % d) { CHECK(0); break; }
    }
    CHECK(u64_divmod_u32(UINT64_MAX, 1, NULL) == UINT64_MAX);
    uint32_t r0 = 99;
    CHECK(u64_divmod_u32(5, 0, &r0) == UINT64_MAX && r0 == 0);   /* division par 0 définie */

    if (g_diff) fprintf(stderr, "%d differences avec la libc\n", g_diff);
    return CHECK_REPORT("host format+math64");
}
