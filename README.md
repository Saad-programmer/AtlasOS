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
| K0.5 | PIC 8259, PIT, clavier PS/2, file circulaire | ✅ implémenté et vérifié |
| K0.6 | Carte mémoire, allocateur physique, pagination, protections | ✅ implémenté et vérifié |
| K0.7 | Durcissement : correction des défauts mémoire E1-E5, références des cadres, politique de `vmm_map`, canaris de pile, sentinelles et pages de garde des piles, outils de mutation et de reproductibilité | ✅ **K0.7 — CLOSED (scope-defined), 2026-11-20** : R1 et P4 clôturés dans leur périmètre ; reports explicites et risques résiduels ouverts ([`docs/review/K0.7-formal-closure.md`](docs/review/K0.7-formal-closure.md)) |

La phase K0 (K0.1 à K0.6) est terminée ; sa revue se trouve dans [`docs/review/K0-review.md`](docs/review/K0-review.md). **K0.7 — CLOSED (scope-defined), 2026-11-20** (sécurisation, sans nouvelle fonctionnalité). Ses sous-jalons clôturés, chacun **dans son périmètre** :

- **R1** (durcissement des piles) : [`docs/review/K0.7-R1-closure.md`](docs/review/K0.7-R1-closure.md) ;
- **P4** (protection des structures critiques et des tables de pages) : [`docs/review/K0.7-P4-closure.md`](docs/review/K0.7-P4-closure.md), décision A du développeur, avec **risques résiduels explicitement acceptés**.

État de P4 :

