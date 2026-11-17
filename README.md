# SAMOS / AtlasOS

> Un système d'exploitation expérimental, écrit à partir de zéro en C et en assembleur x86, conçu à terme pour être piloté par la voix grâce à une intelligence artificielle développée elle aussi de façon indépendante.

SAMOS (également appelé **AtlasOS**) s'inspire de SAM, l'intelligence artificielle du jeu vidéo *Observation* (2019). Il ne reprend aucun code de ce jeu. L'objectif est de construire un vrai système, dans les limites des technologies actuelles.

Projet académique et de recherche mené par un étudiant ingénieur du cycle **ILISI** de la FST Mohammedia (Maroc).

---

## 1. Vision et motivation

Le but n'est pas d'assembler des bibliothèques existantes. Il s'agit de **comprendre et d'implémenter personnellement** chaque mécanisme : le démarrage, la mémoire, les interruptions, les processus, les pilotes, le système de fichiers, puis le moteur numérique, les réseaux de neurones, la reconnaissance et la synthèse vocales.

À terme, SAMOS devra :

- démarrer sur une machine physique ou virtuelle sans Linux ni Windows ;
- isoler les programmes dans des espaces mémoire séparés ;
- comprendre des commandes vocales en langage naturel et planifier des opérations système ;
- expliquer ses décisions et diagnostiquer son propre fonctionnement ;
- fonctionner **hors ligne**, et rester **sûr et opérationnel même si l'IA est indisponible**.

## 2. Objectifs à long terme

| Axe | Objectif |
|---|---|
| A — Noyau | Noyau monolithique modulaire indépendant, cible finale x86-64 |
| B — Voix | Capture audio, reconnaissance vocale, synthèse vocale, sans moteur existant |
| C — IA | Algèbre linéaire, tenseurs, rétropropagation, transformers, inférence, écrits en C/C++ |
| D — Sécurité | L'IA n'est jamais une autorité de confiance ; validation déterministe des actions, capacités, audit |
| E — Hors ligne | Aucune dépendance obligatoire à un service distant |
| F — Rigueur académique | Chaque mécanisme est documenté et explicable (voir `Details.md`) |
| G — Fiabilité | Builds reproductibles, tests automatisés, diagnostics |

## 3. Phase actuelle

**SAMOS-K0 : reconstruction du noyau et fondations** (x86 32 bits, à but pédagogique).

| Jalon | Contenu | État |
|---|---|---|
| K0.1 | Multiboot, point d'entrée, pile, passage à `kmain` | ✅ implémenté et vérifié |
| K0.2 | Script de liaison, sections ELF, disposition mémoire | ✅ implémenté et vérifié |
| K0.3 | VGA texte, port série COM1, journalisation, `panic` | ⏳ à venir |
| K0.4 | GDT, IDT, exceptions CPU | ⏳ à venir |
| K0.5 | PIC 8259, PIT, clavier PS/2, file circulaire | ⏳ à venir |
| K0.6 | Carte mémoire, allocateur physique, pagination, protections | ⏳ à venir |

Après K0.6, le développement fonctionnel s'arrête pour une revue complète, avant toute phase K1 (x86-64, processus, ordonnanceur, appels système).

## 4. Vue d'ensemble de l'architecture

```text
 Ring 3 (futur) : [Agent IA] → [Planificateur] → [Validateur déterministe] → [API système]
                                                         │ appels système
 Ring 0         : ┌───────────────────────────────────────────────────────┐
                  │ core/     kmain, bootinfo, (panic, log)                │
                  │ mm/       (carte mémoire, frames, pagination)  — K0.6  │
                  │ drivers/  debugcon QEMU, (VGA, série, clavier)         │
                  │ arch/i386 entry.asm, linker.ld, io.h, cpu.h, (GDT/IDT) │
                  └───────────────────────────────────────────────────────┘
                                         Matériel / QEMU
```

Tout le code spécifique au processeur se trouve dans `kernel/arch/i386/`. La migration vers x86-64 consistera à ajouter `kernel/arch/x86_64/`.

## 5. Composants implémentés

