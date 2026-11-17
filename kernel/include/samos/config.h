/*
 * SAMOS / AtlasOS — kernel/include/samos/config.h
 * ---------------------------------------------------------------------------
 * Configuration de compilation (K0.7). Les macros sont fournies par le
 * Makefile, selon l'image construite :
 *
 *   CONFIG_KTEST          1 = image de TEST (autotests, modes de faute
 *                         volontaire, sortie QEMU par isa-debug-exit)
 *                         0 = image de PRODUCTION (aucun de ces mécanismes)
 *   CONFIG_QEMU_DEBUGCON  1 = écrire aussi le journal sur le port 0xE9 (QEMU)
 *
 * Aucune valeur par défaut : oublier une macro est une ERREUR de compilation,
 * pour qu'une image ne puisse pas recevoir silencieusement le code de test.
 */
#ifndef SAMOS_CONFIG_H
#define SAMOS_CONFIG_H

#ifndef CONFIG_KTEST
#error "CONFIG_KTEST must be defined by the build (0 = production, 1 = test)"
#endif
#ifndef CONFIG_QEMU_DEBUGCON
#error "CONFIG_QEMU_DEBUGCON must be defined by the build"
#endif

#endif /* SAMOS_CONFIG_H */
