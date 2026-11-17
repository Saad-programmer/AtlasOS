/*
 * SAMOS / AtlasOS — kernel/include/samos/qemu_dbg.h
 * ---------------------------------------------------------------------------
 * Canal de diagnostic minimal propre aux émulateurs (QEMU/Bochs) :
 *
 *  - « debugcon » : chaque octet écrit sur le port d'E/S 0xE9 est transmis à
 *    l'hôte (option QEMU `-debugcon file:...` ou `-debugcon stdio`). Aucun
 *    initialisation n'est nécessaire : c'est le premier moyen d'observation
 *    disponible, avant même le pilote VGA ou série.
 *
 *  - « isa-debug-exit » : périphérique QEMU optionnel
 *    (`-device isa-debug-exit,iobase=0xf4,iosize=0x04`). Écrire la valeur v
 *    sur son port termine QEMU avec le code de sortie (v << 1) | 1. Utilisé
 *    UNIQUEMENT en mode test pour transmettre PASS/FAIL au script de test.
 *
 * Sur du matériel réel, ces ports ne correspondent normalement à aucun
 * périphérique ; les écritures sont ignorées. Le canal debugcon peut être
 * désactivé à la compilation (CONFIG_QEMU_DEBUGCON=0).
 */
#ifndef SAMOS_QEMU_DBG_H
#define SAMOS_QEMU_DBG_H

#include <stdint.h>
#include <samos/compiler.h>

#define QEMU_DEBUGCON_PORT   0xE9
#define QEMU_DEBUG_EXIT_PORT 0xF4

/* Valeurs écrites vers isa-debug-exit. Statut du processus QEMU = (v<<1)|1.
 * Volontairement différentes de 0 : un statut 1 est aussi le code d'erreur
 * générique de QEMU, il ne doit pas pouvoir signifier « succès ». */
#define QEMU_EXIT_SUCCESS 0x10   /* statut QEMU 33 */
#define QEMU_EXIT_FAILURE 0x11   /* statut QEMU 35 */
#define QEMU_EXIT_PANIC   0x12   /* statut QEMU 37 */

void qemu_dbg_putc(char c);
void qemu_dbg_puts(const char *s);
void qemu_dbg_put_hex32(uint32_t v);
void qemu_dbg_put_dec32(uint32_t v);

/* Termine QEMU si isa-debug-exit est présent ; sinon arrête le CPU.
 * Défini UNIQUEMENT dans l'image de test (CONFIG_KTEST = 1). */
SAMOS_NORETURN void qemu_exit(uint32_t code);

#endif /* SAMOS_QEMU_DBG_H */