| Composant | Fichier(s) | Description |
|---|---|---|
| En-tête Multiboot 1 | `kernel/arch/i386/boot/entry.asm` | magic `0x1BADB002`, drapeaux alignement + info mémoire |
| Point d'entrée `_start` | `kernel/arch/i386/boot/entry.asm` | pile de 16 KiB alignée sur 16, EFLAGS remis à 0, `.bss` effacée, appel cdecl de `kmain`, boucle `cli/hlt` |
| Script de liaison | `kernel/arch/i386/linker.ld` | chargement à 1 MiB, 3 segments R-X / R-- / RW-, sections alignées sur 4 KiB, symboles de disposition, assertions de liaison |
| Disposition mémoire | `kernel/core/layout.c` | table des régions du noyau et de leurs droits prévus, vérifiée à chaque démarrage |
| Pile instrumentée | `entry.asm`, `layout.c` | section `.bootstack` précédée d'une zone de garde de 4 KiB ; peinture pour mesurer la profondeur maximale et détecter un débordement |
| Validation du démarrage | `kernel/core/bootinfo.c` | magic, bornes des adresses Multiboot, copie bornée de la ligne de commande |
| Canal de diagnostic | `kernel/drivers/qemu_dbg.c` | port debugcon `0xE9`, sortie `isa-debug-exit` |
| Bibliothèque minimale | `kernel/lib/string.c` | `memset`, `memcpy`, `memmove`, `memcmp`, `strlen`, `strnlen`, `strcmp`, `strncmp`, `strlcpy` |
| Autotests noyau | `kernel/tests/` | 60 vérifications exécutées dans le noyau sous QEMU |

## 6. Composants en cours de développement

- K0.3 : affichage VGA, port série COM1, journalisation, `panic`.

## 7. Composants planifiés

K0.3 à K0.6 (voir le tableau ci-dessus). Plus tard, sous réserve d'autorisation : x86-64, TSS/IST, Ring 3, processus, ordonnanceur, appels système, IPC, VFS, pilotes de stockage et audio, moteur numérique, reconnaissance et synthèse vocales, agent.

## 8. Structure du dépôt

```text
AtlasOS/
├── Makefile                  build, exécution, tests
├── kernel/
│   ├── arch/i386/            code dépendant du CPU
│   │   ├── boot/entry.asm    en-tête Multiboot + _start
│   │   ├── linker.ld         disposition de l'image
│   │   └── include/arch/     io.h (ports d'E/S), cpu.h (registres de contrôle)
│   ├── core/                 kmain.c, bootinfo.c, layout.c
│   ├── drivers/              qemu_dbg.c
│   ├── lib/                  string.c
│   ├── include/samos/        en-têtes internes du noyau
│   └── tests/                autotests exécutés dans le noyau
├── tests/
│   ├── tools/                tests négatifs des outils de vérification
│   ├── host/                 tests unitaires hébergés (ASan + UBSan)
│   └── qemu/                 tests de démarrage QEMU et vérifications GDB
├── tools/check_elf.sh        vérifications statiques de l'image ELF
├── docs/adr/                 décisions d'architecture
├── README.md  Walkthrough.md  Details.md  CLAUDE.md
```

## 9. Environnement de développement

Le développement se fait sous Linux (Zorin OS / Ubuntu 24.04). Versions vérifiées dans l'environnement d'intégration :

| Outil | Version |
|---|---|
| Clang | 18.1.3 |
| NASM | 2.16.01 |
| GNU ld (Binutils) | 2.42 |
| QEMU (`qemu-system-i386`) | 8.2.2 |
| GDB | 15.1 |
| GCC (tests hébergés uniquement) | 13.3.0 |
| GNU Make | 4.3 |

## 10. Prérequis de compilation

```bash
sudo apt install clang nasm binutils make qemu-system-x86 gdb gcc
```

Aucun compilateur croisé n'est nécessaire : Clang génère du code pour la cible `i686-unknown-none-elf` grâce à l'option `--target`.

## 11. Compilation

```bash
make
```

Le résultat est `build/samos.elf` (ELF 32 bits, Multiboot 1). La carte de liaison est produite dans `build/samos.map`.

## 12. Exécution sous QEMU

```bash
make run
```

QEMU charge directement le noyau grâce à son chargeur Multiboot intégré (`-kernel`). Les messages du noyau s'affichent dans le terminal via le port debugcon. Sortie attendue :

```text
SAMOS: kernel entry (SAMOS/AtlasOS 0.1.0-k0)
SAMOS: multiboot magic ok, info at 0x00009500, flags 0x0000024f
SAMOS: cmdline "build/samos.elf "
SAMOS: kernel image 0x00100000-0x0010a000
SAMOS:   0x00100000-0x00102000 r-x  .text
SAMOS:   0x00102000-0x00103000 r--  .rodata
SAMOS:   0x00103000-0x00105000 rw-  .data+.bss
SAMOS:   0x00105000-0x00106000 ---G stack-guard
SAMOS:   0x00106000-0x0010a000 rw-  boot-stack
SAMOS: boot stack high-water 116 bytes, guard intact
SAMOS: K0.2 boot complete, halting
```

