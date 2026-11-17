# ADR 0001 — Fondations de la phase K0

- **Statut :** accepté par le développeur (2026-11-17)
- **Contexte :** reconstruction indépendante du noyau SAMOS en x86 32 bits, à but pédagogique, avant une migration planifiée vers x86-64.

## Décisions

| # | Décision | Justification | Alternative écartée |
|---|---|---|---|
| 1 | Claude implémente tout le code de K0 (jusqu'à K0.6), le développeur garde l'autorité architecturale | décision du développeur | développement fichier par fichier par le développeur |
| 2 | Clang `--target=i686-unknown-none-elf` | compilateur croisé natif, aucun build de toolchain | GCC hôte `-m32` (hypothèses Linux), `i686-elf-gcc` (à compiler) |
| 3 | Multiboot 1 | démarrage direct par `qemu -kernel`, tests automatiques simples | Multiboot 2 (GRUB + ISO nécessaires) |
| 4 | Pas de libgcc ; routines écrites seulement si nécessaires | indépendance, `nm -u` vide | lier libgcc |
| 5 | En-têtes freestanding `stdint/stddef/stdbool/stdarg` | types fournis par le compilateur, aucun code | les réécrire |
| 6 | Noyau lié et chargé à 1 MiB en identité | simplicité en K0 | higher-half (étudié à la migration x86-64) |
| 7 | GNU Make | simple, explicite | CMake |
| 8 | Pile alignée sur 16 (`-mstack-alignment=16`) | ABI i386 SysV/Linux, cohérence x86-64 ; Clang i686-elf ne garantit que 4 par défaut (mesuré) | alignement 4 |
| 9 | Canal debugcon 0xE9 + `isa-debug-exit` pour les tests | observation sans pilote, verdict automatique | analyse d'écran |
| 10 | Documentation en français | décision du développeur | — |
| 11 | Pas de licence | décision réservée au développeur | — |
| 12 | Ancien noyau non importé | nouvelle base de code indépendante | `legacy/` |

## Conséquences

- La migration x86-64 imposera de revoir le protocole de démarrage (Multiboot 1 ne charge pas d'ELF64).
- Le canal debugcon n'existe pas sur le matériel réel ; le port série (K0.3) le complète.
