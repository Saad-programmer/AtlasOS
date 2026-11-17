/*
 * SAMOS / AtlasOS — tests/host/test_string.c
 * Tests unitaires hébergés de kernel/lib/string.c, compilés avec
 * AddressSanitizer + UndefinedBehaviorSanitizer : tout accès hors limites
 * (par exemple une lecture au-delà de `max` dans strnlen) fait échouer le test.
 */
#include <samos/string.h>
#include <stdlib.h>
#include "check.h"

int main(void)
{
    /* strnlen ne doit pas lire au-delà de max : tampon exact SANS '\0'. */
    char *exact = malloc(4);
    exact[0] = 'a'; exact[1] = 'b'; exact[2] = 'c'; exact[3] = 'd';
    CHECK(strnlen(exact, 4) == 4);
    free(exact);

    char buf[32];
    CHECK(memset(buf, 0x5A, sizeof buf) == buf);
    for (size_t i = 0; i < sizeof buf; i++) CHECK(buf[i] == 0x5A);

    memcpy(buf, "0123456789", 11);
    memmove(buf + 3, buf, 7);                 /* destination après la source */
    CHECK(memcmp(buf, "0120123456", 10) == 0);
    memcpy(buf, "0123456789", 11);
    memmove(buf, buf + 3, 7);                 /* destination avant la source */
    CHECK(memcmp(buf, "3456789789", 10) == 0);
    CHECK(memmove(buf, buf, 5) == buf);
    CHECK(memmove(buf, buf + 1, 0) == buf);

    CHECK(memcmp("a", "b", 0) == 0);
    CHECK(memcmp("\xff", "\x01", 1) > 0);     /* comparaison non signée */
    CHECK(strcmp("", "") == 0);
    CHECK(strcmp("ab", "abc") < 0);
    CHECK(strcmp("\xff", "a") > 0);
    CHECK(strncmp("abc", "abd", 2) == 0);
    CHECK(strncmp("ab", "ab\0x", 4) == 0);
    CHECK(strlen("") == 0);

    char small[4];
    CHECK(strlcpy(small, "abcdef", sizeof small) == 6);
    CHECK(strcmp(small, "abc") == 0);
    CHECK(strlcpy(small, "", sizeof small) == 0 && small[0] == '\0');
    CHECK(strlcpy(small, "xyz", 0) == 3);     /* cap = 0 : aucune écriture */

    return CHECK_REPORT("host string");
}