Quitter avec `Ctrl+C`. Débogage : `make debug`, puis dans un autre terminal `gdb build/samos.elf -ex 'target remote :1234'`.

## 13. Tests

```bash
make test
```

| Suite | Commande | Contenu |
|---|---|---|
| Statique ELF | `make check-elf` | ELF32/i386, entrée = `_start`, en-tête Multiboot (offset, alignement, somme), aucun symbole indéfini, aucun segment W+X, sections alignées/ordonnées/NOBITS, aucune section dynamique, absence d'auto-appel de `memset`/`memcpy` |
| Outils (négatif) | `make test-tools` | `check_elf.sh` doit rejeter une image à somme corrompue ou sans en-tête |
| Hébergés | `make test-host` | `string.c` compilé pour l'hôte avec AddressSanitizer + UBSan |
| QEMU | `make test-qemu` | démarrage normal (sans triple fault) et autotests noyau (`-append test`) |
| GDB | `make test-gdb` | état exact des registres à l'entrée de `kmain` ; test négatif du magic invalide |

Le noyau transmet son verdict à QEMU via le périphérique `isa-debug-exit` (statut 33 = succès, 35 = échec, 37 = panic).

## 14. Philosophie de sécurité

- **Le noyau ne fait confiance à aucune donnée externe.** Les informations Multiboot sont validées (bornes, débordement 32 bits, non-chevauchement avec le noyau), puis copiées avec une longueur bornée.
- Arrêt sûr et explicite en cas d'état invalide, sans exécution « au mieux ».
- Segments ELF séparés R-X / R-- / RW- dès K0.1. Leur application réelle par la pagination est prévue en K0.6.
- Les avertissements sont traités comme des erreurs (`-Werror`, `-w+error`, `--fatal-warnings`).
- L'IA ne s'exécutera jamais en Ring 0 et ne sera jamais une frontière de sécurité.
- Un test qui passe ne prouve pas l'absence de défaut. Aucune garantie de sécurité n'est revendiquée à ce stade.

## 15. Développement à partir de zéro

Seuls des outils de développement standard sont utilisés : compilateur, assembleur, éditeur de liens, émulateur, débogueur. Aucun code d'un autre noyau n'est copié. Les en-têtes freestanding du compilateur (`stdint.h`, `stddef.h`, `stdbool.h`, `stdarg.h`) sont autorisés. Ni libc ni libgcc ne sont liées.

## 16. Limitations actuelles

- Pas encore d'IDT : toute exception CPU provoquerait un triple fault.
- Pas d'affichage VGA ni de port série. Seule la sortie debugcon de QEMU existe, invisible sur du matériel réel.
- Pas de pagination : les permissions des segments ne sont pas appliquées par le CPU.
- La zone de garde de pile n'est vérifiée que par logiciel, a posteriori. La page de garde matérielle (non présente) viendra avec la pagination (K0.6).
- En i386 sans PAE, le matériel ne pourra pas rendre les données non exécutables (pas de bit NX).
- Testé uniquement sous QEMU. Sur un PC UEFI récent, le mode texte VGA ne sera pas disponible.

## 17. Feuille de route

`K0.1 ✅ → K0.2 ✅ → K0.3 → K0.4 → K0.5 → K0.6 → revue K0 → (autorisation) K1 x86-64 → processus/ordonnanceur → appels système → IPC → VFS → pilotes → A0 (IA) → V0 (voix) → I0 (agent)`

## 18. Documentation

- [`Walkthrough.md`](Walkthrough.md) : journal de développement reproductible, étape par étape.
- [`Details.md`](Details.md) : cours technique et académique sur les concepts implémentés.
- [`CLAUDE.md`](CLAUDE.md) : directive d'ingénierie du projet.

## 19. Références

- *Multiboot Specification*, version 0.6.96 (Free Software Foundation).
- *Intel® 64 and IA-32 Architectures Software Developer's Manual*, vol. 1, 2 et 3A.
- *System V Application Binary Interface, Intel386 Architecture Processor Supplement*, 4e éd.
- *Tool Interface Standard (TIS) Executable and Linking Format (ELF) Specification*, v1.2.
- Documentation de GNU ld (scripts de liaison), de NASM et de QEMU (`isa-debug-exit`, `debugcon`).
- A. S. Tanenbaum, H. Bos, *Modern Operating Systems*, 4e éd., Pearson.

## 20. Licence

**Aucune licence n'est encore établie.** Le choix appartient au développeur. Sans licence explicite, tous les droits sont réservés.
