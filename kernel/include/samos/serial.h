/*
 * SAMOS / AtlasOS — kernel/include/samos/serial.h
 * ---------------------------------------------------------------------------
 * Pilote du port série UART 16550 (COM1), en émission par scrutation (K0.3).
 *
 * Le port série est le canal de diagnostic le plus robuste : il fonctionne
 * sans affichage, est capturable par QEMU (`-serial file:` / `stdio`) et
 * existe sur de nombreuses machines réelles (connecteur ou en-tête interne).
 * Aucune interruption n'est utilisée (IER = 0) : réception et IRQ 4
 * viendront plus tard si nécessaire.
 */
#ifndef SAMOS_SERIAL_H
#define SAMOS_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SERIAL_COM1 0x3F8

/* Initialise COM1 en 115200 bauds, 8N1, FIFO activée, puis vérifie la
 * présence de l'UART par un test en boucle locale (loopback).
 * Retourne true si le port est fonctionnel. */
bool serial_init(void);
bool serial_available(void);
void serial_putc(char c);                 /* '\n' -> "\r\n" */
void serial_write(const char *s, size_t n);
/* Nombre de caractères abandonnés faute de place dans l'UART (délai dépassé). */
uint32_t serial_dropped(void);

#endif /* SAMOS_SERIAL_H */
