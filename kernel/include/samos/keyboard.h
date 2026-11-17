/*
 * SAMOS / AtlasOS — kernel/include/samos/keyboard.h
 * ---------------------------------------------------------------------------
 * Contrôleur PS/2 Intel 8042 et clavier (K0.5).
 *
 * Chaîne de traitement :
 *   IRQ 1 -> keyboard_irq() lit UN octet sur le port 0x60 et le dépose dans
 *            une file circulaire (aucun décodage en interruption)
 *   code normal -> keyboard_get_event() retire les octets et les fait passer
 *            dans l'automate kbd_decode (hors interruption)
 */
#ifndef SAMOS_KEYBOARD_H
#define SAMOS_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>
#include <samos/kbd_decode.h>

/* Initialise le 8042 (auto-test, test d'interface), réinitialise le
 * clavier, puis enregistre l'IRQ 1. Retourne false en cas d'échec ;
 * keyboard_error() indique l'étape fautive. */
bool keyboard_init(void);
const char *keyboard_error(void);

/* Retire et décode les octets reçus ; true si un événement est disponible. */
bool keyboard_get_event(struct key_event *ev);

uint32_t keyboard_bytes_received(void);
uint32_t keyboard_dropped(void);

#endif /* SAMOS_KEYBOARD_H */
