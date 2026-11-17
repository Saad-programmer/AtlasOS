/*
 * SAMOS / AtlasOS — kernel/drivers/kbd_decode.c
 * Automate de décodage des scancodes PS/2 jeu 1. Voir kbd_decode.h et
 * Details.md §30.
 */
#include <samos/kbd_decode.h>

/* Table US QWERTY, jeu 1, index = code make (0x00..0x39). 0 = pas de caractère. */
static const char g_map_normal[0x3A] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};
static const char g_map_shift[0x3A] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

void kbd_decoder_init(struct kbd_decoder *d)
{
    *d = (struct kbd_decoder){ 0 };
}

static uint8_t current_mods(const struct kbd_decoder *d)
{
    uint8_t m = 0;
    if (d->lshift || d->rshift) m |= KMOD_SHIFT;
    if (d->lctrl || d->rctrl)   m |= KMOD_CTRL;
    if (d->lalt || d->ralt)     m |= KMOD_ALT;
    if (d->caps)                m |= KMOD_CAPS;
    return m;
}

static bool is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool kbd_decode(struct kbd_decoder *d, uint8_t sc, struct key_event *ev)
{
    if (d->skip > 0) {                  /* fin d'une séquence Pause */
        d->skip--;
        return false;
    }
    switch (sc) {
    case 0xE0:
        d->extended = true;
        return false;
    case 0xE1:
        d->skip = 2;                    /* E1 1D 45 (appui) ; E1 9D C5 (relâche) */
        return false;
    case 0x00: case 0xEE: case 0xFA: case 0xFE: case 0xFF:
        /* Octets de contrôle du clavier, pas des touches :
         *   0x00 / 0xFF : erreur ou débordement du tampon interne du clavier
         *   0xEE : réponse à ECHO ; 0xFA : ACK ; 0xFE : RESEND
         * Ils ne sont ambigus avec aucune touche : ce seraient les codes de
         * relâchement de 0x6E, 0x7A, 0x7E, 0x7F, qui n'existent pas en jeu 1.
         * (0xAA, « auto-test réussi », coïncide avec le relâchement de Shift
         * gauche : il est traité comme tel, ce qui est sans effet.) */
        d->extended = false;
        return false;
    default:
        break;
    }

    bool pressed = (sc & 0x80u) == 0;
    uint8_t code = (uint8_t)(sc & 0x7Fu);
    uint16_t keycode = d->extended ? (uint16_t)(KEY_EXT | code) : code;
    bool ext = d->extended;
    d->extended = false;

    /* Shift « fantôme » : certains claviers entourent les touches étendues de
     * E0 2A / E0 AA ; ce n'est pas un vrai Shift, on l'ignore. */
    if (ext && (code == 0x2A || code == 0x36)) {
        return false;
    }

    switch (keycode) {
    case KEY_LSHIFT: d->lshift = pressed; break;
    case KEY_RSHIFT: d->rshift = pressed; break;
    case KEY_LCTRL:  d->lctrl = pressed; break;
    case KEY_RCTRL:  d->rctrl = pressed; break;
    case KEY_LALT:   d->lalt = pressed; break;
    case KEY_RALT:   d->ralt = pressed; break;
    case KEY_CAPSLOCK:
        if (pressed) {
            d->caps = !d->caps;          /* bascule à l'appui uniquement */
        }
        break;
    default:
        break;
    }

    char ascii = 0;
    if (pressed && !ext && code < sizeof(g_map_normal)) {
        bool shift = d->lshift || d->rshift;
        ascii = shift ? g_map_shift[code] : g_map_normal[code];
        if (d->caps && is_letter(ascii)) {
            ascii = (char)(ascii ^ 0x20);   /* Verr. Maj inverse la casse des lettres seulement */
        }
        if ((d->lctrl || d->rctrl) && is_letter(ascii)) {
            ascii = (char)(ascii & 0x1F);   /* Ctrl+lettre : caractère de contrôle (Ctrl+C = 0x03) */
        }
    }

    ev->keycode = keycode;
    ev->pressed = pressed;
    ev->ascii = ascii;
    ev->mods = current_mods(d);
    return true;
}