| Élément | État |
|---|---|
| IDT, gestionnaires, racines du VMM (P4d) ; répertoire et tables de pages (P4e-2) | en lecture seule dans l'identité ; écritures de PDE/PTE par le passage contrôlé `pt_gate_write` (fenêtre CR0.WP) |
| D1 (alias par falsification du PMM), D3 (substitution de PDE), D4 (échecs de création des tables), D2 (preuves d'INVLPG masquées sous QEMU) | corrigés (P4f-B1 à B4) |
| N1 (page de l'image publiée comme table), N2 (PDE vers `.text` ou l'IDT), N5 (audit aveugle) | corrigés (P4f-D), contre-vérifiés (P4f-E) |
| **N3** (libération d'un cadre de données encore projeté, avec des références falsifiées) | **corrigé dans le modèle actuel** (K1.0-N3, intégré dans `main` le 2026-11-19, `daaa7a8`) : relevé indépendant des PTE avant libération et remise à zéro, retrait sûr, audit ; garanties limitées à un espace d'adressage unique et monoprocesseur ([`docs/review/K1.0-N3-implementation.md`](docs/review/K1.0-N3-implementation.md)) |
| GDT/TSS inscriptibles ; NMI ou #MC pendant la fenêtre WP ; monoprocesseur ; matériel réel non testé | **risques acceptés, non corrigés** (registre : `K0.7-P4-closure.md` §7) |

**Une clôture de jalon ne signifie pas que le noyau est sécurisé.** SAMOS n'est ni un système d'exploitation complet ni un système sécurisé : c'est un noyau d'étude, testé sous QEMU.

**Branches.** `main` contient P4 (clôturé dans son périmètre) et la correction K1.0-N3 (`daaa7a8`, 2026-11-19). L'audit de clôture globale de K0.7 ([`docs/review/K0.7-final-closure-audit.md`](docs/review/K0.7-final-closure-audit.md)) conclut que le **périmètre de K0.7 doit être clarifié** avant sa clôture ; la clarification et le traitement des écarts ([`docs/review/K0.7-scope-resolution.md`](docs/review/K0.7-scope-resolution.md)) sont intégrés dans `main` (`94ae07b`, 2026-11-20). La clôture formelle ([`docs/review/K0.7-formal-closure.md`](docs/review/K0.7-formal-closure.md), verdict A approuvé par le développeur) est intégrée dans `main` (`edcd7f5`) : **K0.7 — CLOSED (scope-defined), 2026-11-20**. Cette clôture ne signifie pas que SAMOS est sécurisé, terminé ou validé sur matériel réel. **K1.1** : la revue de conception ([`docs/review/K1.1-design-review.md`](docs/review/K1.1-design-review.md), `63aca66`) est intégrée dans `main` ; le sous-jalon **M64-0** (infrastructure de test seulement : même image i386 validée sous `qemu-system-i386` et `qemu-system-x86_64`) est **intégré dans `main`** (`bbcc73a`, 2026-11-20, sur décision du développeur ; [`docs/review/K1.1-M64-0-report.md`](docs/review/K1.1-M64-0-report.md)). La conception commune M64-1/M64-2 ([`docs/review/K1.1-M64-1-design.md`](docs/review/K1.1-M64-1-design.md)) est intégrée (`5370fe5`). **M64-1** (construction double : un squelette ELF64 x86-64 **lié et inspecté statiquement, non amorçable**) est sur la branche `claude/k1.1-m64-1-build`, **en attente d'audit** ([`docs/review/K1.1-M64-1-report.md`](docs/review/K1.1-M64-1-report.md)). L'étiquette de référence annotée `ref/i386-k0.7` est **publiée sur GitHub** (objet `f671c13`, cible `7e43b78`, référence i386 de la décision D-REF). **SAMOS n'exécute aucun code 64 bits** : le squelette x86-64 se compile et se lie seulement ; aucun tremplin, aucun mode long (M64-2, non autorisé) ; préparation : [`docs/design/K1-readiness.md`](docs/design/K1-readiness.md).

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
| PIC 8259A | `kernel/arch/i386/pic.c` | remappage 0x20-0x2F, masquage par défaut, EOI, lecture ISR/IRR |
| Couche IRQ | `kernel/arch/i386/irq.c` | enregistrement (démasque + porte présente), EOI centralisé, IRQ parasites, compteurs |
| PIT 8254 | `kernel/drivers/pit.c` | 100 Hz (diviseur 11932), compteur 64 bits lu sans déchirure, temps écoulé exact, `pit_sleep_ms` |
| Clavier PS/2 | `kernel/drivers/ps2kbd.c`, `kbd_decode.c` | init complète du 8042 et reset du clavier, ISR minimal, automate jeu 1 (Shift, Ctrl, Alt, Verr. Maj, touches étendues, Pause), US QWERTY |
| File circulaire | `kernel/lib/ringbuf.c` | SPSC sans verrou, indices libres, perte comptée |
| Boucle d'inactivité | `kernel/core/kmain.c` | écho clavier, sommeil `sti; hlt` sans réveil perdu |
| Carte mémoire | `kernel/mm/memmap.c` | analyse défensive de la carte E820/Multiboot (éléments malformés, tronqués, débordements), tri, repli `mem_lower/mem_upper` |
| Allocateur physique | `kernel/mm/pmm_core.c`, `pmm.c` | table de descripteurs typés (FREE, RESERVED, KERNEL, ALLOCATED, PAGETABLE) avec compteur de références, next-fit, réservations prioritaires (mémoire basse, noyau, firmware, Multiboot), double libération et libération d'un cadre référencé refusées, cadres remis à zéro ; cadres de l'image et de la mémoire basse refusés indépendamment des descripteurs (K0.7-P4f-D) |
| Pagination | `kernel/arch/i386/paging.c` | 32 bits, pages 4 KiB, identité sur la RAM gérée (≤ 1 GiB), page nulle et pages de garde non projetées, `.text`/`.rodata` en lecture seule avec CR0.WP ; K0.7-P4 : structures critiques, répertoire et tables scellés, passage contrôlé `pt_gate_write`, PDE à écriture unique, audit au démarrage ; `vmm_map/unmap/translate` (un seul espace d'adressage) avec INVLPG |
| Fautes de page | `paging.c`, `probe.asm` | diagnostic contextuel (NULL, débordement de pile, écriture en lecture seule, hors RAM) ; sondes mémoire avec table de correction d'exception |
| Arithmétique 64 bits | `kernel/lib/math64.c` | division 64/32 sans libgcc (division longue binaire) |
| Bibliothèque minimale | `kernel/lib/string.c` | `memset`, `memcpy`, `memmove`, `memcmp`, `strlen`, `strnlen`, `strcmp`, `strncmp`, `strlcpy` |
| Autotests noyau | `kernel/tests/` | image de test distincte : 729 vérifications en 17 suites à 128 MiB (aussi exécutées à 16 MiB, 512 MiB et 4 GiB ; à 16 MiB, une vérification propre au banc de 128 MiB est **sautée et annoncée** : `[SKIP]`, jamais comptée comme un succès), 7 modes isolés (dont `n3`, K1.0), des modes volontairement fatals et des configurations de RAM limites ; 86 cas QEMU (dont les autotests complets à 16 MiB, 512 MiB et 4 GiB), 91 mutations détectées (`make test`, `make test-mutation`) ; intégration continue GitHub Actions (`.github/workflows/ci.yml`) : compilation, ELF, banc complet et reproductibilité à chaque `push` ; mutations sur déclenchement manuel (91 sur 91 détectées sur GitHub le 2026-11-20) |

## 6. Composants en cours de développement

Aucun. **K0.7 — CLOSED (scope-defined), 2026-11-20** (`docs/review/K0.7-formal-closure.md`) ; K1.1-M64-0 (infrastructure de test à deux émulateurs, aucun code du noyau) est intégré dans `main` (`bbcc73a`) ; K1.1-M64-1 (construction double, squelette ELF64 non amorçable) attend l'audit du développeur sur sa branche. Reports décidés le 2026-11-20, qui ne sont **pas** des réalisations : mémoire basse en lecture seule, couche HAL, instrumentation permanente de la pile de double faute.

## 7. Composants planifiés

Sous réserve d'autorisation (phase K1 et suivantes) : allocateur d'objets du noyau (`kmalloc`), x86-64 (mode long, noyau *higher-half*, NX, IST), TSS/IST, Ring 3, processus, ordonnanceur, appels système, IPC, VFS, pilotes de stockage et audio, moteur numérique, reconnaissance et synthèse vocales, agent.

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
│   │   ├── pic.c, irq.c      PIC 8259A et couche IRQ
│   │   ├── paging.c, probe.asm       pagination 32 bits, fautes de page, sondes
│   │   └── include/arch/     io.h, cpu.h, gdt.h, idt.h, interrupt.h, pic.h, irq.h, paging.h
│   ├── core/                 kmain.c, bootinfo.c, layout.c, log.c, panic.c
│   ├── drivers/              qemu_dbg.c, vga.c, serial.c, pit.c, ps2kbd.c, kbd_decode.c
│   ├── lib/                  string.c, format.c, math64.c, ringbuf.c
│   ├── mm/                   memmap.c, pmm_core.c, pmm.c, mm.c (mémoire physique)
│   ├── include/samos/        en-têtes internes du noyau
│   └── tests/                autotests exécutés dans le noyau
├── tests/
│   ├── tools/                tests négatifs des outils de vérification
│   ├── host/                 tests unitaires hébergés (ASan + UBSan)
│   ├── tools/x86_64_build_check.sh  chaîne x86-64 : drapeaux, séparation, ELF64,
│   │                         tests négatifs, reproductibilité (K1.1-M64-1)
│   └── qemu/                 tests de démarrage QEMU et vérifications GDB ;
│                             emu.sh, emu_matrix.sh, emu_compare.sh : choix et
│                             comparaison des émulateurs (K1.1-M64-0)
├── kernel/arch/x86_64/       SQUELETTE x86-64 non amorçable (K1.1-M64-1) : entry.asm,
│                             skeleton.c, skeleton_test.c, linker.ld
├── mk/x86_64.mk              construction x86-64 séparée (build/x86_64/)
├── tools/check_elf.sh        vérifications statiques de l'image ELF
├── tools/check_elf64.sh      vérifications statiques du squelette ELF64 (règles X1 à X12)
├── docs/adr/                 décisions d'architecture
├── docs/review/              revue de fin de phase (K0)
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

**Squelette x86-64 (K1.1-M64-1), construction seulement :**

```bash
make x86_64
```

Produit `build/x86_64/samos64.elf` et `build/x86_64/samos64-test.elf` : des ELF64 x86-64 **liés et inspectés statiquement** (`make check-elf64`), qui **ne sont pas amorçables**. Ils n'ont pas d'en-tête Multiboot, le format d'image (décision A-1) n'étant pas tranché, et pas de passage en mode long (M64-2). Aucun banc ne les démarre. `make` (cible `all`) ne construit que l'i386.

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
SAMOS: kernel image 0x00100000-0x00136000
  ... (régions de l'image)
[info ] mm: 0x000000000-0x00009fbff available
  ... (carte mémoire)
[info ] mm: 6 regions, 130559 KiB available
[info ] pmm: 32736 frames managed (130944 KiB), 32426 free (129704 KiB), 7 reserved ranges
[info ] paging: enabled (CR0.PG|WP), identity map 0x00001000-0x07fe0000, 32 page tables, null page and stack guard unmapped, .text/.rodata read-only
[info ] pmm: 32736 frames managed (130944 KiB), 32393 free (129572 KiB), 7 reserved ranges
[info ] irq: 8259 PIC remapped to vectors 0x20-0x2f, all lines masked
[info ] timer: PIT channel 0, 100 Hz (divisor 11932)
[info ] keyboard: 8042 self-test ok, PS/2 keyboard reset ok, IRQ1 enabled
SAMOS: boot stack high-water 836 bytes, guard page unmapped (MMU-enforced)
SAMOS: K0.6 boot complete, paging on, interrupts enabled, keyboard echo active
```

Le noyau reste ensuite actif. Les touches tapées dans la fenêtre QEMU (`make run-vga`) sont décodées et renvoyées à l'écran et sur le port série. Disposition US QWERTY.

Quitter avec `Ctrl+C`. `make run-vga` ouvre en plus la fenêtre de l'écran VGA ; il faut un affichage graphique. En cas de panique, l'adresse de chaque cadre de la trace se traduit en nom de fonction avec `addr2line -f -e build/samos.elf 0x...`. Débogage : `make debug`, puis dans un autre terminal `gdb build/samos.elf -ex 'target remote :1234'`.

## 13. Tests

```bash
make test
```

| Suite | Commande | Contenu |
|---|---|---|
| Statique ELF | `make check-elf` | ELF32/i386, entrée = `_start`, en-tête Multiboot (offset, alignement, somme), aucun symbole indéfini, aucun segment W+X, sections alignées/ordonnées/NOBITS, aucune section dynamique, absence d'auto-appel de `memset`/`memcpy` |
| Outils (négatif) | `make test-tools` | `check_elf.sh` doit rejeter une image à somme corrompue ou sans en-tête |
| Hébergés | `make test-host` | `string.c` ; `format.c` (comparaison différentielle avec `snprintf` de la libc) ; `math64.c` (100 000 divisions comparées à la division native) ; `ringbuf.c` (1 million d'opérations comparées à un modèle) ; `kbd_decode.c` (automate et 65 536 paires d'octets) ; `memmap.c` (cartes hostiles et 20 000 cartes aléatoires) ; `pmm_core.c` (invariants, épuisement, 200 000 opérations contre un modèle) ; le tout sous AddressSanitizer + UBSan |
| QEMU | `make test-qemu` | démarrage normal (sans triple fault), sortie série de bout en bout, clavier de bout en bout (frappes injectées par `sendkey`), autotests noyau (`-append test`), chemin de panique (`test=panic`, statut 37), refus d'un mode de test inconnu, exceptions fatales `test=ud/de/gp/np/intbad`, double faute par porte de tâche (`test=df`), triple fault volontaire détecté (`test=triple`), violations mémoire `test=null/wtext/wrodata/unmapped`, débordement de pile attrapé (`test=stack`), RAM de 16 MiB à 4 GiB |
| GDB | `make test-gdb` | état exact des registres à l'entrée de `kmain` ; test négatif du magic invalide ; lecture de la mémoire VGA `0xB8000` depuis l'extérieur du noyau |
| Construction x86-64 (K1.1-M64-1) | `make test-x86_64-build` (inclus dans `make test`) | **aucune exécution** : drapeaux du compilateur (`-mno-red-zone`, `-mgeneral-regs-only`, `-fno-pic`, `-mcmodel=small` provisoire), séparation des objets ELF32/ELF64, symboles des entrées de liaison, règles ELF64 X1 à X12 (`tools/check_elf64.sh`), 27 tests négatifs, reproductibilité |
| Deux émulateurs (K1.1-M64-0) | `make test-x86_64-emu` | la **même** image i386, les mêmes bancs QEMU et GDB sous `qemu-system-i386` puis `qemu-system-x86_64` ; chaque côté doit être complet et sans échec (bilans cohérents, SKIP attendus), puis verdicts, bilans et SKIP identiques ; seules les mesures de `n1stack` et `n1low` peuvent différer, et elles sont affichées (`tests/qemu/emu_matrix.sh`, `emu_compare.sh`) |

**Choix de l'émulateur.** `make test QEMU=qemu-system-x86_64` exécute tous les bancs sous un autre émulateur ; le défaut reste `qemu-system-i386`. Chaque rapport commence par une ligne `[EMU]` : émulateur, version, modèle de CPU mesuré par QMP (`qemu32` ou `qemu64`, **non équivalents**), machine. Sous `qemu-system-x86_64`, GDB est forcé en architecture `i386:x86-64` : `$esp`, `$eflags` et la mémoire sont exacts, mais `$eip`, le désassemblage et les arguments C symboliques ne le sont pas (Details.md §41). Les tests négatifs de ce choix (émulateur absent, invalide, d'une autre architecture ; GDB en erreur ; divergence) sont dans `tests/tools/emu_selftest.sh`, exécuté par `make test-tools`.

Le noyau transmet son verdict à QEMU via le périphérique `isa-debug-exit` (statut 33 = succès, 35 = échec, 37 = panic).

Vérifications du banc de tests lui-même (K0.7-R0, hors de `make test`) :

| Suite | Commande | Contenu |
|---|---|---|
| Mutation | `make test-mutation` | réintroduit des défauts corrigés (E5, E3, filtrage des droits PTE) dans une copie isolée et exige qu'ils soient détectés par des assertions nommées ; un échec de compilation, un délai, une panique ou un triple fault ne comptent jamais comme une détection. Auto-test : `tests/mutation/selftest.sh` |
| Reproductibilité | `make test-repro` | deux compilations propres dans deux répertoires ; images sans débogage identiques octet pour octet, seules les sections `.debug_*` diffèrent (chemin absolu écrit par NASM). `REPRO_STRICT=1` exige des ELF identiques. Auto-test : `tests/tools/repro_selftest.sh` |

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

- PIC 8259 uniquement (pas d'APIC/IOAPIC) : monoprocesseur ; la file clavier repose sur l'ordonnancement mémoire d'un seul cœur x86.
- Clavier : disposition US QWERTY uniquement ; IRQ parasites gérées mais non testées (non reproductibles sous QEMU).
- NMI et Machine Check ne sont pas traités spécifiquement : ils mènent à une panique ; pendant la fenêtre CR0.WP du passage, le gestionnaire s'exécute **sans protection en écriture** (risque accepté pour P4).
- Pas de segments ni de code ring 3 (prévu en K1).
- Le port série est en émission seule, par scrutation (pas d'IRQ 4, pas de réception).
- Le journal n'est pas réentrant : les futurs gestionnaires d'IRQ ne devront pas journaliser.
- **Pas de bit NX** (pagination 32 bits sans PAE) : les données du noyau sont exécutables ; W^X n'est réalisé que pour le code.
- Projection en identité : seule la RAM sous 1 GiB est gérée ; toute la RAM est inscriptible par le noyau, sauf `.text`/`.rodata`, les structures scellées en P4d, le répertoire et les tables de pages (écrits seulement par la fenêtre CR0.WP du passage) ; la GDT/TSS et la table du PMM restent inscriptibles ; pas encore d'espaces d'adressage séparés ni de noyau *higher-half*.
- **N3 :** corrigé en K1.0 (intégré dans `main`), dans le modèle actuel (un seul espace d'adressage, monoprocesseur) : un cadre encore projeté hors identité n'est plus libéré ni redistribué, même si ses références ou son type PMM sont falsifiés. Limites : un seul espace d'adressage (à étendre avant K1.4), fuite de disponibilité possible si les références sont falsifiées vers le haut, coût linéaire en nombre de tables dynamiques (×2,7 à ×23 sous TCG sur `pmm_alloc_frame`/`pmm_free_frame`).
- Les contrôles de P4 parcourent le répertoire : `vmm_map`/`vmm_unmap` sont nettement plus coûteux qu'en K0.6 (mesure sous QEMU seulement).
- K0.7 : les canaris de pile ne protègent que les fonctions que Clang instrumente (11 en production), et seulement à leur retour ; les pages de garde des piles ne couvrent que 4 KiB ; un débordement de la pile de double faute se termine par un triple fault (voir `docs/review/K0.7-R1g-audit.md`).
- Pas d'allocateur d'objets (`kmalloc`) ; l'allocateur physique n'a pas de verrou (monoprocesseur, appelé hors interruption).
- Le code d'autotest et les modes de faute volontaire ne sont compilés que dans l'image de test (`build/samos-test.elf`) ; l'image de production (`build/samos.elf`) ne les contient pas (vérifié par `check_elf.sh`).
- Testé uniquement sous QEMU. Sur un PC UEFI récent, le mode texte VGA ne sera pas disponible.

## 17. Feuille de route

`K0.1 ✅ → … → K0.6 ✅ → K0.7 ✅ CLOSED (scope-defined), 2026-11-20 (R1 ✅, P4 ✅ dans son périmètre ; K1.0-N3 ✅ correction de N3 intégrée) → K1 : décisions d'architecture, kmalloc, x86-64 / higher-half, espaces d'adressage, ring 3 → processus/ordonnanceur → appels système → IPC → VFS → pilotes → A0 (IA) → V0 (voix) → I0 (agent)` — détail : [`docs/design/K1-readiness.md`](docs/design/K1-readiness.md)

## 18. Documentation

- [`Walkthrough.md`](Walkthrough.md) : journal de développement reproductible, étape par étape.
- [`Details.md`](Details.md) : cours technique et académique sur les concepts implémentés.
- [`docs/review/K0-review.md`](docs/review/K0-review.md) : revue complète de K0 et préparation x86-64.
- [`docs/adr/0001-k0-fondations.md`](docs/adr/0001-k0-fondations.md) : décisions d'architecture de K0.
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

*Décision du 2026-11-20 :* aucune licence n'est choisie pour le moment, et aucune ne doit être ajoutée automatiquement. Aucune release n'est publiée.
