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
| K0.3 | VGA texte, port série COM1, journalisation, `panic` | ✅ implémenté et vérifié |
| K0.4 | GDT, IDT, exceptions CPU | ✅ implémenté et vérifié |
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
| Canal de diagnostic QEMU | `kernel/drivers/qemu_dbg.c` | port debugcon `0xE9`, sortie `isa-debug-exit` |
| Pilote VGA texte | `kernel/drivers/vga.c` | 80×25, couleurs, défilement, tabulation, curseur matériel CRTC |
| Pilote série | `kernel/drivers/serial.c` | UART 16550 COM1 115200 8N1, test de présence et de boucle locale, attente bornée |
| Journalisation | `kernel/core/log.c` | `kprintf`/`klog` vers debugcon + série + VGA, troncature signalée |
| Panique | `kernel/core/panic.c` | message, fichier/ligne, trace de pile validée (chaîne EBP), garde de récursion, `KASSERT` |
| Formatage | `kernel/lib/format.c` | `ksnprintf` indépendant (`%c %s %d %i %u %x %X %p`, drapeaux, largeur, précision, `l/ll/z`), testé contre la libc |
| GDT et TSS | `kernel/arch/i386/gdt.c`, `gdt_flush.asm` | segments plats code/données noyau, TSS principal, TSS de double faute avec pile dédiée |
| IDT et exceptions | `kernel/arch/i386/idt.c`, `isr_stubs.asm`, `exceptions.c` | 32 portes d'exception, cadre uniforme, `cld` et réalignement ABI, gestionnaires enregistrables, rapport complet (code d'erreur décodé, registres, CR0/2/3) puis `panic` |
| Double faute | `exceptions.c`, `gdt.c` | porte de tâche vers une pile dédiée : survit à une pile noyau épuisée |
| Arithmétique 64 bits | `kernel/lib/math64.c` | division 64/32 sans libgcc (division longue binaire) |
| Bibliothèque minimale | `kernel/lib/string.c` | `memset`, `memcpy`, `memmove`, `memcmp`, `strlen`, `strnlen`, `strcmp`, `strncmp`, `strlcpy` |
| Autotests noyau | `kernel/tests/` | 127 vérifications exécutées dans le noyau sous QEMU, plus 8 démarrages volontairement fatals |

## 6. Composants en cours de développement

- K0.5 : PIC 8259, PIT, clavier PS/2, file circulaire.

## 7. Composants planifiés

K0.5 et K0.6 (voir le tableau ci-dessus). Plus tard, sous réserve d'autorisation : x86-64, TSS/IST, Ring 3, processus, ordonnanceur, appels système, IPC, VFS, pilotes de stockage et audio, moteur numérique, reconnaissance et synthèse vocales, agent.

## 8. Structure du dépôt

