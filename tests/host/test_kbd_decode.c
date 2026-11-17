/*
 * SAMOS / AtlasOS — tests/host/test_kbd_decode.c
 * Tests de l'automate kernel/drivers/kbd_decode.c (pur, sans E/S).
 */
#include <samos/kbd_decode.h>
#include <string.h>
#include "check.h"

/* Envoie une suite d'octets et concatène les caractères produits. */
static void feed(struct kbd_decoder *d, const unsigned char *seq, size_t n, char *out, size_t cap)
{
    size_t k = 0;
    struct key_event ev;
    for (size_t i = 0; i < n; i++) {
        if (kbd_decode(d, seq[i], &ev) && ev.pressed && ev.ascii && k + 1 < cap) {
            out[k++] = ev.ascii;
        }
    }
    out[k] = '\0';
}

int main(void)
{
    struct kbd_decoder d;
    struct key_event ev;
    char out[64];

    kbd_decoder_init(&d);
    /* "hi" : h make/break, i make/break */
    const unsigned char hi[] = { 0x23, 0xA3, 0x17, 0x97 };
    feed(&d, hi, sizeof hi, out, sizeof out);
    CHECK(strcmp(out, "hi") == 0);

    /* Shift + 1 => '!', Shift relâché => '1' */
    const unsigned char bang[] = { 0x2A, 0x02, 0x82, 0xAA, 0x02, 0x82 };
    feed(&d, bang, sizeof bang, out, sizeof out);
    CHECK(strcmp(out, "!1") == 0);

    /* Verr. Maj : lettres en majuscules, chiffres inchangés ; Shift + Caps => minuscule */
    const unsigned char caps[] = { 0x3A, 0xBA, 0x1E, 0x9E, 0x02, 0x82, 0x2A, 0x1E, 0x9E, 0xAA, 0x3A, 0xBA, 0x1E };
    feed(&d, caps, sizeof caps, out, sizeof out);
    CHECK(strcmp(out, "A1aa") == 0);

    /* Ctrl + C => 0x03 */
    kbd_decoder_init(&d);
    CHECK(kbd_decode(&d, 0x1D, &ev) && (ev.mods & KMOD_CTRL));
    CHECK(kbd_decode(&d, 0x2E, &ev) && ev.ascii == 0x03);
    CHECK(kbd_decode(&d, 0x9D, &ev) && !(ev.mods & KMOD_CTRL));

    /* Touches étendues : flèches, Ctrl droit (E0 1D) distinct du gauche */
    kbd_decoder_init(&d);
    CHECK(!kbd_decode(&d, 0xE0, &ev));
    CHECK(kbd_decode(&d, 0x4B, &ev) && ev.keycode == KEY_LEFT && ev.pressed && ev.ascii == 0);
    CHECK(!kbd_decode(&d, 0xE0, &ev));
    CHECK(kbd_decode(&d, 0xCB, &ev) && ev.keycode == KEY_LEFT && !ev.pressed);
    CHECK(!kbd_decode(&d, 0xE0, &ev) && kbd_decode(&d, 0x1D, &ev) && ev.keycode == KEY_RCTRL);
    CHECK(ev.mods & KMOD_CTRL);
    /* Le préfixe ne « fuit » pas sur la touche suivante */
    CHECK(kbd_decode(&d, 0xE0, &ev) == false && kbd_decode(&d, 0x9D, &ev) && !(ev.mods & KMOD_CTRL));
    CHECK(kbd_decode(&d, 0x1E, &ev) && ev.keycode == 0x1E && ev.ascii == 'a');

    /* Shift fantôme E0 2A ignoré : ne change pas l'état Shift */
    CHECK(!kbd_decode(&d, 0xE0, &ev) && !kbd_decode(&d, 0x2A, &ev));
    CHECK(kbd_decode(&d, 0x1E, &ev) && ev.ascii == 'a');

    /* Pause : E1 1D 45 E1 9D C5 ne produit aucun événement ni état résiduel */
    const unsigned char pause[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
    int events = 0;
    for (size_t i = 0; i < sizeof pause; i++) events += kbd_decode(&d, pause[i], &ev);
    CHECK(events == 0);
    CHECK(kbd_decode(&d, 0x1E, &ev) && ev.ascii == 'a' && ev.mods == 0);

    /* Octets de contrôle ignorés ; codes hors table : événement sans caractère */
    CHECK(!kbd_decode(&d, 0xFA, &ev) && !kbd_decode(&d, 0xFE, &ev) && !kbd_decode(&d, 0x00, &ev));
    CHECK(kbd_decode(&d, 0x58, &ev) && ev.ascii == 0);          /* F12 */

    /* Robustesse : les 256 valeurs, dans tous les états, sans faute (ASan/UBSan) */
    for (int pre = 0; pre < 256; pre++) {
        for (int b = 0; b < 256; b++) {
            kbd_decoder_init(&d);
            (void)kbd_decode(&d, (unsigned char)pre, &ev);
            (void)kbd_decode(&d, (unsigned char)b, &ev);
        }
    }
    CHECK(1);
    return CHECK_REPORT("host kbd_decode");
}
