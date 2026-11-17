/*
 * SAMOS / AtlasOS — kernel/include/samos/kbd_decode.h
 * ---------------------------------------------------------------------------
 * Décodeur de codes de balayage (scancodes) PS/2 « jeu 1 » (K0.5).
 *
 * Le contrôleur 8042 traduit par défaut le jeu 2 émis par le clavier en jeu 1
 * (bit 6 de l'octet de configuration). Jeu 1 :
 *   - « make » (appui)    : code c dans 0x01..0x58
 *   - « break » (relâche) : c | 0x80
 *   - préfixe 0xE0 : la touche suivante est « étendue » (flèches, Ctrl droit...)
 *   - préfixe 0xE1 : séquence Pause (E1 1D 45 E1 9D C5), ignorée
 *
 * Le décodeur est une machine à états PURE (aucune E/S) : il est testé sur
 * l'hôte. Disposition : US QWERTY (disposition par défaut de QEMU). Une table
 * AZERTY pourra être ajoutée sans changer l'automate.
 */
#ifndef SAMOS_KBD_DECODE_H
#define SAMOS_KBD_DECODE_H

#include <stdbool.h>
#include <stdint.h>

/* Codes de touche : code jeu 1 pour les touches simples, 0x100 | code pour
 * les touches étendues. */
#define KEY_EXT          0x100u
#define KEY_ESC          0x01u
#define KEY_BACKSPACE    0x0Eu
#define KEY_TAB          0x0Fu
#define KEY_ENTER        0x1Cu
#define KEY_LCTRL        0x1Du
#define KEY_LSHIFT       0x2Au
#define KEY_RSHIFT       0x36u
#define KEY_LALT         0x38u
#define KEY_CAPSLOCK     0x3Au
#define KEY_RCTRL        (KEY_EXT | 0x1Du)
#define KEY_RALT         (KEY_EXT | 0x38u)
#define KEY_UP           (KEY_EXT | 0x48u)
#define KEY_LEFT         (KEY_EXT | 0x4Bu)
#define KEY_RIGHT        (KEY_EXT | 0x4Du)
#define KEY_DOWN         (KEY_EXT | 0x50u)

#define KMOD_SHIFT  (1u << 0)
#define KMOD_CTRL   (1u << 1)
#define KMOD_ALT    (1u << 2)
#define KMOD_CAPS   (1u << 3)   /* verrouillage majuscules actif */

struct key_event {
    uint16_t keycode;
    bool pressed;               /* true = appui, false = relâchement */
    char ascii;                 /* 0 si la touche ne produit pas de caractère */
    uint8_t mods;               /* modificateurs actifs APRÈS cet événement */
};

struct kbd_decoder {
    bool extended;              /* 0xE0 reçu */
    uint8_t skip;               /* octets restants à ignorer (séquence Pause) */
    bool lshift, rshift, lctrl, rctrl, lalt, ralt, caps;
};

void kbd_decoder_init(struct kbd_decoder *d);

/* Consomme un octet. Retourne true et remplit *ev si une touche a été
 * décodée ; false pour un préfixe, un octet ignoré ou inconnu. */
bool kbd_decode(struct kbd_decoder *d, uint8_t scancode, struct key_event *ev);

#endif /* SAMOS_KBD_DECODE_H */
