/*
 * SAMOS / AtlasOS — kernel/include/samos/panic.h
 * ---------------------------------------------------------------------------
 * Arrêt fatal du noyau (K0.3). Une « panique » est déclenchée lorsqu'un
 * invariant est violé et qu'il n'existe aucune manière sûre de continuer.
 *
 * panic() :
 *   1. masque les interruptions (aucun code ne doit s'exécuter « par-dessus »),
 *   2. détecte une panique récursive (panic pendant panic) et se réduit alors
 *      au strict minimum,
 *   3. affiche le message, le fichier/la ligne et une trace de pile (chaîne
 *      des EBP) sur tous les puits de journalisation,
 *   4. en mode test, termine QEMU avec QEMU_EXIT_PANIC ; sinon arrête le CPU.
 */
#ifndef SAMOS_PANIC_H
#define SAMOS_PANIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <samos/compiler.h>

SAMOS_NORETURN void panic_at(const char *file, int line, const char *fmt, ...) SAMOS_PRINTF(3, 4);

#define panic(...) panic_at(__FILE__, __LINE__, __VA_ARGS__)

/* Assertion du noyau : toujours active (pas de NDEBUG) — un invariant violé
 * dans un noyau est un défaut grave, pas une information de débogage. */
#define KASSERT(cond)                                              \
    do {                                                           \
        if (!(cond)) {                                             \
            panic("assertion failed: %s", #cond);                  \
        }                                                          \
    } while (0)

/* En mode test, la panique termine QEMU (statut 37) au lieu d'arrêter le CPU. */
void panic_set_qemu_exit(bool enabled);

/*
 * Parcourt la chaîne des cadres de pile à partir de `ebp` et écrit au plus
 * `max` adresses de retour dans out[]. Chaque cadre est validé (dans la pile
 * de démarrage, aligné sur 4, strictement croissant) : une pile corrompue
 * arrête le parcours au lieu de provoquer une faute.
 * Retourne le nombre d'adresses écrites.
 */
size_t stack_collect(uintptr_t ebp, uintptr_t *out, size_t max);

#endif /* SAMOS_PANIC_H */