```text
AtlasOS/
├── Makefile                  build, exécution, tests
├── kernel/
│   ├── arch/i386/            code dépendant du CPU
│   │   ├── boot/entry.asm    en-tête Multiboot + _start
│   │   ├── linker.ld         disposition de l'image
│   │   ├── gdt.c, gdt_flush.asm      GDT et TSS
│   │   ├── idt.c, isr_stubs.asm      IDT et points d'entrée des interruptions
│   │   ├── exceptions.c      répartition, rapport d'exception, tâche de double faute
│   │   └── include/arch/     io.h, cpu.h, gdt.h, idt.h, interrupt.h
│   ├── core/                 kmain.c, bootinfo.c, layout.c, log.c, panic.c
│   ├── drivers/              qemu_dbg.c, vga.c, serial.c
│   ├── lib/                  string.c, format.c, math64.c
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

QEMU charge directement le noyau grâce à son chargeur Multiboot intégré (`-kernel`). Les messages du noyau arrivent dans le terminal par le **port série COM1**, et sont aussi copiés dans `build/debugcon.log` et sur l'écran VGA. Sortie attendue :

```text
SAMOS: kernel entry (SAMOS/AtlasOS 0.1.0-k0)
[info ] console: debugcon + serial COM1 115200 8N1 + VGA 80x25
[info ] cpu: GDT loaded (flat 4 GiB, TSS main + double-fault task)
[info ] cpu: IDT loaded (32 exception gates, #DF task gate)
SAMOS: multiboot magic ok, info at 0x00009500, flags 0x0000024f
SAMOS: cmdline "build/samos.elf "
SAMOS: kernel image 0x00100000-0x00110000
SAMOS:   0x00100000-0x00105000 r-x  .text
SAMOS:   0x00105000-0x00107000 r--  .rodata
SAMOS:   0x00107000-0x0010b000 rw-  .data+.bss
SAMOS:   0x0010b000-0x0010c000 ---G stack-guard
SAMOS:   0x0010c000-0x00110000 rw-  boot-stack
SAMOS: boot stack high-water 792 bytes, guard intact
SAMOS: K0.4 boot complete, halting
```

Quitter avec `Ctrl+C`. `make run-vga` ouvre en plus la fenêtre de l'écran VGA ; il faut un affichage graphique. En cas de panique, l'adresse de chaque cadre de la trace se traduit en nom de fonction avec `addr2line -f -e build/samos.elf 0x...`. Débogage : `make debug`, puis dans un autre terminal `gdb build/samos.elf -ex 'target remote :1234'`.

## 13. Tests

```bash
make test
```

| Suite | Commande | Contenu |
|---|---|---|
| Statique ELF | `make check-elf` | ELF32/i386, entrée = `_start`, en-tête Multiboot (offset, alignement, somme), aucun symbole indéfini, aucun segment W+X, sections alignées/ordonnées/NOBITS, aucune section dynamique, absence d'auto-appel de `memset`/`memcpy` |
| Outils (négatif) | `make test-tools` | `check_elf.sh` doit rejeter une image à somme corrompue ou sans en-tête |
| Hébergés | `make test-host` | `string.c`, `format.c` (comparaison différentielle avec `snprintf` de la libc) et `math64.c` (100 000 divisions comparées à la division native), sous AddressSanitizer + UBSan |
| QEMU | `make test-qemu` | démarrage normal (sans triple fault), sortie série de bout en bout, autotests noyau (`-append test`), chemin de panique (`test=panic`, statut 37), refus d'un mode de test inconnu, exceptions fatales `test=ud/de/gp/np/intbad`, double faute par porte de tâche (`test=df`), triple fault volontaire détecté (`test=triple`) |
| GDB | `make test-gdb` | état exact des registres à l'entrée de `kmain` ; test négatif du magic invalide ; lecture de la mémoire VGA `0xB8000` depuis l'extérieur du noyau |

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

- Les IRQ matérielles ne sont pas encore activées (PIC non programmé, vecteurs 32-47 non présents).
- NMI et Machine Check ne sont pas traités spécifiquement : ils mènent à une panique.
- Pas de segments ni de code ring 3 (prévu en K1).
- Le port série est en émission seule, par scrutation (pas d'IRQ 4, pas de réception).
- Le journal n'est pas réentrant : les futurs gestionnaires d'IRQ ne devront pas journaliser.
- Pas de pagination : les permissions des segments ne sont pas appliquées par le CPU.
- La zone de garde de pile n'est vérifiée que par logiciel, a posteriori. La page de garde matérielle (non présente) viendra avec la pagination (K0.6).
- En i386 sans PAE, le matériel ne pourra pas rendre les données non exécutables (pas de bit NX).
- Testé uniquement sous QEMU. Sur un PC UEFI récent, le mode texte VGA ne sera pas disponible.

## 17. Feuille de route

`K0.1 ✅ → K0.2 ✅ → K0.3 ✅ → K0.4 ✅ → K0.4 → K0.5 → K0.6 → revue K0 → (autorisation) K1 x86-64 → processus/ordonnanceur → appels système → IPC → VFS → pilotes → A0 (IA) → V0 (voix) → I0 (agent)`

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
- National Semiconductor, fiche technique PC16550D (UART).
- ISO/IEC 9899:2011 (C11), §7.21.6 (fonctions de formatage).
- A. S. Tanenbaum, H. Bos, *Modern Operating Systems*, 4e éd., Pearson.

## 20. Licence

**Aucune licence n'est encore établie.** Le choix appartient au développeur. Sans licence explicite, tous les droits sont réservés.
