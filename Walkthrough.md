# SAMOS / AtlasOS — Walkthrough

Journal de développement **chronologique et reproductible** du noyau SAMOS. Ce document consigne ce qui a réellement été fait, dans l'ordre, avec les commandes exactes, les résultats observés, les erreurs rencontrées et leurs corrections.

> Convention : les commandes « historiques » sont celles exécutées au moment du jalon. La **procédure actuelle recommandée** est toujours celle du `README.md` (`make`, `make run`, `make test`).

---

## Étape 0 — État initial et préparation de l'environnement (2026-11-17)

**État de départ.** Le dépôt `saad-programmer/atlasos` contenait un seul commit (`9373a9d first commit`) avec uniquement `CLAUDE.md`. L'ancien noyau 32 bits n'est pas dans le dépôt et, par décision du développeur, il n'y sera pas importé : AtlasOS est une base de code nouvelle.

**Outils présents dans l'environnement d'intégration (Ubuntu 24.04) :**

```bash
clang --version   # Ubuntu clang version 18.1.3
nasm -v           # NASM version 2.16.01
ld -v             # GNU ld (GNU Binutils for Ubuntu) 2.42
gdb --version     # GNU gdb 15.1
```

**Outil installé :** QEMU, absent au départ.

```bash
apt-get update && apt-get install -y qemu-system-x86
qemu-system-i386 --version   # QEMU emulator version 8.2.2
```

**Vérification préalable des options du compilateur.** Avant d'écrire le noyau, un petit fichier C a été compilé avec les options envisagées, puis désassemblé (`objdump -d`). Deux constats en sont ressortis :

1. Toutes les options sont acceptées par Clang 18 pour `--target=i686-unknown-none-elf`, y compris `-mgeneral-regs-only`.
2. **Constat important :** pour cette cible, Clang n'assure par défaut qu'un **alignement de pile de 4 octets**. Une fonction `g()` appelant `h(buf)` a donné `sub $0x40,%esp ; push %eax ; call`, soit un ESP qui n'est pas multiple de 16 au moment de l'appel. Avec `-mstack-alignment=16`, le code devient `sub $0x48,%esp ; mov %eax,(%esp) ; call`, et l'alignement sur 16 est maintenu. L'option a donc été retenue pour respecter la convention i386 System V que nous avons choisie (explication dans `Details.md`, §3.4).

**Décisions approuvées par le développeur :** Clang i686 freestanding, Multiboot 1, GNU Make, noyau chargé à 1 MiB en identité, en-têtes `stdint.h`/`stddef.h`/`stdbool.h`/`stdarg.h` autorisés, pas de libgcc tant qu'elle n'est pas nécessaire, documentation en français, aucune licence choisie.

`CLAUDE.md` a été mis à jour pour enregistrer l'autorisation d'implémenter K0 (jusqu'à K0.6 inclus), la règle d'explication obligatoire de chaque changement de code, et ces décisions.

---

## Jalon K0.1 — Démarrage Multiboot, pile et entrée en C

### A. Objectif

Obtenir un noyau ELF 32 bits **réellement démarrable** par un chargeur Multiboot 1 (ici le chargeur intégré de QEMU). Il doit :

- établir lui-même un environnement d'exécution C conforme à l'ABI ;
- valider les informations de démarrage avant de les utiliser ;
- s'arrêter de façon sûre ;
- se tester automatiquement.

### B. État de départ

Aucun code. Aucun moyen d'observer l'exécution.

### C. Fichiers créés

| Fichier | Rôle |
|---|---|
| `Makefile` | Compilation (Clang, NASM, ld), cibles `run`, `debug`, `check-elf`, `test*`, `clean` |
| `.gitignore` | Exclut `build/`, objets, ELF, journaux |
| `kernel/arch/i386/boot/entry.asm` | En-tête Multiboot, pile de 16 KiB, `_start` |
| `kernel/arch/i386/linker.ld` | Chargement à 1 MiB, 3 segments R-X/R--/RW-, symboles `__*_start/__*_end` |
| `kernel/arch/i386/include/arch/io.h` | `inb/outb/inw/outw/inl/outl`, `io_wait` |
| `kernel/arch/i386/include/arch/cpu.h` | Lecture d'EFLAGS, CR0, CR2, CR3, ESP ; `cli/sti`, `invlpg`, `cpu_halt_forever` |
| `kernel/include/samos/compiler.h` | Macros d'attributs (`_Noreturn`, `packed`, assertions statiques) |
| `kernel/include/samos/errno.h` | Codes d'erreur négatifs du noyau |
| `kernel/include/samos/kernel.h` | Prototype de `kmain`, symboles du linker et de la pile |
| `kernel/include/samos/multiboot.h` | Structures Multiboot 1 (`multiboot_info`, `multiboot_mmap_entry`), avec assertions sur les offsets |
| `kernel/include/samos/bootinfo.h`, `kernel/core/bootinfo.c` | Validation du magic et des adresses, copie bornée de la ligne de commande et du nom du chargeur, analyse des jetons |
| `kernel/include/samos/qemu_dbg.h`, `kernel/drivers/qemu_dbg.c` | Canal debugcon (port 0xE9) et `qemu_exit` (isa-debug-exit) |
| `kernel/include/samos/string.h`, `kernel/lib/string.c` | Fonctions mémoire et chaînes |
| `kernel/core/kmain.c` | Point d'entrée C |
| `kernel/include/samos/ktest.h`, `kernel/tests/ktest.c`, `kernel/tests/test_boot.c` | Cadre d'autotests noyau et suite K0.1/K0.2 |
| `tools/check_elf.sh` | Vérifications statiques de l'ELF |
| `tests/host/*` | Tests hébergés (ASan + UBSan) de `string.c` |
| `tests/qemu/run.sh`, `tests/qemu/cases.sh` | Tests de démarrage QEMU |
| `tests/qemu/gdb_checks.sh` | Vérifications par GDB au niveau instruction |
| `README.md`, `Walkthrough.md`, `Details.md` | Documentation |

### D. Séquence d'implémentation

1. **`entry.asm`.** L'en-tête Multiboot (`magic`, `flags = 0x3`, `checksum = -(magic+flags)`) a été placé dans une section dédiée `.multiboot`. La pile est réservée dans `.bss` (`resb 16384`, `align=16`). `_start` exécute, dans cet ordre :
   1. `cli` ;
   2. `mov esp, boot_stack_top` : c'est la première chose à faire, car ESP est indéfini à l'entrée ;
   3. `push 0 / popfd` : EFLAGS remis à zéro, donc DF = 0 comme l'exige l'ABI ;
   4. effacement de `.bss` par `rep stosd`, après avoir sauvegardé le magic d'EAX dans ESI ;
   5. `sub esp, 8`, puis `push ebx` et `push esi` : ESP est multiple de 16 au moment du `call` ;
   6. vérification `test esp, 0xF` : on s'arrête si l'alignement est faux ;
   7. `call kmain` ;
   8. boucle `cli ; hlt ; jmp`.
2. **`linker.ld`.** `KEEP(*(.multiboot))` est placé en tête de `.text` pour que l'en-tête soit au début du premier segment chargé. `PHDRS` définit trois segments à permissions distinctes. Des `ASSERT` vérifient l'adresse de chargement et l'alignement de `.bss`.
3. **En-têtes `arch/`.** Ils regroupent les primitives matérielles en assembleur en ligne.
4. **`multiboot.h`.** Les structures ont été écrites d'après la spécification. Des `_Static_assert` vérifient les offsets de `cmdline` (16), `mmap_length` (44) et `boot_loader_name` (64), ainsi que la taille de l'élément mmap (24).
5. **`bootinfo.c`.** Chaque adresse fournie par le chargeur passe par `bootinfo_region_ok()`, qui refuse :
   - une adresse nulle ;
   - un débordement 32 bits, détecté sans calculer `addr + len` ;
   - un chevauchement avec `[__kernel_start, __kernel_end)`.

   Les chaînes sont copiées avec `strnlen` borné dans des tampons du noyau.
6. **`qemu_dbg.c`.** C'est le moyen d'observation le plus simple : une seule instruction `out` par caractère, sans aucune initialisation.
7. **`kmain.c`.** Il annonce l'entrée, valide le démarrage (arrêt explicite sinon), puis exécute les autotests si la ligne de commande contient `test`, ou s'arrête sinon.
8. **Tests**, voir G.

### E. Compilation

```bash
make
```

Commandes réellement exécutées par `make`, ici pour un fichier C :

```bash
nasm -f elf32 -g -F dwarf -w+all -w+error kernel/arch/i386/boot/entry.asm -o build/kernel/arch/i386/boot/entry.o
clang --target=i686-unknown-none-elf -march=i686 -std=c11 -ffreestanding -nostdlibinc -fno-pic -fno-pie \
      -fno-stack-protector -fcf-protection=none -mgeneral-regs-only -mstack-alignment=16 \
      -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-common -O2 -g \
      -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wundef \
      -Wvla -Wimplicit-fallthrough -Wcast-qual -Ikernel/include -Ikernel/arch/i386/include -MMD -MP \
      -c kernel/core/kmain.c -o build/kernel/core/kmain.o
ld -m elf_i386 -T kernel/arch/i386/linker.ld -nostdlib -z max-page-size=0x1000 -z noexecstack \
   --build-id=none --fatal-warnings -Map=build/samos.map -o build/samos.elf <objets...>
```

La signification de chaque option est expliquée dans le `Makefile` et dans `Details.md` (§3).

Disposition obtenue (`readelf -lW build/samos.elf`) :

```text
  Type  Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align
  LOAD  0x001000 0x00100000 0x00100000 0x01077 0x01077 R E 0x1000
  LOAD  0x003000 0x00102000 0x00102000 0x00741 0x00741 R   0x1000
  LOAD  0x004000 0x00103000 0x00103000 0x00004 0x0526c RW  0x1000
```

Dans le troisième segment, `FileSiz = 4` correspond à la seule variable de `.data`. `MemSiz = 0x526c` inclut `.bss` (pile de 16 KiB et `g_bootinfo`), que le chargeur remplit de zéros et que `_start` efface de nouveau.

### F. Exécution

```bash
make run
# équivaut à : qemu-system-i386 -m 128M -display none -no-reboot -no-shutdown -kernel build/samos.elf -debugcon stdio
```

Sortie observée :

```text
SAMOS: kernel entry (SAMOS/AtlasOS 0.1.0-k0)
SAMOS: multiboot magic ok, info at 0x00009500, flags 0x0000024f
SAMOS: cmdline "build/samos.elf "
SAMOS: K0.1 boot complete, halting
```

Les drapeaux `0x24f` signifient : mem (bit 0), boot_device (1), cmdline (2), mods (3), mmap (6) et nom du chargeur (9). On observe aussi que le chargeur de QEMU place dans la ligne de commande le nom du fichier noyau, suivi des arguments de `-append`.

### G. Tests

```bash
make test
```

| Test | Méthode | Résultat attendu | Résultat observé |
|---|---|---|---|
| ELF32 / i386 / EXEC / entrée = `_start` | `readelf -h`, `nm` | conforme | **PASS** (entrée `0x100010`) |
| En-tête Multiboot | `od` + calcul bash | offset < 8192, aligné, somme 0 | **PASS** (offset 4096, flags `0x3`, checksum `0xe4524ffb`) |
| Symboles indéfinis | `nm -u` | aucun | **PASS** |
| Segments | `readelf -lW` | pas de W+X, ≥ 1 MiB, vaddr = paddr | **PASS** (3 segments) |
| `memset`/`memcpy`/`memmove` sans auto-appel | `objdump -d` | aucun `call` vers soi-même | **PASS** |
| `string.c` hébergé | GCC + ASan + UBSan | 0 échec, 0 erreur de sanitizer | **PASS** (50 vérifications) |
| Boot normal | QEMU, timeout 3 s | messages attendus, statut 124, pas de reset supplémentaire | **PASS** |
| Autotests noyau | QEMU `-append test` | statut 33, `ALL TESTS PASSED` | **PASS** (42 vérifications, 0 échec) |
| ABI à l'entrée de `kmain` | GDB `break *kmain` | ESP%16 = 12, arg0 = `0x2badb002`, arg1 ≠ 0, DF = 0, IF = 0, ESP dans la pile | **PASS** (arg1 = `0x9500`) |
| Magic invalide (négatif) | GDB remplace l'argument par `0xdeadbeef` | `FATAL: invalid multiboot magic (0xdeadbeef)` puis arrêt | **PASS** |

Contenu des autotests noyau (`kernel/tests/test_boot.c`) :

- état du CPU : DF = 0, IF = 0, CR0.PE = 1 ;
- exécution sur notre pile, sommet aligné sur 16, taille de 16 KiB, alignement ABI du cadre de `kmain` ;
- disposition : noyau à 1 MiB, sections alignées sur 4 KiB et ordonnées, pile dans `.bss`, `kmain` dans `.text` ;
- `.data` chargée avec sa valeur initiale, `.bss` à zéro ;
- informations Multiboot et rejet des régions invalides (adresse nulle, débordement, chevauchement du noyau) ;
- fonctions de chaîne, y compris `memmove` avec chevauchement et `strlcpy` avec troncature.

### H. Débogage : erreurs rencontrées et corrections

1. **`awk: function strtonum never defined`** dans `check_elf.sh`. L'`awk` d'Ubuntu est `mawk`, qui ne connaît pas l'extension GNU `strtonum`. Le calcul de la somme Multiboot a été déplacé dans l'arithmétique de bash (`$(( ... & 0xFFFFFFFF ))`), ce qui fonctionne avec mawk comme avec gawk.
2. **Runtime ASan de Clang absent** (`libclang_rt.asan-x86_64.a` introuvable). Les tests hébergés utilisent GCC (`libasan`/`libubsan` disponibles), modifiable avec `HOST_CC`. Le noyau reste compilé par Clang.
3. **Faux positif « reset CPU inattendu ».** Le script supposait un seul `CPU Reset` au démarrage. Le journal `-d cpu_reset` de QEMU 8.2.2 en contient **deux** : l'initialisation de l'objet CPU (EIP = 0) puis le reset système (EIP = `0xfff0`). Le seuil a été mesuré puis fixé à 2 (variable `QEMU_BOOT_RESETS`). La capacité du script à détecter un vrai triple fault sera démontrée en K0.4 par un test volontaire.
4. **`Argument to arithmetic operation not a number`** dans GDB. `$esp` y est typé comme un pointeur, ce qui interdit `$esp % 16`. Correction : conversion `(unsigned int)$esp`. C'était une erreur du script de test, pas du noyau.

### I. Intégration

K0.1 fournit à tous les jalons suivants :

- la pile ;
- les symboles de disposition (utilisés en K0.6 pour réserver l'image du noyau) ;
- `bootinfo` (en K0.6, la carte mémoire sera lue à partir de `mmap_addr/mmap_length`, déjà validés) ;
- le canal de diagnostic ;
- l'infrastructure de tests : chaque nouveau jalon ajoute une suite `ktest_suite_*` et des cas dans `tests/qemu/cases.sh`.

### J. Critères d'achèvement

- [x] Compilation propre avec `-Werror`, sans symbole indéfini.
- [x] En-tête Multiboot valide et reconnu (QEMU démarre le noyau).
- [x] Pile valide et alignée, vérifiée au niveau instruction par GDB.
- [x] Magic validé, avec un test négatif qui le prouve.
- [x] Boucle d'arrêt sûre, sans triple fault.
- [x] Tests automatiques reproductibles (`make test`).
- [x] Documentation à jour.

### K. Reproduction depuis un dépôt propre

```bash
git clone https://github.com/saad-programmer/atlasos.git && cd atlasos
sudo apt install clang nasm binutils make qemu-system-x86 gdb gcc
make clean && make test        # historique K0.1 : 23 lignes [PASS] attendues, aucune [FAIL]
make run                       # affiche les 4 lignes de la section F ; Ctrl+C pour quitter
```

---

## Jalon K0.2 — Script de liaison, sections ELF et disposition mémoire

### A. Objectif

Rendre la disposition mémoire de l'image **explicite, vérifiée et exploitable** :

- isoler la pile dans sa propre section, avec une zone de garde ;
- mesurer l'utilisation de la pile ;
- exposer une table des régions et de leurs droits, dont K0.6 aura besoin pour réserver le noyau et configurer la pagination ;
- renforcer les vérifications statiques de l'ELF ;
- prouver que le vérificateur sait échouer.

### B. État de départ

K0.1 avait déjà un script de liaison fonctionnel (1 MiB, 3 segments). La pile était dans `.bss`, directement au-dessus des autres variables globales. Un débordement les aurait corrompues sans aucun signal.

### C. Fichiers créés et modifiés

| Fichier | Changement |
|---|---|
| `kernel/arch/i386/boot/entry.asm` | pile déplacée dans `.bootstack` (alignée 4 KiB) ; constante `STACK_PAINT` ; peinture de `[__stack_guard_start, boot_stack_top)` avant l'appel de `kmain` |
| `kernel/arch/i386/linker.ld` | section de sortie `.bootstack (NOLOAD)` : 4 KiB de garde puis la pile ; 3 nouvelles `ASSERT` (pile collée à la garde, sommet aligné sur 16, noyau < 4 MiB) |
| `kernel/include/samos/kernel.h` | symboles `__stack_guard_start/end`, `BOOT_STACK_PAINT` |
| `kernel/include/samos/layout.h`, `kernel/core/layout.c` | **nouveaux** : table des régions, `kernel_layout_valid()`, `kernel_layout_print()`, `boot_stack_high_water()`, `boot_stack_guard_intact()` |
| `kernel/core/kmain.c` | affichage et vérification de la disposition (arrêt si invalide) ; affichage de la profondeur de pile |
| `kernel/tests/test_layout.c` | **nouveau** : suite K0.2 (18 vérifications) |
| `kernel/tests/test_boot.c` | test de placement de pile adapté (pile hors de `.bss`) |
| `tools/check_elf.sh` | vérification des sections (présence, alignement 4 KiB, ordre, NOBITS, absence de sections dynamiques/PIC) |
| `tests/tools/check_elf_negative.sh` | **nouveau** : tests négatifs du vérificateur |
| `tests/qemu/cases.sh`, `Makefile` | nouveaux motifs attendus, cible `test-tools`, nouvelles sources |

### D. Séquence d'implémentation

1. Création de la section `.bootstack` dans `entry.asm` (`nobits alloc noexec write align=4096`).
2. Section de sortie correspondante dans `linker.ld`, après `.bss` :
   - `__stack_guard_start` ;
   - `. += PAGE_SIZE` ;
   - `__stack_guard_end` ;
   - `KEEP(*(.bootstack))`.

   La pile termine ainsi l'image : `boot_stack_top == __kernel_end`.
3. Ajout de la peinture dans `_start`, après l'effacement de `.bss`, au moment où la pile est vide.
4. `layout.c` : la table des régions est remplie à l'exécution, car les adresses des symboles du linker ne sont pas des constantes de compilation en C. Le code vérifie la contiguïté, l'alignement et la couverture exacte de l'image.
5. Tests : la suite K0.2 comprend un **test négatif** qui écrit dans la garde, vérifie la détection, puis restaure le motif.
6. `check_elf.sh` : nouvelles vérifications de sections, puis `check_elf_negative.sh`.

### E. Compilation

Inchangée : `make`. Nouvelle disposition (`readelf -SW build/samos.elf`) :

```text
.text      PROGBITS 0x00100000   AX  align 4096
.rodata    PROGBITS 0x00102000   A   align 4096
.data      PROGBITS 0x00103000   WA  align 4096
.bss       NOBITS   0x00104000   WA  align 4096
.bootstack NOBITS   0x00105000   WA  align 4096
```

### F. Exécution

`make run`. Sortie observée :

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

### G. Tests

```bash
make clean && make test
```

Résultat observé : **34 lignes `[PASS]`, aucune `[FAIL]`**, statut de sortie 0.

| Test | Résultat attendu | Résultat observé |
|---|---|---|
| Sections présentes, alignées sur 4 KiB, ordonnées | conforme | **PASS** (5 sections) |
| Aucune section `.got/.plt/.dynamic/.interp/.rel*/.eh_frame/.note` | aucune | **PASS** |
| `.bss` et `.bootstack` NOBITS | oui | **PASS** |
| `check_elf.sh` rejette une somme Multiboot corrompue | rejet, diagnostic « checksum invalide » | **PASS** |
| `check_elf.sh` rejette une image sans en-tête | rejet, diagnostic « introuvable » | **PASS** |
| Autotests noyau (K0.1 + K0.2) | statut 33 | **PASS** (60 vérifications, 0 échec) |
| Garde intacte après démarrage, profondeur mesurée | intacte, 0 < profondeur < 16 KiB | **PASS** (116 octets) |
| Sensibilité de la mesure (tableau local de 2 KiB) | profondeur ≥ 2048 | **PASS** |
| Écriture simulée dans la garde (négatif) | détectée, puis restaurée | **PASS** |
| Régression K0.1 (ABI GDB, magic invalide) | inchangé | **PASS** |

### H. Débogage

1. **Motif de test absent** `"---G  stack-guard"`. Le noyau affichait `---G stack-guard`, avec une seule espace, car la fonction `print_perms` écrit 4 caractères pour la garde contre 3 + 1 espace pour les autres régions. L'erreur était dans le motif du test, corrigé ; le noyau était correct.
2. **Vérification du vérificateur.** Avant d'en faire un test permanent, l'octet de somme de contrôle d'une copie de l'ELF a été mis à zéro à la main. `check_elf.sh` a bien répondu `[FAIL] checksum invalide (somme=4294967045)` avec le statut 1.

### I. Intégration

- `layout.c` dépend uniquement des symboles du linker et du canal de diagnostic.
- La table des régions sera consommée par l'allocateur physique (réservation de `[__kernel_start, __kernel_end)`) et par la pagination (droits par région, garde non présente) en K0.6.
- `boot_stack_high_water()` servira à dimensionner la pile face aux interruptions (K0.4/K0.5).

### J. Critères d'achèvement

- [x] Toutes les sections chargées sont alignées sur 4 KiB, ordonnées, sans chevauchement (vérifié statiquement **et** à l'exécution).
- [x] Aucune région W+X dans le modèle ni dans les segments ELF.
- [x] Zone de garde de 4 KiB sous la pile, détection logicielle démontrée par un test négatif.
- [x] Mesure de la profondeur de pile, avec sensibilité démontrée.
- [x] Le vérificateur ELF prouve qu'il sait échouer.
- [x] Aucune régression K0.1.
- [x] Documentation à jour (`Details.md` §13-17).

### K. Reproduction

```bash
git checkout <commit K0.2>
make clean && make test    # 34 lignes [PASS], aucune [FAIL]
make run                   # sortie de la section F
```

---

## Jalon K0.3 — VGA, port série, journalisation et panique

### A. Objectif

Doter le noyau de vrais canaux de diagnostic, indépendants de QEMU :

- l'écran VGA texte ;
- le port série COM1 ;
- une journalisation formatée commune aux trois canaux ;
- un chemin d'erreur fatale (`panic`) qui explique où et pourquoi le système s'arrête.

### B. État de départ

Seul le port debugcon `0xE9` de QEMU permettait d'observer le noyau, avec des fonctions d'affichage ad hoc (`qemu_dbg_put_hex32`…). Une erreur fatale appelait `boot_fail()`, propre à `kmain.c`.

### C. Fichiers créés et modifiés

| Fichier | Changement |
|---|---|
| `kernel/include/samos/vga.h`, `kernel/drivers/vga.c` | **nouveaux** : pilote VGA texte 80×25 |
| `kernel/include/samos/serial.h`, `kernel/drivers/serial.c` | **nouveaux** : UART 16550 COM1 |
| `kernel/include/samos/format.h`, `kernel/lib/format.c` | **nouveaux** : `ksnprintf`/`kvsnprintf` |
| `kernel/include/samos/math64.h`, `kernel/lib/math64.c` | **nouveaux** : division 64/32 sans libgcc |
| `kernel/include/samos/log.h`, `kernel/core/log.c` | **nouveaux** : journal multi-puits, `kprintf`, `klog` |
| `kernel/include/samos/panic.h`, `kernel/core/panic.c` | **nouveaux** : `panic`, `KASSERT`, `stack_collect` |
| `kernel/arch/i386/boot/entry.asm` | `xor ebp, ebp` avant `call kmain` (terminateur de la chaîne de cadres) |
| `kernel/core/kmain.c` | réécrit : journal d'abord, `panic()` à la place de `boot_fail()`, journal formaté |
| `kernel/core/layout.c` | affichage via `kprintf` |
| `kernel/tests/ktest.c` | sortie via `kprintf` ; modes `test=panic` et refus d'un mode inconnu |
| `kernel/tests/test_console.c` | **nouveau** : suite K0.3 (27 vérifications) |
| `tests/host/test_format.c` | **nouveau** : test différentiel contre la libc + `math64` |
| `tests/qemu/run.sh`, `tests/qemu/cases.sh` | motifs `serial:` (série seule) et `!` (interdit) ; 3 nouveaux cas |
| `tests/qemu/gdb_checks.sh` | panique attendue pour le magic invalide ; lecture de la mémoire VGA |
| `Makefile` | nouvelles sources ; `-Wformat=2` ; `make run` sur série ; `make run-vga` |

### D. Séquence d'implémentation

1. **`math64.c` puis `format.c`, testés sur l'hôte avant toute intégration.** Le formateur est le composant le plus exposé (tampons, arguments variadiques). La division 64 bits a été écrite d'abord, car le formateur en dépend pour `%llu`/`%lld`. Le test différentiel a révélé une lacune de conception avant même l'exécution : la précision des entiers (`%.5d`) n'était pas traitée. Elle a été implémentée selon C11 plutôt que documentée comme manquante.
2. **`vga.c`.** Tampon `volatile`, défilement, `\n\r\t\b`, curseur CRTC, avec détection couleur ou mono via le port `0x3CC`.
3. **`serial.c`.** Test du registre SCR, initialisation à 115 200 bauds 8N1, test en boucle locale, émission par scrutation bornée.
4. **`log.c`.** Diffusion vers les trois puits, tampon de pile de 256 octets, troncature signalée.
5. **`panic.c`.** Garde de récursion, trace EBP validée, sortie QEMU en mode test. Dans `entry.asm`, EBP est mis à 0 avant `kmain`.
6. **Intégration dans `kmain`.** Le journal est initialisé en premier.
7. **Tests.** Suite noyau K0.3, cas QEMU série et panique, contrôle de la mémoire VGA par GDB.

### E. Compilation

`make`. Nouvelle option : `-Wformat=2`, qui fait d'un format non littéral une erreur (`-Wformat-nonliteral`, `-Wformat-security`). La liaison reste sans aucun symbole indéfini : `nm -u` est vide, malgré l'usage d'entiers 64 bits.

### F. Exécution

```bash
make run
```

Sortie observée sur le terminal, via COM1 :

```text
SAMOS: kernel entry (SAMOS/AtlasOS 0.1.0-k0)
[info ] console: debugcon + serial COM1 115200 8N1 + VGA 80x25
SAMOS: multiboot magic ok, info at 0x00009500, flags 0x0000024f
SAMOS: cmdline "build/samos.elf "
SAMOS: kernel image 0x00100000-0x0010d000
SAMOS:   0x00100000-0x00104000 r-x  .text
SAMOS:   0x00104000-0x00106000 r--  .rodata
SAMOS:   0x00106000-0x00108000 rw-  .data+.bss
SAMOS:   0x00108000-0x00109000 ---G stack-guard
SAMOS:   0x00109000-0x0010d000 rw-  boot-stack
SAMOS: boot stack high-water 792 bytes, guard intact
SAMOS: K0.3 boot complete, halting
```

Le même texte a été relu dans la mémoire VGA à `0xB8000` par GDB (`build/tests/vga.txt`).

Panique volontaire :

```bash
qemu-system-i386 -kernel build/samos.elf -append test=panic -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04
```

Sortie observée :

```text
*** KERNEL PANIC: deliberate test panic (42)
*** at kernel/tests/ktest.c:44
*** stack trace (5 frames, addr2line -e build/samos.elf <addr>):
***   #0 0x001025c9
***   #1 0x001025ab
***   #2 0x0010251f
***   #3 0x001001e3
***   #4 0x0010005d
*** system halted
```

`addr2line -f -e build/samos.elf` traduit ces adresses en `deliberate_panic_inner`, `deliberate_panic`, `ktest_run_all`, `kmain` et `_start`, soit exactement la chaîne d'appels réelle.

### G. Tests

```bash
make clean && make test
```

Résultat observé : **39 lignes `[PASS]`, aucune `[FAIL]`**, statut 0.

| Test | Résultat attendu | Observé |
|---|---|---|
| Formateur vs `snprintf` glibc (32 formats) + cas propres à SAMOS + troncature | 0 différence, 0 erreur ASan/UBSan | **PASS** (43 vérifications) |
| `u64_divmod_u32` vs division native (100 000 cas, d = 0 défini) | identique | **PASS** |
| Suite noyau K0.3 (VGA, série, formatage i386, troncature, trace de pile) | 27/27 | **PASS** |
| Total des autotests noyau | statut 33 | **PASS** (87 vérifications, 0 échec) |
| `serial_output` : messages présents dans le **seul** journal COM1 | présents | **PASS** |
| `panic_path` : message, `at ktest.c:`, trace, `#1 0x001…`, `system halted`, statut 37 | conforme | **PASS** |
| `unknown_mode` : `test=nope` refusé, statut 35 | conforme | **PASS** |
| `boot_halt` : aucune panique, pas de triple fault | conforme | **PASS** |
| GDB : texte de démarrage dans la mémoire VGA `0xB8000` | présent | **PASS** |
| GDB : magic invalide → `KERNEL PANIC: invalid multiboot magic 0xdeadbeef` | conforme | **PASS** |
| Régressions K0.1/K0.2 (ELF, ABI, garde de pile) | inchangé | **PASS** |

### H. Débogage

1. **Le compilateur a refusé les formats de test volontairement invalides.** Avec `-Werror=format`, GCC a rejeté `"%-05d"` (« '0' flag ignored with '-' flag »), `"a%qb"` et `"fin%"` dans `test_format.c`. C'est la preuve que l'attribut `SAMOS_PRINTF` fonctionne. Comme ces cas sont exactement ce que le test veut exercer, les avertissements de format ont été désactivés **dans ce seul fichier de test**, par `#pragma`, avec un commentaire.
2. **`SAME("100%%")` sans argument** : le macro variadique exige au moins un argument. Il a été remplacé par `SAME("100%%%s", "")`.
3. **Mesure de pile.** La profondeur maximale est passée de 116 à 792 octets, ce qui correspond à l'ajout du tampon de 256 octets de `kprintf` et des tampons de conversion. C'est cohérent et largement sous les 16 Kio.
4. **Test GDB VGA intermittent (défaut du script, pas du noyau).** Un second `make test` a échoué : `[FAIL] gdb: texte absent de la memoire VGA`. Le journal GDB montrait un arrêt à `0xfffeb2e7`, c'est-à-dire dans le BIOS. Le noyau n'avait pas encore démarré. **Cause :** la boucle d'attente cherchait `boot complete` dans le journal debugcon, mais le fichier de l'exécution **précédente** existait encore. La boucle se terminait immédiatement, avant que QEMU ne recrée le fichier. **Correction :** suppression des journaux avant chaque lancement de QEMU (`rm -f`). **Vérification :** 10 exécutions consécutives de `gdb_checks.sh`, avec 8/8 `[PASS]` à chaque fois. Leçon : un test qui attend un état doit d'abord détruire toute trace de l'état précédent.

### I. Intégration

- Tout le noyau journalise désormais via `kprintf`/`klog`, et toute erreur fatale passe par `panic`.
- `qemu_dbg` reste le canal de secours de la panique récursive.
- K0.4 s'appuiera sur `panic` pour les exceptions CPU, et sur la règle « pas de journal en IRQ » (Details §18.5) pour K0.5.

### J. Critères d'achèvement

- [x] VGA : écriture, retour à la ligne, défilement, couleurs, curseur matériel, bornes vérifiés.
- [x] Série : présence, boucle locale, transmission de bout en bout vérifiées.
- [x] Formateur conforme à C11 sur le sous-ensemble supporté, sans débordement possible, sans libgcc.
- [x] Panique : message, emplacement, trace de pile correcte et symbolisable, récursion gérée.
- [x] Aucune régression.
- [x] Documentation à jour (Details §18 à 21).

### K. Reproduction

```bash
make clean && make test    # 39 lignes [PASS], aucune [FAIL]
make run                   # sortie de la section F sur le terminal
```

---

## Jalon K0.4 — GDT, TSS, IDT et exceptions processeur

### A. Objectif

Remplacer les tables héritées du chargeur par des tables propres au noyau, et faire en sorte que **toute exception processeur** soit :

- soit traitée par un gestionnaire enregistré (exception récupérable) ;
- soit transformée en un rapport complet suivi d'une panique.

Le noyau doit aussi survivre à une double faute, y compris quand la pile noyau est inutilisable.

### B. État de départ

Le GDTR du chargeur pouvait être invalide, et l'IDTR était indéfini. Toute exception provoquait un triple fault, donc un redémarrage sans aucun diagnostic.

### C. Fichiers créés et modifiés

| Fichier | Changement |
|---|---|
| `kernel/arch/i386/include/arch/gdt.h`, `gdt.c` | **nouveaux** : 5 descripteurs (nul, code, données, TSS principal, TSS #DF), TSS, pile de 4 KiB pour la double faute |
| `kernel/arch/i386/gdt_flush.asm` | **nouveau** : `gdt_load` (LGDT, saut lointain, rechargement des segments), `tss_load` (LTR) |
| `kernel/arch/i386/include/arch/idt.h`, `idt.c` | **nouveaux** : 256 portes ; 0-31 portes d'interruption, 8 porte de tâche, 32-47 préparées mais non présentes |
| `kernel/arch/i386/include/arch/interrupt.h` | **nouveau** : `struct interrupt_frame`, enregistrement des gestionnaires, noms des exceptions |
| `kernel/arch/i386/isr_stubs.asm` | **nouveau** : 48 stubs générés par macro, `isr_common`, table des stubs |
| `kernel/arch/i386/exceptions.c` | **nouveau** : `isr_dispatch`, décodage des codes d'erreur, rapport, `double_fault_task` |
| `kernel/arch/i386/include/arch/cpu.h` | `sgdt`, `sidt`, `str`, lecture des registres de segment |
| `kernel/core/kmain.c` | `gdt_init()` + `idt_init()` juste après le journal |
| `kernel/tests/test_cpu.c` | **nouveau** : suite K0.4 (40 vérifications) |
| `kernel/tests/test_faults.c` | **nouveau** : modes fatals `ud`, `de`, `gp`, `np`, `intbad`, `df`, `triple` |
| `kernel/tests/ktest.c`, `ktest.h` | suite et modes fatals |
| `tests/qemu/run.sh` | **correction** de la détection de triple fault ; `EXPECT_TRIPLE` |
| `tests/qemu/cases.sh` | 7 nouveaux cas fatals |
| `tests/qemu/gdb_checks.sh` | motif VGA indépendant du numéro de jalon |
| `Makefile` | nouvelles sources C et ASM |

### D. Séquence d'implémentation

1. **GDT et TSS** (`gdt.h`, `gdt.c`, `gdt_flush.asm`). Les structures sont vérifiées par `_Static_assert` (8 et 104 octets). Le TSS principal est chargé dans TR, ce qui est obligatoire pour qu'une commutation de tâche matérielle puisse sauvegarder l'état courant. Le TSS de double faute reçoit un état initial complet.
2. **Stubs** (`isr_stubs.asm`). Les macros `ISR_NOERR`/`ISR_ERR` et `%rep` génèrent 48 points d'entrée. Une vérification par `nm` (48 symboles) et par désassemblage (`isr_stub_13` : `push $0xd ; jmp isr_common`, sans code d'erreur factice) confirme le résultat.
3. **IDT** (`idt.c`) et **répartiteur** (`exceptions.c`).
4. **Intégration** dans `kmain`, avant la validation Multiboot, pour que même ces erreurs soient diagnostiquées.
5. **Tests.** La suite récupérable est exécutée dans le noyau. Les modes fatals sont exécutés chacun dans un démarrage QEMU séparé.

### E. Compilation

`make`. Deux fichiers NASM s'ajoutent. `nm -u` reste vide.

### F. Exécution

`make run`. Nouvelles lignes au démarrage :

```text
[info ] cpu: GDT loaded (flat 4 GiB, TSS main + double-fault task)
[info ] cpu: IDT loaded (32 exception gates, #DF task gate)
```

Exemple de rapport d'exception (`-append test=gp`) :

```text
*** CPU exception 13: General Protection (#GP), error code 0x00000f00
    selector error: GDT index 480
    EAX=0x00000f00 EBX=0x00106d70 ECX=0x00000000 EDX=0x0010ffa7
    ESI=0x0010ffa4 EDI=0x2badb002 EBP=0x0010ff88 ESP=0x0010ff70
    EIP=0x00104838 CS=0008 EFLAGS=0x00000046
    DS=0010 ES=0010 FS=0010 GS=0010
    CR0=0x00000011 CR2=0x00000000 CR3=0x00000000
*** KERNEL PANIC: unhandled General Protection (#GP) at EIP 0x00104838
```

`addr2line -f -e build/samos.elf 0x00104838` donne `ktest_fatal_mode`, la fonction qui contient le `mov %ax, %ds` fautif.

Double faute (`-append test=df`) :

```text
*** DOUBLE FAULT (#DF) caught by task gate, running on dedicated stack
    interrupted task: EIP=0x00104850 ESP=0x0010ff70 EBP=0x0010ff88 EFLAGS=0x00000002
    EAX=0x0000000e EBX=0x00106d70 ECX=0x0000000b EDX=0x0000000e CR3=0x00000000
*** KERNEL PANIC: double fault
```

### G. Tests

```bash
make clean && make test
```

Résultat observé : **46 lignes `[PASS]`, aucune `[FAIL]`**, statut 0.

| Test | Attendu | Observé |
|---|---|---|
| GDTR (base, limite), CS = 0x08, DS/ES/FS/SS = 0x10, TR = 0x18 | conforme | **PASS** |
| Octets d'accès : `0x9B`, `0x93`, TSS principal `0x8B` (occupé), TSS #DF `0x89` | conforme | **PASS** |
| IDTR ; portes 0-31 = `0x8E`/sel. 0x08 ; porte 8 = `0x85`/sel. 0x20 ; 32 et 255 non présentes | conforme | **PASS** |
| `int3` : trappe (EIP après), EAX préservé, DF = 1 → 0 dans le gestionnaire → 1 après IRETD, IF = 0, pile alignée | conforme | **PASS** |
| `ud2`, `div` par 0 : faute (EIP sur l'instruction, octets `0F 0B`/`F7 F1`), reprise après correction d'EIP | conforme | **PASS** |
| Enregistrement : nul refusé, double enregistrement refusé, désenregistrement effectif | conforme | **PASS** |
| Total des autotests noyau | statut 33 | **PASS** (127 vérifications, 0 échec) |
| `exc_ud`, `exc_de` | rapport + panique, statut 37 | **PASS** |
| `exc_gp` : `#GP`, code `0xf00`, « GDT index 480 » | conforme | **PASS** |
| `exc_np` : `INT 0x20` → `#NP`, code `0x102`, « IDT index 32 » | conforme | **PASS** |
| `exc_int_invalid` : `INT 0x80` → `#GP`, code `0x402`, « IDT index 128 » | conforme | **PASS** |
| `exc_df` : double faute par porte de tâche, aucun gestionnaire #NP exécuté, pas de triple fault | conforme | **PASS** |
| `exc_triple` : triple fault **détecté** par le banc | conforme | **PASS** |
| Régressions K0.1 à K0.3 | inchangées | **PASS** |

### H. Débogage : trois constats et un défaut du banc de test

Premier passage : `selftest`, `exc_np`, `exc_df` et `exc_triple` en échec.

1. **`g[2].access == 0x92` faux.** La valeur lue était `0x93`. Le CPU positionne le bit *Accessed* du descripteur de données en écrivant dans la GDT quand DS est chargé. Pour CS, QEMU ne l'a pas positionné. **Correction :** le bit est pré-positionné (`0x9B`/`0x93`) ; le CPU n'écrit plus jamais dans la table, qui pourra être protégée en écriture (Details §22.3).
2. **`INT 0x80` a produit #GP et non #NP.** L'entrée 128 est entièrement nulle, donc de type invalide, et le type est vérifié avant le bit de présence. **Correction :** le test #NP utilise le vecteur 32 (type valide, P = 0). Le comportement #GP de l'entrée nulle est conservé comme cas de test distinct (`intbad`). Le commentaire de `idt.c`, qui annonçait à tort #NP, a été corrigé.
3. **Pas de double faute.** Conséquence directe du point 2 : `INT 0x80` produisait une #GP, traitée normalement. Avec `INT 0x20`, on obtient #NP pendant la livraison de #NP, donc #DF.
4. **Défaut du banc de test : un triple fault n'était pas détecté.** Le cas `exc_triple` a bien provoqué un arrêt de QEMU (statut 0), mais `run.sh` n'a compté que 2 `CPU Reset`. L'exécution manuelle avec `-d cpu_reset,int` montre `check_exception old: 0x8 new 0xd` puis `Triple fault`. **Avec `-no-reboot`, QEMU s'arrête au lieu de réinitialiser le CPU** : aucune ligne `CPU Reset` supplémentaire. L'hypothèse de K0.1 (« un triple fault ajoute un reset ») était fausse. **Correction :** détection de la chaîne `Triple fault`. **Validation :** `exc_triple` l'exige et passe, et les 11 autres cas l'interdisent et passent. Les verdicts antérieurs restent valides, car le contrôle du statut QEMU aurait révélé un triple fault.
5. **Régression d'un test GDB** au passage à K0.4 : le contrôle VGA cherchait le texte littéral `K0.3 boot complete`. Il cherche désormais `boot complete, halting`, indépendant du jalon.

### I. Intégration

- `panic` (K0.3) est le point d'aboutissement de toute exception non gérée.
- K0.5 rendra présents les vecteurs 32-47 et y enregistrera les gestionnaires d'IRQ via `interrupt_register`.
- En K0.6 :
  - le gestionnaire #PF (vecteur 14) sera enregistré ;
  - la page de garde rendra un débordement de pile détectable par #PF, puis #DF, attrapée par la porte de tâche ;
  - le CR3 de la tâche #DF devra être mis à jour (`gdt_set_double_fault_cr3`).

### J. Critères d'achèvement

- [x] GDT et TSS propres chargés, segments rechargés, vérifiés dans les registres réels.
- [x] IDT complète, chaque type de porte vérifié.
- [x] Fautes et trappes distinguées et démontrées ; reprise après faute démontrée.
- [x] Registres, DF, IF et alignement corrects dans les gestionnaires.
- [x] Exceptions fatales : rapport décodé et panique, pour 5 types distincts.
- [x] Double faute attrapée sur une pile dédiée.
- [x] Triple fault effectivement détectable par le banc de test.
- [x] Aucune régression ; documentation à jour (Details §22 à 27).

### K. Reproduction

```bash
make clean && make test    # 46 lignes [PASS], aucune [FAIL]
qemu-system-i386 -kernel build/samos.elf -append test=df -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04
```

---

## Jalon K0.5 — PIC, PIT, clavier PS/2 et file circulaire

### A. Objectif

Faire passer le noyau d'un programme séquentiel à un système **piloté par les interruptions** :

- une horloge périodique ;
- un clavier fonctionnel ;
- une communication sûre entre gestionnaires d'interruption et code normal ;
- une boucle d'inactivité qui dort sans perdre d'événement.

### B. État de départ

IDT complète (K0.4). Les vecteurs 32-47 étaient préparés mais non présents. Le PIC était dans l'état laissé par le BIOS, avec ses vecteurs en collision avec les exceptions, et IF = 0 en permanence. Le noyau s'arrêtait par HLT après le démarrage.

### C. Fichiers créés et modifiés

| Fichier | Changement |
|---|---|
| `kernel/arch/i386/include/arch/pic.h`, `pic.c` | **nouveaux** : 8259A (ICW1-4, masques, EOI, ISR/IRR) |
| `kernel/arch/i386/include/arch/irq.h`, `irq.c` | **nouveaux** : enregistrement, répartition, IRQ parasites, EOI, compteurs |
| `kernel/include/samos/pit.h`, `kernel/drivers/pit.c` | **nouveaux** : PIT canal 0, ticks, uptime, sleep |
| `kernel/include/samos/ringbuf.h`, `kernel/lib/ringbuf.c` | **nouveaux** : file SPSC |
| `kernel/include/samos/kbd_decode.h`, `kernel/drivers/kbd_decode.c` | **nouveaux** : automate scancodes jeu 1 |
| `kernel/include/samos/keyboard.h`, `kernel/drivers/ps2kbd.c` | **nouveaux** : 8042 et clavier |
| `kernel/arch/i386/include/arch/cpu.h` | `irq_save/irq_restore`, `cpu_idle` (`sti; hlt`), `cpu_rdtsc` |
| `kernel/arch/i386/exceptions.c` | vecteurs 0x20-0x2F redirigés vers `irq_dispatch` |
| `kernel/core/kmain.c` | init PIC/PIT/clavier ; boucle d'inactivité avec écho |
| `kernel/tests/test_irq.c` | **nouveau** : suite K0.5 (36 vérifications) et mode `test=kbd` |
| `kernel/tests/test_cpu.c`, `test_faults.c` | **régressions corrigées** (vecteur 32 devenu IRQ 0) |
| `tests/host/test_ringbuf.c`, `test_kbd_decode.c` | **nouveaux** : tests par modèle et automate |
| `tests/qemu/run.sh`, `cases.sh` | `run_case_keys` (frappes via moniteur QEMU), cas `keyboard_e2e` |
| `tests/qemu/gdb_checks.sh` | motif VGA `boot complete` |

### D. Séquence d'implémentation

1. Primitives `cpu.h` : `irq_save/restore`, `cpu_idle`, `rdtsc`.
2. `ringbuf.c` et `kbd_decode.c` : composants **purs**, écrits en premier pour pouvoir être testés sur l'hôte. En relisant le décodeur, j'ai trouvé et supprimé une étiquette `case` dupliquée (`0xAA + 0x50` = `0xFA`) avant la compilation.
3. `pic.c`, puis `irq.c`, puis le raccordement dans `isr_dispatch`.
4. `pit.c`. Une division 64 bits (`/ 1000u`) dans `pit_sleep_ms` a été remplacée par `u64_divmod_u32` avant toute compilation, car elle aurait introduit `__udivdi3`.
5. `ps2kbd.c` (8042 complet).
6. `kmain.c` : initialisation, puis boucle d'inactivité. La première rédaction de la boucle contenait une course (réveil perdu) ; elle a été corrigée à la relecture (Details §28.5).
7. Tests : suite noyau, test interactif, tests hébergés, harnais `sendkey`.

### E. Compilation

`make`. `nm -u` reste vide, ce qui confirme que les multiplications 64 bits (`ticks * diviseur * 1000`) sont générées en ligne, sans `__muldi3`.

### F. Exécution

`make run`. Lignes ajoutées au démarrage :

```text
[info ] irq: 8259 PIC remapped to vectors 0x20-0x2f, all lines masked
[info ] timer: PIT channel 0, 100 Hz (divisor 11932)
[info ] keyboard: 8042 self-test ok, PS/2 keyboard reset ok, IRQ1 enabled
SAMOS: boot stack high-water 792 bytes, guard intact
SAMOS: K0.5 boot complete, interrupts enabled, keyboard echo active
```

Avec `make run-vga`, les touches tapées s'affichent.

### G. Tests

```bash
make clean && make test
```

Résultat observé : **49 lignes `[PASS]`, aucune `[FAIL]`**, statut 0.

| Test | Attendu | Observé |
|---|---|---|
| Masques PIC : IRQ0 et IRQ1 démasquées, toutes les autres masquées ; portes 0x20/0x21 présentes, 0x22 absente ; ISR vide | conforme | **PASS** |
| PIT : diviseur 11932, compteur matériel dans [1, 11932] | conforme | **PASS** |
| 10 ticks consécutifs (EOI correct), une IRQ comptée par tick, aucune IRQ sans gestionnaire | conforme | **PASS** |
| Étalonnage contre la RTC CMOS | 90-110 ticks/s | **PASS** (100) |
| `pit_sleep_ms(50)` | ≥ 50 ms et < 200 ms | **PASS** |
| IRQ 0 masquée pendant 10 périodes : aucun tick ; démasquée : reprise | conforme | **PASS** |
| `pit_sleep_ms` refuse de dormir avec IF = 0 | `false` | **PASS** |
| Total des autotests noyau | statut 33 | **PASS** (163 vérifications) |
| `ringbuf` : 1 million d'opérations comparées au modèle, débordement des compteurs | 0 divergence | **PASS** |
| `kbd_decode` : Shift, Verr. Maj, Ctrl+C, étendues, Shift fantôme, Pause, octets de contrôle, 65 536 paires | conforme, sans erreur ASan/UBSan | **PASS** (21 vérifications) |
| `keyboard_e2e` : frappes `sendkey` | `'Samos K0.5 !'`, `dropped=0` | **PASS** (31 octets, 31 IRQ1) |
| Régressions K0.1-K0.4 (dont exceptions fatales, double faute, triple fault) | inchangé | **PASS** |
| IRQ parasites 7/15 | — | **NOT TESTED** (non reproductible sous QEMU) |

### H. Débogage

Premier passage : `selftest`, `exc_np`, `exc_df` et `keyboard_e2e` en échec.

1. **Régression K0.4 : `(t[32].type_attr & IDT_PRESENT) == 0`.** Le test K0.4 supposait le vecteur 32 inutilisé ; il est devenu l'IRQ 0 du PIT. Le test porte désormais sur le vecteur 0x2F (IRQ 15, inutilisée).
2. **Régression K0.4 : `fatal mode 'np' returned: fault not raised`.** `INT 0x20` exécutait le gestionnaire du **timer** au lieu de provoquer #NP, avec en prime un EOI parasite envoyé au PIC. Les tests #NP et #DF utilisent désormais `INT 0x2E` (code d'erreur `0x172`, « IDT index 46 »). Ce constat est documenté comme un risque de sécurité (Details §28.6).
3. **Clavier muet.** Le noyau affichait `kbd ready` puis `kbd timeout, partial line: ''`. Une session manuelle avec `-monitor stdio` montrait que QEMU recevait bien les `sendkey`. Le compteur ajouté au message d'expiration a confirmé qu'aucun octet n'était reçu. **Cause :** l'octet de configuration du 8042, lu après la commande `0xAD`, contenait le bit 4 (horloge du port 1 coupée), et sa réécriture finale annulait la commande d'activation `0xAE`. **Correction :** le bit 4 est effacé explicitement. **Résultat :** `'Samos K0.5 !' (bytes=31 dropped=0 irq1=31)`.
4. **Test hébergé du décodeur non lié** (`undefined reference to samos_strcmp`). Le test utilise `strcmp`, renommé par `host_prefix.h` : `kernel/lib/string.c` a été ajouté à sa ligne de compilation.

### I. Intégration

- Les IRQ passent par l'infrastructure K0.4 : stubs, `isr_common`, `isr_dispatch`. Aucun nouveau code assembleur n'a été nécessaire.
- La règle « pas de journal en IRQ » (K0.3, §18.5) est respectée par tous les gestionnaires.
- K0.6 pourra mesurer les temps (`pit_uptime_ms`) et devra préserver le fonctionnement des IRQ lors de l'activation de la pagination : les gestionnaires, leur pile et les tampons doivent rester mappés.

### J. Critères d'achèvement

- [x] Les IRQ ne sont plus confondues avec les exceptions ; masquage par défaut.
- [x] Timer périodique à la fréquence attendue, mesurée contre une source indépendante.
- [x] EOI démontré ; masquage démontré.
- [x] Clavier fonctionnel de bout en bout avec de vraies frappes, sans perte.
- [x] File ISR → noyau sans verrou, vérifiée par modèle.
- [x] Boucle d'inactivité sans réveil perdu.
- [x] Régressions détectées et corrigées ; documentation à jour (Details §28 à 32).

### K. Reproduction

```bash
make clean && make test    # 49 lignes [PASS], aucune [FAIL]
make run-vga               # taper au clavier dans la fenêtre QEMU : écho à l'écran
```

---

## Jalon K0.6 — Carte mémoire, allocateur physique, pagination et protections

### A. Objectif

Donner au noyau :

- une connaissance **fiable** de la mémoire physique ;
- un allocateur de cadres **sûr** ;
- une **pagination** qui applique enfin les protections prévues depuis K0.2 : page nulle, code en lecture seule, page de garde de pile.

C'est le dernier jalon de K0 : il est suivi de l'arrêt du développement fonctionnel et de la revue complète.

### B. État de départ

Le noyau s'exécutait en mémoire physique, sans aucune protection. Les droits R-X/R--/RW- des segments ELF n'étaient qu'une intention. La garde de pile n'était vérifiée que par logiciel, a posteriori. La carte mémoire était seulement localisée et bornée par `bootinfo` (K0.1), jamais lue.

### C. Fichiers créés et modifiés

| Fichier | Changement |
|---|---|
| `kernel/include/samos/memmap.h`, `kernel/mm/memmap.c` | **nouveaux** : parseur défensif, tri, repli |
| `kernel/include/samos/pmm.h`, `kernel/mm/pmm_core.c` | **nouveaux** : cœur pur de l'allocateur par bitmap |
| `kernel/mm/pmm.c` | **nouveau** : instance du noyau (bitmap statique, réserves, remise à zéro) |
| `kernel/include/samos/mm.h`, `kernel/mm/mm.c` | **nouveaux** : orchestration, avertissement au-delà de 1 GiB |
| `kernel/arch/i386/include/arch/paging.h`, `kernel/arch/i386/paging.c` | **nouveaux** : tables, `vmm_*`, #PF, diagnostic |
| `kernel/arch/i386/probe.asm` | **nouveau** : sondes et correction d'exception |
| `kernel/arch/i386/exceptions.c`, `interrupt.h` | `exception_report` public ; diagnostic CR2 dans la tâche #DF |
| `kernel/core/layout.c`, `layout.h` | vérification de la garde par le MMU quand la pagination est active |
| `kernel/core/kmain.c` | `mm_init()` avant les IRQ ; message final K0.6 |
| `kernel/tests/test_mm.c` | **nouveau** : suite K0.6 (60 vérifications) |
| `kernel/tests/test_faults.c` | modes `null`, `wtext`, `wrodata`, `unmapped`, `stack` |
| `kernel/tests/test_layout.c` | test de débordement simulé : refusé par le MMU après K0.6 |
| `tests/host/test_memmap.c`, `test_pmm.c` | **nouveaux** |
| `tests/qemu/run.sh`, `cases.sh` | `QEMU_MEM` par cas ; 10 nouveaux cas |
| `tests/qemu/gdb_checks.sh` | motifs VGA adaptés au défilement |
| `docs/review/K0-review.md` | **nouveau** : revue de fin de phase |

### D. Séquence d'implémentation

1. **Composants purs d'abord** : `memmap.c` et `pmm_core.c`, avec leurs tests hébergés. Ces tests ont réussi dès leur première exécution, notamment les cartes hostiles et les 200 000 opérations contre le modèle. Pendant la rédaction, j'ai réordonné le calcul d'arrondi de `to_frames` pour que le contrôle de débordement précède l'addition.
2. `pmm.c` : bitmap statique de 128 KiB, réserves (mémoire basse, image du noyau, structures Multiboot), remise à zéro à l'allocation.
3. `probe.asm` puis `paging.c`. J'ai placé la pagination dans `arch/i386/`, et non dans `mm/`, car le format des tables est propre à l'architecture : `mm/` reste portable vers x86-64.
4. `mm.c` et l'intégration dans `kmain`, **avant** le PIC, pour que le timer et le clavier fonctionnent d'emblée sous pagination.
5. Adaptation de K0.2 : la page de garde n'est plus lisible (#PF). `boot_stack_guard_intact()` vérifie donc qu'elle est non projetée, et le test de débordement simulé passe par une sonde, qui doit échouer.
6. Tests noyau, modes fatals, tailles de RAM.

### E. Compilation

`make`. Nouvelle source assembleur : `probe.asm`. Taille de l'image : `.text` 32 KiB, `.bss` 139 KiB (dont la bitmap de 128 KiB), `.bootstack` 20 KiB (garde comprise). `nm -u` reste vide.

### F. Exécution

`make run`. Nouvelles lignes :

```text
[info ] mm: 0x000000000-0x00009fbff available
[info ] mm: 0x00009fc00-0x00009ffff reserved
[info ] mm: 0x0000f0000-0x0000fffff reserved
[info ] mm: 0x000100000-0x007fdffff available
[info ] mm: 0x007fe0000-0x007ffffff reserved
[info ] mm: 0x0fffc0000-0x0ffffffff reserved
[info ] mm: 6 regions, 130559 KiB available
[info ] pmm: 32736 frames managed (130944 KiB), 32426 free (129704 KiB), 7 reserved ranges
[info ] paging: enabled (CR0.PG|WP), identity map 0x00001000-0x07fe0000, 32 page tables, null page and stack guard unmapped, .text/.rodata read-only
[info ] pmm: 32736 frames managed (130944 KiB), 32393 free (129572 KiB), 7 reserved ranges
...
SAMOS: boot stack high-water 836 bytes, guard page unmapped (MMU-enforced)
SAMOS: K0.6 boot complete, paging on, interrupts enabled, keyboard echo active
```

Vérification arithmétique : $32426 - 32393 = 33$ cadres, soit 1 répertoire et 32 tables ($\lceil \texttt{0x07fe0000} / 4\,\text{MiB} \rceil = 32$).

Débordement de pile (`-append test=stack`) :

```text
*** DOUBLE FAULT (#DF) caught by task gate, running on dedicated stack
    last page-fault address CR2=0x00131ffc: kernel stack overflow (guard page below the boot stack)
*** KERNEL PANIC: double fault
```

`0x00131ffc` est le dernier mot de la page de garde `[0x131000, 0x132000)`, exactement là où le CPU a tenté d'empiler le cadre de la #PF.

### G. Tests

```bash
make clean && make test
```

Résultat observé : **61 lignes `[PASS]`, aucune `[FAIL]`**, statut 0.

| Test | Attendu | Observé |
|---|---|---|
| Carte : 10 scénarios hostiles + 20 000 cartes aléatoires (hébergé, ASan) | aucune lecture hors tampon, résultat trié | **PASS** (15 vérifications) |
| PMM : arrondis, chevauchements, I1-I4, épuisement, 200 000 opérations contre le modèle (hébergé) | conforme | **PASS** (1958 vérifications) |
| Carte réelle sous QEMU : triée, non malformée, > 100 MiB disponibles | conforme | **PASS** |
| Allocation : alignée, hors noyau, ≥ 1 MiB ; cadre recyclé remis à zéro | conforme | **PASS** |
| Libérations refusées : double, noyau, VGA, cadre 0, non alignée, hors mémoire | conforme | **PASS** |
| Épuisement : 32 393 cadres alloués, tous valides, puis rendus | conforme | **PASS** (710 ms) |
| CR0.PG + WP, CR3, CR3 de la tâche #DF | conforme | **PASS** |
| Traductions : `.text`/`.rodata` R, `.data`/VGA RW, page nulle et garde absentes, superviseur | conforme | **PASS** |
| Sondes : 9 accès légitimes ou interdits, avec CR2 et code d'erreur exacts | conforme | **PASS** |
| `vmm_map/unmap` : alias physique, EEXIST, EINVAL, INVLPG effectif, projection R | conforme | **PASS** |
| Total des autotests noyau | statut 33 | **PASS** (223 vérifications) |
| `pf_null`, `pf_write_text`, `pf_write_rodata`, `pf_unmapped` | diagnostic exact, statut 37 | **PASS** |
| `stack_overflow` : #DF par porte de tâche, diagnostic, pas de triple fault | conforme | **PASS** |
| `mem_16M`, `mem_512M`, `mem_1500M` (plafond 1 GiB signalé), `mem_4G` (région > 4 GiB ignorée), autotests à 512 MiB | conforme | **PASS** |
| Régressions K0.1-K0.5 | inchangé | **PASS** |

### H. Débogage

1. **`error: all paths through this function will call itself [-Winfinite-recursion]`.** Clang a détecté la récursion volontaire du mode `stack`. Plutôt que de désactiver l'avertissement, j'ai ajouté une borne `volatile` jamais atteinte, qui rend la récursion formellement finie.
2. **Mesure « 0 ms » pour l'épuisement de 129 MiB.** Le résultat était physiquement impossible. Cause : la suite K0.5 se termine par `cli`, donc le PIT ne comptait plus. La mesure est désormais faite avec IF = 1, ce qui donne 710 ms. La remise à zéro, elle, était prouvée indépendamment par le test du cadre sali.
3. **Motif obsolète `bytes, guard intact`.** Le message de démarrage a changé volontairement (`guard page unmapped (MMU-enforced)`).
4. **Test GDB VGA en échec.** Le journal de démarrage dépasse désormais 25 lignes : l'écran a défilé et `SAMOS: kernel entry` n'est plus dans le tampon (la première ligne visible est `SAMOS: kernel image ...`). Le noyau est correct, le défilement ayant été testé en K0.3. Le test cherche maintenant des lignes de fin de démarrage.
5. **Avertissement ajouté** après les essais de tailles de RAM : à 1,5 GiB et 4 GiB, le plafond de 1 GiB était appliqué **silencieusement**. Un message `mm: RAM above 1024 MiB is not managed in K0` le signale désormais, et un cas de test l'exige.

### I. Intégration

- K0.2 : les régions de `layout.c` déterminent les droits des pages, et la page de garde devient non présente.
- K0.4 : #PF est enregistrée par `interrupt_register` ; la tâche #DF reçoit le CR3 du noyau et diagnostique le débordement.
- K0.5 : les IRQ sont activées **après** la pagination. Le timer, le clavier et leurs tampons fonctionnent sous pagination (toute la suite K0.5 s'exécute avec PG = 1).

### J. Critères d'achèvement

- [x] Carte mémoire lue et validée, robuste aux cartes hostiles.
- [x] Régions utilisables et réservées identifiées ; noyau et structures de démarrage réservés.
- [x] Allocateur de cadres avec invariants vérifiés (I1-I4).
- [x] Pagination active ; traduction vérifiée (identité et alias).
- [x] Fautes de page diagnostiquées ; protections démontrées (NULL, `.text`, `.rodata`, garde, non projeté).
- [x] Débordement de pile attrapé sans triple fault.
- [x] Aucune régression ; documentation à jour (Details §33 à 38).
- [x] **Arrêt du développement fonctionnel** ; revue `docs/review/K0-review.md`.

### K. Reproduction

```bash
make clean && make test    # 61 lignes [PASS], aucune [FAIL]
qemu-system-i386 -m 128M -kernel build/samos.elf -append test=stack -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04
```

---

## Phase K0.7 — Durcissement, vérification indépendante et préparation de la publication

Phase autorisée par le développeur après la revue de K0. Elle ne comporte **aucune** fonctionnalité de K1 et aucun travail de portage 64 bits. Le compte rendu complet se trouve dans `docs/review/K0.7-review.md`. Les étapes ci-dessous sont consignées dans l'ordre réel d'exécution.

### K0.7.0 — Nouvel audit et preuves des défauts mémoire

Le noyau a été relu en entier, sans s'appuyer sur la revue K0. Les défauts de l'API mémoire ont d'abord été **reproduits sur le code d'origine** (commit `697dc01`), dans un *worktree* Git isolé, à l'aide d'un fichier de test temporaire non commité. Ce fichier est conservé sous `docs/review/evidence/k06-evidence.c.txt`, et le journal sous `docs/review/evidence/k06-memory-defects.txt` :

```text
[EVID] E1 vmm_map(0x0) = 0
[EVID] E1 read NULL after map: probe=0 (0 = NULL deref no longer faults)
[EVID] E2 vmm_map(guard) = 0
[EVID] E3 vmm_map(0xD0000000 -> .text, W) = 0
[EVID] E3 write via alias = 0, kmain first word 53e58955 -> cccccccc
[EVID] E4 pmm_free_frame(live page table 00137000) = 0 (0 = accepted)
[EVID] E5 pmm_free_frame(still mapped) = 0
[EVID] E5 realloc returned 0015a000 (same frame=1) while 0xD0001000 still maps it
```

Ces résultats contredisent la revue K0, qui classait ces risques MEDIUM (« sans objet en K0 »). Ils sont reclassés **HIGH** : l'API du noyau permet de violer les invariants qu'il prétend garantir.

### K0.7.1 — Images de production et de test séparées ; vérification du banc

**Fichiers.**

- `Makefile` : deux images (`build/samos.elf`, `build/samos-test.elf`), objets séparés (`build/prod/`, `build/test/`), macros `CONFIG_KTEST` et `CONFIG_QEMU_DEBUGCON`, option `-ffile-prefix-map`, cibles `run-test`, `test-mutation` et `test-repro`.
- `kernel/include/samos/config.h` : **erreur de compilation** si une macro de configuration manque. Une image ne peut donc pas recevoir le code de test par oubli.
- `kmain.c`, `panic.c`, `log.c`, `qemu_dbg.c` : code de test, `qemu_exit` et debugcon conditionnés. En production, une panique récursive écrit sur le port série ; une ligne de commande `test=...` est refusée avec un avertissement.
- `kernel/tests/ktest.c` : bilan par suite (`suite X done: N checks, F failures`).
- `tools/check_elf.sh` : mode `--production`, qui exige l'absence des symboles de test et de toute instruction `out` vers `0xE9`/`0xF4` ; mode `--test`.
- `tests/qemu/run.sh` : réécrit, avec une seule fonction de verdict pour les deux types de cas. Ajouts : `IMG=prod|test`, motifs `re:`, échec si QEMU ou l'image est absent, si le journal QEMU est vide ou si aucun cas n'a été exécuté.
- `tests/qemu/cases.sh` : démarrages de production ; cas `prod_refuses_test` ; bilan de chaque suite exigé avec N > 0 et 0 échec.
- `tests/qemu/gdb_checks.sh` : image de production, journaux série.
- `tests/tools/check_elf_negative.sh` : l'image de test présentée comme production doit être rejetée.
- `tests/tools/harness_selftest.sh` : **nouveau**, 8 scénarios où le banc QEMU doit échouer, plus une contre-épreuve.

**Vérification de la vérification.** Avant d'en faire un test permanent, `check_elf.sh --production` a été lancé sur l'image de test. Il a rejeté l'image avec 5 diagnostics : 4 symboles de test et l'écriture sur un port QEMU, alors que l'image de test contient 14 instructions `out` vers ces ports. Pour chaque scénario de `harness_selftest.sh`, la **cause** de l'échec a été vérifiée dans le journal correspondant :

| Scénario | Cause observée |
|---|---|
| H1 QEMU introuvable | `QEMU introuvable` |
| H2 image absente | `image absente` |
| H3 motif inexistant | `motif absent` |
| H4 statut incorrect | `statut QEMU 37, attendu 33` |
| H5 motif interdit | `motif interdit present` |
| H6 aucun cas | `aucun cas exécuté` |
| H7 triple fault non attendu | `triple fault ou reset inattendu (triple=1, resets=2)` |
| H8 image inconnue | `statut QEMU 1, attendu 124` |

**Résultat :** `make test` donne 24 cas QEMU sur 24, les vérifications GDB, les tests hébergés et les tests d'outils au vert. Statut 0.

### K0.7.2 — Chemin d'interruption : segments, fautes imbriquées, concurrence

| Fichier | Changement | Pourquoi |
|---|---|---|
| `isr_stubs.asm` | segments zéro-étendus (`xor eax,eax ; mov ax,ds ; push eax`) au lieu de `push ds` | SDM vol. 2 (PUSH) : sur les processeurs récents, `push` d'un segment n'écrit que 16 bits, et les 16 bits hauts du cadre gardent des restes de la pile. QEMU zéro-étend, ce qui masquait le problème. **Défaut supposé d'après la spécification, non reproductible sous QEMU.** |
| `exceptions.c` | garde d'imbrication : une exception pendant une exception (hors sonde) produit un rapport `NESTED` puis une panique | sinon une faute répétée dans `exception_report`/`kprintf` ré-entrerait jusqu'à épuiser la pile |
| `exceptions.c`, `irq.c`, `interrupt.h` | `cpu_in_interrupt()` : profondeur d'exception et d'IRQ | permettre aux fonctions non réentrantes de refuser le contexte d'interruption |
| `pic.c` | `pic_mask/unmask` sous `irq_save` | lecture-modification-écriture de l'IMR non atomique |
| `vga.c` | écritures sous `irq_save` ; état ramené dans les bornes avant tout accès | une panique en IRQ au milieu d'une écriture pouvait laisser `g_col == 80` et écrire hors de l'écran (sur la ligne 25 inexistante) |

Tests ajoutés :

- dans le gestionnaire `int3` : les 32 bits de `ds/es/fs/gs` valent exactement `0x10` ; `cpu_in_interrupt()` est vrai dans le gestionnaire et faux après ;
- démarrage `test=nested` : `NESTED CPU exception 6 (Invalid Opcode (#UD)) ... while handling exception 3 (Breakpoint (#BP))`, puis panique, sans panique récursive.

Résultat : `make test` réussi, avec 25 cas QEMU sur 25.

**Défaut de construction trouvé après coup (corrigé dans un commit séparé).** Le `Makefile` de l'étape K0.7.1 contenait une règle à motifs à deux cibles (`$(BUILD)/prod/%.o $(BUILD)/test/%.o: %.asm`). Pour GNU Make, une telle règle signifie qu'une **seule** exécution produit les **deux** cibles. Après `make clean`, les objets assembleur de l'image de test n'étaient donc jamais produits (`ld: cannot find build/test/kernel/arch/i386/boot/entry.o`). Les compilations incrémentales masquaient le défaut ; une compilation depuis un arbre propre l'a révélé. La règle a été scindée en deux. Résultat vérifié depuis `make clean` : `make test` donne 97 lignes `[PASS]` et aucun `[FAIL]` ; `make -j8` réussit aussi.

### K0.7-M1 — Page nulle et page de garde jamais projetables (E1, E2)

**Méthode (décidée après deux interruptions).** Les changements se font par petites modifications ciblées, avec compilation et tests après chacune, et un commit par sous-jalon validé.

**Changements.**

| Fichier | Changement |
|---|---|
| `kernel/include/samos/errno.h` | `SAMOS_EPERM` : refus imposé par une règle de sécurité |
| `kernel/arch/i386/paging.c` | `va_must_stay_unmapped()` : vrai pour la page nulle et la page de garde. `vmm_map()` répond `SAMOS_EPERM` pour ces pages, quels que soient le cadre et les droits demandés. Le refus est **dans l'API**, il ne dépend pas des appelants. `paging_init()` contournait déjà ces deux pages ; il n'est pas affecté. |
| `kernel/tests/test_mm.c` | `test_forbidden_mappings()` : 14 vérifications. Les quatre tentatives (nulle et garde, en RW puis en R) sont refusées. Après ces tentatives, les deux pages ne sont toujours pas traduites et fautent toujours. Une adresse voisine légitime reste projetable. |

**Vérification.**

- Test ciblé (`-append test`) : statut 33, 239 vérifications, 0 échec.
- **Contre-épreuve par injection de défaut.** Dans une copie isolée des sources, la garde a été neutralisée (`if (0 && va_must_stay_unmapped(va))`, substitution vérifiée : 1 occurrence avant, 1 après). Résultat : statut 35, **8 échecs**, dont `mem_probe_read((const void *)0, &v) == -1`. La page nulle redevenait donc lisible, et le test le détecte.
- Régression complète depuis `make clean` : 97 `[PASS]`, 0 `[FAIL]`, 25 cas QEMU sur 25. Compilation parallèle propre réussie.

**Limite assumée à ce stade.** M1 ferme E1 et E2. E3 (alias inscriptible), E4 (table de pages libérable) et E5 (cadre projeté libérable) restent ouverts jusqu'aux sous-jalons M2 à M5.

### K0.7-M2.1 — Conception du suivi des cadres physiques

**Règle de travail :** une seule modification architecturale cohérente par session.

**Fichiers.**

| Fichier | Rôle |
|---|---|
| `docs/design/K0.7-M2-frame-tracking.md` | conception : type, propriété et références distingués ; transitions ; invariants F1-F3 (descripteur) et G1-G5 (globaux) ; traitement de la projection en identité ; dépendance avec le VMM ; étude du `stash` ; plan M2.2-M5 |
| `kernel/include/samos/frame.h` | descripteur de 16 bits (3 bits de type, bit 3 réservé, 12 bits de références), types, propriétaire déduit du type, `frame_desc_valid()`. **Définitions seules**, incluses par aucun fichier du noyau à ce stade |
| `tests/host/test_frame.c`, `tests/host/cases.sh` | aller-retour de l'encodage, invariants F1-F3, propriété ; contrôle **exhaustif** des 65 536 valeurs (exactement 5 + 4095 descripteurs valides) |

**Décision sur la projection en identité.** Elle n'est **pas** comptée dans les références. La compter donnerait à chaque cadre au moins une référence permanente et rendrait toute libération impossible. La limite qui en découle (l'identité reste un alias inscriptible des cadres alloués et des tables de pages) est documentée ; son traitement est reporté à M3, avec justification à fournir.

**Dépendance identifiée.** `paging.c` alloue ses tables par `pmm_alloc_frame()` : le PMM ne peut pas les reconnaître sans une modification du VMM, prévue en M3. `paging.c` n'est pas modifié en M2.1.

**Vérification.**

- `frame.h` compile avec les options réelles des deux images. Un premier essai, qui compilait l'en-tête comme fichier principal, signalait des fonctions `static inline` inutilisées : c'est un artefact de cette méthode, l'avertissement ne s'applique pas à un en-tête inclus. L'essai a été refait par inclusion.
- **Absence de changement de comportement prouvée :** les octets chargés (`objcopy -O binary`) et les ELF sans débogage sont **identiques** avant (`072fe23`) et après, pour les deux images.
- Régression depuis `make clean` : 98 `[PASS]`, 0 `[FAIL]`, 25 cas QEMU sur 25. Compilation parallèle propre réussie.

**Constat annexe (non corrigé ici).** Les ELF complets diffèrent entre deux répertoires de compilation : NASM écrit le répertoire absolu (`DW_AT_comp_dir`) dans ses informations DWARF, et `-ffile-prefix-map` ne s'applique qu'à Clang. Ce défaut de reproductibilité sera traité dans un sous-jalon dédié.

### K0.7-M2.2 — Descripteurs de cadres intégrés au PMM

**Reprise après interruption.** Des modifications non commitées existaient dans `pmm.h`, `pmm_core.c`, `test_mm.c` et le document de conception. Elles ont été relues avant d'être conservées. Corrections apportées :

- empêcher la rétrogradation d'un cadre `KERNEL` en `RESERVED` ;
- commentaires obsolètes (« bitmap ») ;
- une assertion tautologique du test hébergé remplacée par une vraie vérification d'atomicité ;
- une formule du document de conception corrompue : des barres obliques inverses avaient été interprétées comme caractères de contrôle. La formule a été réécrite en texte simple, et aucun fichier modifié ne contient plus de caractère de contrôle.

**Changements.**

- `pmm_core.c`, `pmm.h` : table de descripteurs à la place de la bitmap ; `set_type()` seul point de modification de `free_count` ; `pmm_core_reserve_as()` (refus atomique, `KERNEL` jamais rétrogradé) ; `pmm_core_type()` ; `pmm_core_check()`.
- `pmm.c` : table statique `g_meta[2^18]` (512 KiB), réservée comme partie de l'image (`FRAME_KERNEL`).
- `mm.c` : `pmm_core_check()` exécuté à chaque démarrage, production comprise.
- Tests :
  - hébergé `test_descriptors()` : types, chevauchements, refus atomique, capacité, région se terminant en 2^64, **corruptions délibérées détectées** ;
  - noyau `test_frame_types()` : table des descripteurs `KERNEL` et non libérable, aux deux bouts ;
  - QEMU : ligne `descriptor table consistent` exigée de 16 MiB à 4 GiB et en production.

**Défaut trouvé par le test hébergé.** Le calcul de la fin de la mémoire gérée, réécrit contre le débordement en 2^64, comptait un dernier cadre partiel (`0x904` au lieu de `0x903`). La sémantique de K0.6 a été rétablie sans débordement.

**Vérification.**

- `make clean && make test` : statut 0, **99 `[PASS]`, 0 `[FAIL]`**, 26 cas QEMU sur 26, `host pmm_core` à 1986 vérifications. Compilation parallèle depuis zéro réussie.
- Injection de défauts, dans une copie isolée, sur 6 mutants (substitution vérifiée à 1 occurrence) : **6 sur 6 détectés** (rétrogradation `KERNEL`, refus non atomique, dérive de `free_count`, vérificateur aveugle, E1/E2 désactivés, double libération acceptée).

### K0.7-M2.3 (partie 1) — Allocation typée des tables de pages

**Statut : partiel.** Le code est en place et la régression passe, mais les tests dédiés à E4 et la contre-épreuve n'ont **pas** encore été écrits (session interrompue). E4 n'est donc pas encore *démontré* corrigé.

- `pmm_core_alloc_as()` (types `ALLOCATED` ou `PAGETABLE` seulement) ; `pmm_alloc_pagetable()` côté noyau.
- `paging.c` : le répertoire et les tables de pages sont alloués avec le type `FRAME_PAGETABLE`. `pmm_core_free()` refusant tout cadre non `ALLOCATED`, ils ne sont plus libérables par `pmm_free_frame()`.
- Aucune structure de pagination statique n'existe. Politique de destruction : aucune en K0 (aucun chemin ne retire une table) ; à revoir en K1.
- Vérification : `make clean && make test` donne 99 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 ; compilation parallèle propre réussie.
- Reste à faire : les tests E4 (type, refus, contenu et compteurs inchangés, pagination intacte, scénario E4 rejoué), la contre-épreuve isolée et la mise à jour du document de conception.

### K0.7-M2.3 (finalisation) — E4 démontré corrigé

Tests ajoutés un par un dans `test_pagetable_frames` (`kernel/tests/test_mm.c`), chacun compilé et exécuté avant le suivant :

1. type `PAGETABLE` du répertoire actif (251 vérifications, 0 échec) ;
2. refus de sa libération, avec type, compteurs et PDE inchangés, CR3, traduction et accès à `kmain` intacts (259) ;
3. même chose sur la table active qui projette `kmain`, après vérification que c'est bien la structure attendue (267) ;
4. cadre ordinaire alloué, libéré puis réalloué ; E1 et E2 toujours refusés ; `pmm_core_check` à 0 (274).

**Contre-épreuves (copies isolées).**

- Mutant A (libération acceptant `PAGETABLE`) : statut 35, 7 échecs ciblés (`pmm_free_frame(paging_directory()) == SAMOS_EINVAL`, type, `free_count`…).
- Mutant B (tables allouées par `pmm_alloc_frame`, soit la condition d'origine de E4) : statut 35, échecs sur le type et sur le refus.
- Aucun triple fault. Un mutant plus large (`pmm_core_free` acceptant tout type) a provoqué une panique avant d'atteindre ces tests : il a été écarté comme preuve non spécifique.

**Validation.** `make clean && make test` donne un statut 0, 99 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 (RAM de 16 MiB à 4 GiB, production, tests négatifs du banc). Compilation parallèle depuis zéro réussie.

**Toujours ouverts :** E3 (alias inscriptibles, M2.5) et E5 (références, M2.4).

### K0.7-M2.4a — Opérations de références dans le cœur du PMM

Premier sous-jalon de M2.4 (correction de E5), limité au cœur pur. `paging.c` et `vmm_map()` ne sont pas modifiés.

- `kernel/include/samos/pmm.h`, `kernel/mm/pmm_core.c` :
  - `pmm_core_ref()`, `pmm_core_unref()`, `pmm_core_refs()` et le compteur `ref_errors` ;
  - seuls les cadres `FRAME_ALLOCATED` sont acceptés (sinon `SAMOS_EPERM`) ;
  - borne de 4095 et refus de passer sous 0 (`SAMOS_ERANGE`) ;
  - chaque refus est décidé avant toute écriture, donc le descripteur reste intact.
- `tests/host/test_pmm_refs.c` (24 vérifications) : +1 et −1 exacts, sous-dépassement, 4095 références acceptées puis la suivante refusée, types `KERNEL`, `RESERVED`, `FREE` et `PAGETABLE` refusés, cadre hors limites, valeur de `ref_errors`, descripteurs inchangés après chaque refus, libération et `pmm_core_check` inchangées.
- **Contre-épreuves** (copie isolée) : chacune des trois gardes supprimées est détectée par l'assertion attendue (débordement ligne 41, sous-dépassement ligne 34, type ligne 50). Sans la garde de type, ASan signale aussi une écriture hors de la table.
- **Limite connue :** `pmm_core_check()` compte encore toute référence non nulle comme une anomalie (règle de M2.2). C'est exact tant qu'aucun code noyau ne prend de référence ; la règle devra être ramenée à F2 lors de l'intégration (M2.4b).
- Validation : `make clean && make test` donne 100 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 ; compilation parallèle propre réussie.

### K0.7-M2.4b — Références intégrées au PMM noyau

- `pmm_core_check()` : la règle provisoire de M2.2 est remplacée par F2 (déjà appliqué par `frame_desc_valid()`).
- `pmm_core_free()` : `SAMOS_EBUSY` si des références existent (compteur `busy_frees`), décidé avant toute écriture.
- `pmm.c`, `pmm.h` : `pmm_ref`, `pmm_unref`, `pmm_refs`, avec refus des adresses non alignées.
- Tests :
  - `tests/host/test_pmm_busy.c` (19 vérifications) : libération à 0, 1 et 2 références ; descripteur, `free_count` et compteurs inchangés lors d'un refus ; libération après retrait de toutes les références ; `pmm_core_check` accepte des références valides et rejette des références sur `PAGETABLE` ou `FREE` ; E4 toujours refusé ;
  - `test_kernel_refs` (noyau) : mêmes propriétés via l'interface physique ; refus des cadres non alignés, du noyau, du répertoire de pages et des cadres libres.
- **Contre-épreuve** (copie isolée, garde `refs > 0` supprimée) : test hébergé à 8 échecs (premier : `pmm_core_free(&p, b) == SAMOS_EBUSY`) ; noyau au statut 35 avec 4 échecs ciblés (premier : `pmm_free_frame(f) == SAMOS_EBUSY`) ; aucun triple fault.
- Validation : `make clean && make test` donne 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 ; compilation parallèle propre réussie.
- **E5 n'est pas corrigé** : le VMM ne prend pas encore de références (M2.4c).

### K0.7-M2.4c — Références prises par vmm_map

- `paging.c` :
  - `static map_page(..., counted)` ; `vmm_map()` passe `counted = true`, `paging_init()` passe `counted = false` (identité non comptée) ;
  - ce chemin interne est protégé par `KASSERT(!paging_enabled())` ;
  - une référence est prise en dernier, juste avant l'écriture de la PTE ; un cadre `FREE` est refusé.
- **Régression attendue observée.** Sans relâchement dans `vmm_unmap()`, deux tests existants échouaient à la libération (`test_mm.c` lignes 184 et 210). Ils vérifient maintenant le compte exact de références (2 et 1) et les relâchent explicitement, de façon temporaire jusqu'à M2.4d.
- **Nouveau test `test_vmm_refs`** (noyau) :
  - aucune référence après le démarrage ;
  - +1 exact, 2 pour deux projections, traduction correcte ;
  - aucun effet sur le compteur pour E1, E2, `va` ou `pa` non alignées, droits invalides, `EEXIST` et cadre `FREE` ;
  - libération refusée tant que le cadre est projeté ;
  - `ERANGE` à 4095 sans PTE installée ;
  - `ENOMEM`, mémoire épuisée, sans référence prise ;
  - nettoyage complet, E4 toujours refusé, `pmm_core_check` à 0.
- **Contre-épreuve** (prise de référence supprimée) : statut 35, 16 échecs, premiers : `vmm_map(va1, f, PTE_WRITE) == SAMOS_OK && pmm_refs(f) == 1` et `pmm_free_frame(f) == SAMOS_EBUSY` ; aucun triple fault.
- Validation : `make clean && make test` donne 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26, dont `selftest_512M` ; compilation parallèle propre réussie.
- **E5 reste ouvert** (M2.4d : relâchement dans `vmm_unmap`).

### K0.7-M2.4d — Références relâchées par vmm_unmap

- `paging.c` : `vmm_unmap()` refuse une adresse non alignée (`EINVAL`) et les adresses de l'identité (`EPERM`). Pour un cadre `ALLOCATED`, il vérifie que la référence existe, efface la PTE, exécute `INVLPG`, puis appelle `pmm_unref()` (vérifié par `KASSERT`).
- **Défauts existants trouvés et corrigés :** arrondi silencieux des adresses non alignées ; possibilité de supprimer une projection de l'identité, y compris celle du code du noyau.
- **Tests :**
  - les relâchements temporaires de M2.4c échouaient bien une fois `vmm_unmap()` corrigé (le sous-dépassement est refusé). Ils sont retirés, et les tests vérifient maintenant 0 référence après les retraits ;
  - nouveau `test_vmm_unmap_refs` :
    - 1 → 0 référence ;
    - 2 → 1 → 0 avec deux adresses ;
    - libération refusée puis autorisée ;
    - double retrait refusé sans sous-dépassement ;
    - `EINVAL` et `ENOENT` sans effet ;
    - identité (`kmain`, VGA, page 0) refusée et intacte ;
    - traduction et TLB cohérents après retrait ;
    - E1, E2 et E4 intacts ;
    - `pmm_core_check` à 0.
- **Contre-épreuves :** C1 (pas de décrémentation) donne un statut 35 avec des échecs ciblés ; C2 (`pmm_unref` défaillant) donne un arrêt explicite `assertion failed: e == SAMOS_OK`, statut 37 ; aucun triple fault.
- **Validation :** `make clean && make test` donne 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 ; compilation parallèle propre réussie ; 341 vérifications noyau.
- **E5 reste ouvert** (M2.4e : démonstration de bout en bout).

### K0.7-M2.4e — Validation finale de E5

- **`test_e5_scenario`** (noyau) : les 10 étapes du scénario d'origine, plus un second scénario à deux projections. Les autotests passent de 341 à 367 vérifications, 0 échec.
- **Erreur de méthode corrigée.** Une première exécution affichait « 341 vérifications » alors que la compilation avait échoué (variable inutilisée) : QEMU avait démarré l'ancien binaire. Les exécutions suivantes sont conditionnées à la réussite de la compilation.
- **Contre-épreuves** (statut 35, échecs relevés *dans* le scénario E5, sans triple fault) :
  - X1 (pas de `pmm_ref`) et X2 (pas de refus `EBUSY`) reproduisent E5 d'origine : A est libéré puis redistribué comme B, et son contenu est écrasé ;
  - X3 (pas de `pmm_unref`) : le compteur reste à 1 et A n'est plus jamais libérable.
- **Ordre de `vmm_unmap()`** : l'analyse montre que `pmm_unref()` ne peut pas échouer dans un état valide (hypothèse documentée dans la conception, section 15). Aucune modification nécessaire.
- **Validation :** `make clean && make test` donne 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 (RAM de 16 MiB à 4 GiB, autotests à 128 et 512 MiB, fautes de page, débordement de pile, production) ; compilation parallèle propre réussie.
- **E5 corrigé et démontré.** E3 reste ouvert (M2.5).

### K0.7-M2.5a — Audit des alias inscriptibles (E3), sans changement de code

- **Document :** `docs/design/K0.7-M2.5-writable-aliases.md`. Il contient la carte physique de l'image de production (symboles du linker), les droits réellement installés lus **depuis l'extérieur du noyau** par le moniteur QEMU (`info mem` ; CR0 = `0x80010011`, CR3 = `0x00191000`), l'analyse par catégorie de cadres, les chemins P1-P6 vers `kmain` et les tables de pages, les invariants A1-A6 et la stratégie M2.5b-d.
- **Constats principaux :**
  - `.text` et `.rodata` sont en lecture seule dans l'identité : refuser les nouveaux alias inscriptibles suffit pour eux ;
  - **les tables de pages ont déjà un alias inscriptible : l'identité.** Le répertoire (CR3 = `0x00191000`) est dans la plage `-rw`, donc refuser une projection dynamique ne fermera pas ce chemin (P4). E3 ne pourra être déclaré corrigé que pour P2 et P3 ;
  - le type `FRAME_KERNEL` ne distingue pas code, constantes et données ; la politique proposée (seul `ALLOCATED` est projetable dynamiquement) n'a pas besoin de cette distinction.
- **Constat supplémentaire C1 (non corrigé) :** `map_page()` accepte tous les bits bas de PTE, y compris `PTE_USER` : un correctif dédié est proposé en M2.5c.
- Vérification : `make clean && make test` donne 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26. **E3 reste ouvert.**

### K0.7-M2.5b — Seuls les cadres ALLOCATED sont projetables dynamiquement

- `paging.c` : dans `map_page()` compté (`vmm_map()`), `t == FRAME_FREE` devient `t != FRAME_ALLOCATED` → `SAMOS_EPERM`. `KERNEL`, `PAGETABLE`, `RESERVED` et les adresses non gérées sont désormais refusés, en plus de `FREE`. Le contrôle précède `get_table()` : un refus ne crée aucune table. Le chemin d'identité de `paging_init()` n'est pas restreint ; aucune API de contournement.
- **Test** `test_e3_type_policy` : refus (avec et sans `PTE_WRITE`) pour `kmain`, `.rodata`, GDT, table du PMM, CR3, table de pages active, VGA, cadre 0, cadre libéré, deux adresses non gérées ; puis aucune table créée, `free_count` et références inchangés, cadre `ALLOCATED` toujours projetable, identité de `kmain` intacte et en lecture seule, E1/E2/E4 intacts, `pmm_core_check` à 0. Autotests : 367 → 390 vérifications, 0 échec.
- **Contre-épreuve** (ancienne condition `t == FRAME_FREE`, copie isolée) : statut 35, 12 échecs ciblés, aucun triple fault.
- **Validation :** `make clean && make test` donne 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 ; compilation parallèle propre (`make -j8`) réussie.
- **Limites :** les tables de pages restent inscriptibles par l'identité (P4) ; les bits de PTE ne sont pas filtrés (C1, M2.5c) ; **E3 n'est pas déclaré corrigé avant M2.5d.**

### K0.7-M2.5c — Validation stricte des droits de vmm_map()

- `paging.c` : sur le chemin compté, `(flags & ~PTE_WRITE) != 0` donne `SAMOS_EINVAL`. Seuls `0` (lecture seule) et `PTE_WRITE` sont admis. `PTE_PRESENT` est posé par le VMM seul. `PTE_USER`, PWT, PCD, PAT, A, D, G et les bits 9-11 sont refusés. Le contrôle suit immédiatement celui de l'alignement, donc avant toute table, référence ou PTE. Le chemin d'identité n'est pas modifié. Constat C1 corrigé.
- **Vérification matérielle :** les PDE créés valent `P | W` (sans U/S) ; les droits effectifs sont l'intersection PDE/PTE (SDM vol. 3A §4.6). Le test parcourt tout le répertoire et toutes les tables : aucun bit U/S.
- **Test** `test_vmm_flags_policy` : PTE exacte (`P`, puis `P | W`), comportement en lecture et en écriture, 16 combinaisons refusées sans traduction ni référence, `EINVAL` (et non `EEXIST`) sur une adresse déjà projetée avec PTE intacte, tables, `free_count` et références inchangés, E1, E2, E4 et E5 intacts. Autotests : 390 → 417 vérifications, 0 échec.
- **Contre-épreuve** (`if (0)`, copie isolée) : statut 35, 7 échecs ; les 14 combinaisons de bits bas sont acceptées et prennent chacune une référence ; aucun triple fault.
- **Validation :** `make clean && make test` donne 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 ; compilation parallèle propre (`make -j8`, images de production et de test) réussie.
- **Limites :** l'identité reste `RW`, y compris les tables de pages (P4) ; pas de bit NX en i386 sans PAE ; **E3 n'est pas déclaré corrigé avant M2.5d.**

### K0.7-M2.5d — Validation finale de E3

- **Test** `test_e3_scenario` : rejeu de l'attaque d'origine (`vmm_map(0xD0000000, page de kmain, PTE_WRITE)`), puis des alias de `.rodata`, de CR3, de la table active, de la GDT et de la table du PMM. Chaque cible est tentée avec `PTE_WRITE` et `0`, dans une table existante et dans un répertoire sans table : toujours `EPERM`. Aucune écriture par alias, même sous contre-épreuve. Vérifiés ensuite : code et PTE d'identité intacts, CR0.WP actif, compteurs inchangés, droits interdits refusés sur un cadre `ALLOCATED` (`EINVAL`), cycle normal d'un cadre alloué, E1, E2, E4, E5, aucun bit U/S. Autotests : 417 → 453 vérifications, 0 échec.
- **Contre-épreuves** (copies isolées, statut 35, aucun triple fault) : C1 (politique de type ancienne) donne 21 échecs, dont 9 dans le scénario ; C2 (flags non filtrés) donne 12 échecs, dont 5 ; C3 (les deux) donne 33 échecs, dont 14. Le code de `kmain` reste intact sous C1 et C3.
- **Validation :** `make clean && make test` donne 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 (démarrage de production de 16 MiB à 4 GiB, autotests à 128 et 512 MiB, fautes de page, débordement de pile) ; `make -j8` propre pour les deux images ; autotests manuels à 128 et 512 MiB : 453/0. À 16 MiB, une seule assertion préexistante échoue (`test_memmap` attend plus de 100 MiB), identique au commit `0876cd3`.
- **Verdict :** E3 corrigé pour `vmm_map()` (P2, P3, alias ring 3). **P4 reste ouvert** : les tables de pages restent inscriptibles par l'identité.

### K0.7-M2 — Audit de clôture (PMM, VMM, protections mémoire), sans changement de code

- **Document :** `docs/design/K0.7-M2-closure-audit.md`. Il contient les mécanismes en place, la vérification des 10 invariants dans le code réel (inventaire de **tous** les écrivains de descripteurs, de PTE et de PDE), l'inventaire des tests et contre-épreuves, les risques résiduels classés (exploitable, architectural, futur), les hypothèses et les recommandations.
- **Verdict :** E1, E2, E4 et E5 sont corrigés ; E3 est corrigé pour `vmm_map()`, mais **P4 reste ouvert** (tables de pages inscriptibles par l'identité). Les 10 invariants sont vérifiés ; l'invariant 9 l'est avec une réserve déjà documentée : une table vide subsiste après un `ERANGE`. Aucune vulnérabilité exploitable par les interfaces actuelles.
- **Expériences de l'audit** (copies isolées, non commitées) :
  - 4095 projections d'un même cadre, puis `ERANGE` : aucune PTE ni référence, une table vide créée ;
  - démarrage de production dès 1792 KiB ;
  - en dessous de la fin de l'image (environ 1,57 MiB), le noyau ne prend jamais la main.
- **Constats nouveaux :**
  - N1 : les cibles `make test-mutation` et `make test-repro`, annoncées plus haut (entrée sur les images séparées), **ne fonctionnent pas** : leurs scripts n'ont jamais été commités ;
  - N2 : `test_memmap` suppose plus de 100 MiB, d'où un échec unique des autotests à 16 MiB ;
  - N3 : la table des limites K0.6 de `Details.md` n'a pas encore de pendant K0.7.
- **Validation :** `make clean && make test` donne 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 ; `make -j8` depuis zéro produit des binaires identiques bit à bit à la compilation séquentielle ; autotests à 128 et 512 MiB : 453 vérifications, 0 échec ; à 16 MiB : 1 échec préexistant (N2).

### K0.7-R0 — Réparation de l'infrastructure de tests (N1)

**Objectif.** Rendre fonctionnelles les cibles `make test-mutation` et `make test-repro`, annoncées dans l'entrée « images séparées » ci-dessus mais jamais livrées. **Correction de l'historique :** dans ce document, ces deux cibles sont présentées comme disponibles depuis `9b13325`. C'est inexact : elles échouaient jusqu'à ce jalon (`No such file or directory`).

**Cause.** `9b13325` a ajouté les cibles au Makefile, mais pas les scripts (aucune écriture de ces fichiers dans le journal de la session). Le même commit a **tronqué `README.md` à 0 octet** (N4, découvert en R0) : vraisemblablement une session interrompue pendant l'écriture des fichiers.

**Fichiers.**

| Fichier | Rôle |
|---|---|
| `tests/mutation/run.sh` (nouveau) | moteur de mutation |
| `tests/mutation/cases.sh` (nouveau) | 3 mutations : E5, E3, droits PTE |
| `tests/mutation/selftest.sh` (nouveau) | 11 situations où le moteur doit échouer, plus une contre-épreuve |
| `tests/tools/repro_check.sh` (nouveau) | vérification de reproductibilité |
| `tests/tools/repro_selftest.sh` (nouveau) | 4 situations d'échec, plus une contre-épreuve |
| `Makefile` | commentaires des deux cibles uniquement |
| `README.md` | restauré depuis `main` ; section 13 complétée |

**Moteur de mutation.**
- Il copie les fichiers suivis par Git (contenu de l'arbre de travail) dans un répertoire temporaire, sans `build/` : aucun ancien binaire n'est possible.
- Il compile et exécute d'abord la version **saine**, qui doit donner le statut 33 et 0 échec.
- Pour chaque mutation : le texte original doit apparaître **exactement une fois** ; le fichier et le binaire doivent différer de la version saine ; compilation, autotests sous QEMU, puis verdict.
- **DÉTECTÉE** exige : le statut attendu (35), un bilan `failures=N` avec N > 0, chaque assertion nommée présente en `[FAIL]`, et aucun triple fault ni reset.
- Un échec de compilation, un délai (124), une panique (37) ou un arrêt du CPU donne **INVALIDE**, jamais « détectée ».
- À la fin, le script vérifie que HEAD, la branche et `git status` du dépôt sont inchangés. Le nettoyage est assuré par `trap`, interruption comprise.
- Les motifs sont passés à `awk` par `ENVIRON`. Un premier essai avec `awk -v` a échoué : `-v` interprète les séquences d'échappement, et un `\n` du source devenait un saut de ligne (défaut révélé par l'auto-test).

**Reproductibilité.**
- Deux copies, dans des répertoires de longueurs différentes, et deux compilations propres dont le code de sortie est vérifié.
- Trois verdicts par image :
  - IDENTIQUE : SHA-256 égal ;
  - DÉBOGAGE SEULEMENT : ELF sans débogage (`objcopy --strip-debug`) identique, et toutes les sections différentes sont des `.debug_*` ;
  - DIFFÉRENT : échec.
- Le script nomme les sections différentes, compte les chemins de compilation retrouvés dans l'image et donne le premier octet différent.
- **Limite :** NASM 2.16 écrit le répertoire absolu dans `DW_AT_comp_dir` et n'a pas d'équivalent de `-ffile-prefix-map`. Les ELF complets diffèrent donc entre répertoires (`.debug_info`, `.debug_aranges`), et `REPRO_STRICT=1` échoue pour cette raison.
- Les fichiers `.map`, qui contiennent des chemins, ne sont pas comparés.

**Débogage.**
- L'auto-test d'interruption utilisait d'abord SIGINT. Un shell non interactif lance ses commandes en arrière-plan avec SIGINT ignoré, et un signal ignoré à l'entrée ne peut pas être intercepté. Il utilise donc SIGTERM, traité comme INT et HUP.
- `mawk` (l'`awk` installé) n'a pas `strtonum` : la conversion hexadécimale se fait en bash.
- `KEEP=1` était forcé lorsque la version saine échouait, ce qui laissait un répertoire dans `/tmp`. Il est retiré : les journaux utiles sont affichés à la place.

**Validation.**

| Commande | Résultat |
|---|---|
| `make clean && make test` | code 0, 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 |
| `make test-mutation` | code 0 : version saine 453 vérifications et 0 échec ; `e5_free_referenced`, `e3_type_check` et `flags_filter` DÉTECTÉES (35, 21 et 12 échecs) ; environ 18 s |
| `make test-repro` | code 0 : les deux images DÉBOGAGE SEULEMENT |
| `REPRO_STRICT=1` | code 1 (attendu) |
| `tests/mutation/selftest.sh` | 11 situations conformes |
| `tests/tools/repro_selftest.sh` | 5 situations conformes |
| `make -j8` depuis zéro | réussi |

Aucun répertoire temporaire ne subsiste après les exécutions.

**Limites.**
- Les mutations portent sur l'image de test et les autotests du noyau ; les tests hôtes ne sont pas encore mutés.
- Seuls les fichiers **suivis** sont copiés : un nouveau fichier source non ajouté à Git fait échouer la version saine (code 2).
- Mutations candidates pour la suite : E1/E2 (`va_must_stay_unmapped`), E4 (libération de `PAGETABLE`), `pmm_ref` omis dans `map_page`, `pmm_unref` omis dans `vmm_unmap`, `INVLPG` omis.
- Ni l'une ni l'autre cible n'est incluse dans `make test`.

**Reproduction.**

```bash
make test-mutation
make test-repro
tests/mutation/selftest.sh
tests/tools/repro_selftest.sh
```

### K0.7-R1a — Audit et conception des canaris de pile, sans changement de code

- **Document :** `docs/design/K0.7-R1-stack-canaries.md`. Il contient l'inventaire des piles, les risques K1 à K6, l'étude des deux mécanismes (canaris de fonctions et sentinelles de frontières), la stratégie, les changements nécessaires, les tests T1 à T7 et C1 à C3, les critères d'acceptation et le découpage R1b à R1e.
- **Piles réelles :**
  - S1, pile de démarrage de 16 KiB : utilisée par `kmain` et par **toutes** les interruptions et exceptions, car une porte ring 0 vers ring 0 ne change pas de pile ;
  - S2, pile de double faute de 4 KiB dans `.bss`, via la porte de tâche.
  - Profondeur maximale mesurée de S1 : 836 octets ; plus grand cadre hors tests : 300 octets (`kvprintf`, mesuré par `-fstack-usage` dans une copie isolée).
- **Constats :**
  - S2 n'a aucune garde : son débordement écraserait les TSS, puis la GDT ;
  - le sommet de S1 (`0x191000`) touche le **répertoire de pages** ;
  - un cadre de plus de 4 KiB peut sauter la page de garde, car Clang ignore `-fstack-clash-protection` pour cette cible.
- **Faits vérifiés sur Clang 18 (cible `i686-unknown-none-elf`) :**
  - canari global `__stack_chk_guard` et appel à `__stack_chk_fail` ;
  - `-mstack-protector-guard=tls` et `-mstack-protector-guard-symbol=` sont ignorés sans avertissement ;
  - `no_stack_protector` est respecté ; `stack_protect` est inconnu, d'où une activation progressive **par fichier** ;
  - `-Wframe-larger-than` fonctionne avec `-Werror`.
- **Stratégie retenue :**
  - `-fstack-protector-strong` dans les deux images ;
  - canari défini en NASM dans `.rodata`, écrit par `_start` (à partir de `RDTSC`, octet faible nul) avant `kmain` et avant la pagination, donc en lecture seule ensuite ;
  - `__stack_chk_fail` appelle `panic` ;
  - `-Wframe-larger-than=3072` ;
  - sentinelles en bas de S2.
  - **Décision D1 demandée** au développeur : pages de garde supplémentaires (au-dessus de S1, sous S2), car elles changent la disposition mémoire.
- **État de référence :**
  - `make clean && make test` donne 101 `[PASS]`, 0 `[FAIL]`, 26 cas QEMU sur 26 ;
  - `make test-mutation` donne 3 mutations sur 3 détectées ;
  - `make test-repro` donne 2 images reproductibles.

### K0.7-R1b — Infrastructure minimale des canaris de pile

- **Canari :** `__stack_chk_guard` est défini en NASM dans `.rodata`. `_start` l'écrit à l'étape 4b, après avoir vérifié que CR0.PG vaut 0. La valeur est `RDTSC` mélangé, octet faible nul, jamais nulle. Il devient lecture seule à l'activation de la pagination (CR0.WP).
- **Échec :** `__stack_chk_fail` (`kernel/core/stack_protector.c`) appelle `panic` avec l'adresse de la fonction fautive. `panic_at`, `stack_collect` et `emergency_puts` sont exclus de l'instrumentation (`SAMOS_NO_STACK_PROTECTOR`).
- **Instrumentation minimale :** `-fstack-protector-strong` uniquement pour `kernel/tests/test_canary.c`, dans l'image de test. `-Wframe-larger-than=3072` pour tout le noyau.
- **Nouveaux tests :**
  - suite « K0.7 stack canary » (13 vérifications) ;
  - cas QEMU `canary_smash` (corruption de la seule copie locale, puis panique, statut 37) ;
  - `canary_check.sh` : artefacts ELF (canari dans une page sans segment W, instrumentation de `canary_frame_probe`, aucun appel dans l'image de production) ;
  - `frame_limit_check.sh` : un cadre de 5020 octets refusé, un cadre d'environ 2000 octets accepté ;
  - mutations `canary_not_instrumented` et `canary_not_seeded`.
- **Défauts de test corrigés pendant le jalon :**
  - deux copies du canari trouvées : l'appelant gardait la valeur dans un registre sauvegardé dans le cadre ;
  - cadre de « 5000 octets » accepté : le puits défini dans le même fichier a été intégré, et le tableau réduit à un octet.
  - Détail dans la conception, section 12.
- **Mesures externes** (moniteur QEMU, production) :
  - valeur d'image `0xE3A9C500` ; à l'exécution `0x94786D00`, puis `0x979CDA00` ;
  - page `0x106000` en `----A----` (ni W ni U).
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | code 0, 113 `[PASS]`, 0 `[FAIL]`, 27 cas QEMU sur 27 |
| autotests | 466 vérifications, 0 échec |
| `make test-mutation` | 5 mutations sur 5 détectées |
| `make test-repro` | 2 images reproductibles (débogage seulement) |
| `make -j8` depuis zéro | réussi |

- **Limites :** le noyau n'est pas encore instrumenté (R1c) ; l'entropie vient du TSC seul ; les risques K2 à K6 ne sont pas traités ici (pages de garde : sous-jalon séparé, D1 autorisé dans son principe).

### K0.7-R1c — Activation globale des canaris de fonctions

- **Changement :** `-fstack-protector-strong` dans `CFLAGS` pour les deux images, à la place de `-fno-stack-protector`. L'option par fichier de R1b est retirée. Pas de changement NASM ni du script de liens.
- **Mesures :**
  - production : 11 fonctions instrumentées sur 208, 11 sites d'appel, `.text` 21 153 → 21 569 octets (+416) ;
  - test : 24 sur 238, `.text` +736 ;
  - `.rodata`, `.data` et `.bss` inchangés ;
  - profondeur maximale de S1 au démarrage : 836 → 852 octets.
- **Vérification sur le code machine** (`canary_check.sh`, réécrit) :
  - une fonction est comptée instrumentée si elle lit deux fois le canari et appelle `__stack_chk_fail` ;
  - fonctions requises (`kvprintf`, `kvsnprintf`, `out_number`, `memmap_parse`) ;
  - exclusions (`__stack_chk_fail`, `panic_at`, `stack_collect`, `emergency_puts`) et **toute la fermeture d'appels** du chemin de secours ;
  - cohérence des sites d'appel, dans les deux images.
- **Fonctions à tableau non instrumentées :** recensées via DWARF ; toutes expliquées (exclusion, `_Noreturn`, tableau réduit par l'optimiseur, tableau `static`, fonction intégrée). Détail dans la conception, section 13.4.
- **Contre-épreuves :** `canary_not_instrumented` (autotests) et `canary_global_flag` (nouveau type `mutant_tool` : détection par `canary_check.sh` sur l'image de **production**, diagnostics nommés). Auto-test du moteur : 12 situations conformes, dont `tool_undetected`.
- **Défaut préexistant corrigé :** `selftest_512M` dépassait de façon aléatoire son délai de 10 s ; la durée est de 10 à 11 s aussi avec l'image de R1b. `TIMEOUT=30`, justifié dans `cases.sh`.
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | code 0, 117 `[PASS]`, 0 `[FAIL]`, 27 cas QEMU sur 27 |
| cas QEMU couverts | démarrage de production et de test, série, clavier, exceptions, fautes de page, débordement de pile, double faute, exceptions imbriquées, `canary_smash`, RAM de 16 MiB à 4 GiB |
| autotests | 466 vérifications, 0 échec (E1 à E5, PMM et VMM compris) |
| `make test-mutation` | 6 mutations sur 6 détectées |
| `make test-repro` | 2 images reproductibles (débogage seulement) |
| `make -j8` depuis zéro | réussi |
| `-Wframe-larger-than=3072` et `-fstack-protector-strong` | présents sur les 61 lignes de compilation C (les deux images) |

### K0.7-R1d — Sentinelles de la pile de double faute (S2)

- **S2 :** `g_df_stack[4096]` (`.bss`, alignée sur 16), qui croît vers le bas depuis `g_tss_df.esp` (le sommet). Usage réel du chemin #DF complet, mesuré : 848 octets.
- **Sentinelles :** 4 mots au bas de S2, en production à `0x109110`-`0x10911f`, juste au-dessus de `g_tss_df` (`0x1090a0`-`0x109107`). Valeurs `0xDF5E11A5`, `0xDF5E22A5`, `0xDF5E33A5` et `0xDF5E44A5`, relues par le moniteur QEMU. Posées dans `gdt_init` avant la création de la tâche #DF.
- **Vérification :** `df_stack_sentinel_damage()`, non instrumentée, sans appel ni écriture, ne lit rien d'autre que les 4 mots. Points :
  - entrée de `double_fault_task` (signalement, puis panique dédiée si altérées) ;
  - `panic_at` (ligne de diagnostic seulement, aucune nouvelle panique) ;
  - fin du démarrage dans `kmain` ;
  - autotests.
- **Défaut rencontré et corrigé :** une première version, dont les fonctions étaient définies avant `gdt_init`, déplaçait `g_df_stack` en tête de `.bss`, sous la GDT et les TSS : la disposition changeait sans intention. L'autotest de non-recouvrement l'a révélé. Les fonctions ont été déplacées, et la disposition est identique à R1a. L'ordre de `.bss` n'est pas garanti.
- **Tests :**
  - suite « K0.7 double-fault stack sentinels » (12 vérifications, dont une détection non destructive avec restauration) ;
  - cas QEMU `dfsentinel` (mot 2 altéré, vraie #DF, masque 0x4, statut 37, ni triple fault ni panique récursive) ;
  - `exc_df` et `stack_overflow` exigent maintenant « intact ».
- **Mutations :** `df_sentinel_check_removed` et `df_sentinel_not_initialized` détectées. Une première forme de la seconde (appel supprimé) était refusée par `-Werror` : classée INVALIDE par le moteur, puis reformulée.
- **`canary_check.sh` :** `df_stack_sentinel_damage` ajoutée aux exclusions, et une fonction exclue absente de l'image est désormais un échec (sinon, la vérification serait vide de sens).
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | code 0, 118 `[PASS]`, 0 `[FAIL]`, 28 cas QEMU sur 28 |
| autotests | 478 vérifications, 0 échec |
| `make test-mutation` | 8 mutations sur 8 détectées |
| `make test-repro` | 2 images reproductibles |
| `make -j8` depuis zéro | réussi |

- **Limites :** détection au moment des vérifications seulement ; ni empêchement, ni remplacement d'une page de garde ; un vrai débordement de S2 écraserait les TSS avant d'être constaté.

### K0.7-R1e — Conception des pages de garde de S1 et S2, sans changement dans le dépôt

- **Document :** `docs/design/K0.7-R1e-guard-pages.md`.
- **Cartographie relevée** (`nm`, `readelf`, moniteur QEMU) :
  - le sommet de S1 touche le **répertoire de pages**, dans les deux images (CR3 = `0x191000` en production, `0x19c000` en test) ;
  - S2 partage sa première page avec la GDT et les deux TSS, et sa dernière avec l'IDT ;
  - aucune garde n'est possible sans déplacement.
- **Proposition :**
  - section NOBITS `.stacks` dans `linker.ld`, contenant garde S2, S2 (sortie de `.bss`, définie en NASM `.dfstack`), garde basse de S1 (qui garde aussi le sommet de S2), S1, garde haute de S1 ;
  - nouveaux symboles et assertions de l'éditeur de liens ;
  - S2 remise à zéro explicitement dans `gdt_init`.
- **Prototype jetable** (copie isolée, supprimée ; aucun fichier du dépôt modifié) :
  - disposition liée et démarrée : CR3 = `0x193000`, +8 KiB réservés, fichier +96 octets ;
  - régression : seul l'autotest `boot_stack_top == __kernel_end` échoue, comme prévu ;
  - `df`, `stack`, `dfsentinel` et `canary` passent, sans triple fault ;
  - une fois les gardes retirées de l'identité : trois trous dans `info mem`, sondes en faute à l'adresse exacte, `vmm_map` refusé (`EPERM`) ;
  - **débordement volontaire de S2** : #PF à `CR2 = 0x197ffc` (premier mot sous S2), #DF, puis **triple fault** ; rien n'est écrit sous S2.
- **Défauts de méthode pendant le prototype :** deux exécutions ont d'abord utilisé un binaire périmé, à cause d'erreurs de compilation (déclaration manquante, `-Winfinite-recursion`). Elles ont été écartées, et les mesures refaites en conditionnant l'exécution à la réussite de la compilation.
- **Plan R1f :**
  - R1f-1 : disposition seule, gardes encore projetées ;
  - R1f-2 : garde haute de S1 ;
  - R1f-3 : garde S2 ;
  - R1f-4 : validation finale de R1.
- **Validation de référence** (dépôt inchangé) :
  - `make clean && make test` donne 118 `[PASS]`, 0 `[FAIL]`, 28 cas QEMU sur 28 ;
  - `make test-mutation` donne 8 mutations sur 8 ;
  - `make test-repro` donne 2 images reproductibles.

### K0.7-R1f-1 — Disposition explicite des piles S1 et S2 (gardes encore projetées)

- **Changement :**
  - section NOBITS `.stacks` dans `linker.ld`, de bas en haut : page réservée à la garde S2, S2 (4 KiB, maintenant définie en NASM `.dfstack` et sortie de `.bss`), garde basse de S1 (inchangée, absente), S1 (16 KiB), page réservée à la garde haute ;
  - 17 nouvelles assertions de l'éditeur de liens ;
  - `gdt.c` utilise `df_stack_area` et remet S2 à zéro avant les sentinelles.
  - `paging.c` n'est **pas** modifié : les deux pages réservées restent projetées (R1f-2 et R1f-3).
- **Production, avant puis après :**
  - S2 : `0x109110` (dans `.bss`, sur la page de la GDT et des TSS), puis `0x18c000` ;
  - S1 : `0x18d000`-`0x191000`, puis `0x18e000`-`0x192000` ;
  - pages réservées : `0x18b000` et `0x192000` ;
  - CR3 : `0x191000`, puis `0x193000`.
  - Image de test : `.stacks` de `0x198000` à `0x1a0000`, CR3 `0x1a0000`.
  - Fichier de production +4 octets ; mémoire réservée +8 KiB (`memsz` `0x89000` → `0x8b000`).
- **Vérifications :**
  - suite « K0.7 stack layout » (26 vérifications : bornes, alignements, cohérence des symboles et du TSS, droits effectifs, types PMM, refus de libération et de projection, structures critiques hors des pages réservées, CR3 = `__kernel_end`) ;
  - `check_elf.sh` étendu (chaîne des bornes, tailles, aucun objet dans une page de garde ou réservée) ;
  - mutation `stack_layout_intruder` détectée.
- **Défaut de vérification corrigé :** le premier contrôle des intrus signalait la borne `__df_stack_end` ; le motif d'exclusion est corrigé, et la mutation prouve que le contrôle reste efficace.
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | code 0, 122 `[PASS]`, 0 `[FAIL]`, 28 cas QEMU sur 28 |
| cas QEMU couverts | double faute, débordement, canari, sentinelles, exceptions imbriquées, fautes de page, RAM de 16 MiB à 4 GiB |
| autotests | 508 vérifications |
| `make test-mutation` | 9 mutations sur 9 détectées |
| auto-test du moteur | 12 situations conformes |
| `make test-repro` | 2 images reproductibles |
| `make -j8` depuis zéro | réussi |

- **Risque résiduel :** les pages réservées ne protègent encore rien ; un débordement de S2 atteindrait la fin de `.bss` (table du PMM) sans faute, jusqu'à R1f-3.

### K0.7-R1f-2 — Activation de la garde haute de S1

- **Changement :**
  - `paging.c` : `va_must_stay_unmapped()` couvre `[__stack_top_guard_start, __stack_top_guard_end)` (symboles du linker, aucune adresse en dur) ; la boucle d'identité utilise la même règle ;
  - `vmm_map` refuse cette page (`EPERM`) avant toute recherche de table ;
  - nouveau diagnostic « access above the boot stack top (S1 top guard page) » ;
  - la région `top-guard` devient `---G`.
  - La garde de S2 reste réservée et projetée (R1f-3).
- **Garde :** `0x192000` en production, `0x19f000` en test. Moniteur QEMU : trous à `0x18d000` et `0x192000` (production), à `0x19a000` et `0x19f000` (test) ; CR3 (`0x193000`, `0x1a0000`) et la garde S2 restent `rw`.
- **Tests :**
  - suite « K0.7 S1 top guard » (24 vérifications : PTE nulle, traduction absente, sondes du premier et du dernier mot avec CR2 et code d'erreur exacts, `vmm_map` refusé sans effet de bord, voisins intacts, `vmm_unmap` refusé, garde basse absente, garde S2 présente) ;
  - cas QEMU `topguard` et `topguard_end` (écriture directe, #PF code 0x2, panique identifiable, statut 37, ni double faute ni triple fault ni panique récursive) ;
  - `cr2_matches` compare CR2 à l'adresse lue par `nm` (contre-épreuve : un décalage faux est signalé).
- **Mutation :** `top_guard_mapped` (règle retirée) détectée : 13 échecs, 3 assertions nommées.
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | code 0, 126 `[PASS]`, 0 `[FAIL]`, 32 cas QEMU sur 32 |
| autotests | 532 vérifications |
| `make test-mutation` | 10 mutations sur 10 détectées |
| `make test-repro` | 2 images reproductibles |
| `make -j8` depuis zéro | réussi |
| cas couverts | canaris, sentinelles, double faute, débordement, exceptions imbriquées, RAM de 16 MiB à 4 GiB |

### K0.7-R1f-3 — Activation de la garde basse de S2

- **Changement :**
  - `paging.c` : `va_must_stay_unmapped()` couvre `[__df_guard_start, __df_guard_end)`, donc la page est absente de l'identité et refusée par `vmm_map` ;
  - diagnostic « access to the double-fault stack guard page (below S2) » ;
  - région `df-guard` en `---G`.
  - Disposition et gardes de S1 inchangées.
- **Gardes actives :**
  - production : `0x18b000` (S2), `0x18d000` et `0x192000` (S1) ;
  - test : `0x199000`, `0x19b000` et `0x1a0000`. L'image de test a grandi, d'où un décalage d'une page par rapport au relevé de R1f-1 ; les adresses ne sont jamais codées en dur.
  - Moniteur QEMU : S2 est une page `rw` isolée entre deux gardes ; CR3 `rw`.
- **Tests :**
  - suite « K0.7 S2 guard » (32 vérifications : PTE nulle, sondes depuis S1 avec CR2 et code exacts, `vmm_map` et `vmm_unmap` refusés sans effet de bord, cadre du noyau non libérable, TSS au sommet exact de S2, sentinelles intactes, reste de S2 à zéro, structures critiques lisibles, gardes de S1 absentes) ;
  - cas QEMU `dfguard` et `dfguard_end` (écriture directe depuis S1, #PF code 0x2, panique identifiable, statut 37, sans triple fault) ;
  - `cr2_matches` généralisé à un symbole quelconque.
- **Mutation :** `df_guard_mapped` détectée (12 échecs, 3 assertions nommées) ; `top_guard_mapped` est toujours détectée.
- **Rappel :** un débordement réel de S2 se termine par un triple fault (prototype R1e) ; ce scénario n'est pas dans le banc.
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | code 0, 130 `[PASS]`, 0 `[FAIL]`, 36 cas QEMU sur 36 |
| autotests | 564 vérifications |
| `make test-mutation` | 11 mutations sur 11 détectées |
| `make test-repro` | 2 images reproductibles |
| `make -j8` depuis zéro | réussi |

### K0.7-R1g — Audit final contradictoire des protections de piles et de la mémoire

- **Document :** `docs/review/K0.7-R1g-audit.md`. Il contient les invariants G1-G11 (gardes), C1-C7 (canaris) et S1-S7 (S2), avec leurs preuves et leurs limites, la recherche de contournements X1-X7, les défauts F1-F6, les risques et la décision. **Aucun code du noyau modifié.**
- **Expériences de l'audit** (copies isolées, supprimées) :
  - gardes peintes dans `_start` et vérifiées juste avant la pagination : 0 mot modifié ;
  - profondeur maximale : S1 2 204 octets sur l'ensemble des autotests, S2 848 octets dans les trois scénarios de double faute ;
  - `info tlb` : 32 732 traductions, toutes en identité, aucun alias vers une garde, aucune page `U`, `.text` et `.rodata` sans W ;
  - RAM : démarrage normal dès 1 792 KiB, panique `ENOMEM` déterministe à 1 664-1 728 KiB, aucune sortie à 1 600 KiB.
- **Défauts trouvés :**
  - **F1** (preuve insuffisante) : la remise à zéro de S2 par `gdt_init` n'est pas démontrée ; la mutation `s2_zeroing_removed` n'est **pas détectée**, car le chargeur remplit déjà S2 de zéros ;
  - F2 et F3 (documentation inexacte ou obsolète) : corrigés dans R1 §16, R1e §12.3, le README et `Details.md` §37, avec P4 explicite ;
  - F4 à F6 : lacunes de couverture et outillage.
  - Aucun constat critique ou élevé, aucune défaillance de protection.
- **Validation contradictoire :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | 130 `[PASS]`, 0 `[FAIL]`, 36 cas QEMU sur 36 |
| `make test-mutation` | 11 mutations sur 11 |
| auto-tests des outils | 12 situations (mutation) et 5 (reproductibilité) conformes |
| `make test-repro` | réussi |
| `make -j8` depuis zéro | réussi |
| options de compilation | 61 unités C, toutes avec `-fstack-protector-strong` et `-Wframe-larger-than=3072` |

- **Décision :** protections conformes ; clôture formelle de R1 recommandée **après** un court R1h (F1, puis éventuellement F4 et F6). P4 reste ouvert.

### K0.7-R1h — Correction des lacunes de preuve F1, F4 et F6

- **F1 (remise à zéro de S2) :**
  - la preuve de R1f-3 était **invalide** : le chargeur remplit déjà S2 de zéros ;
  - **nouvelle preuve :** `ktest_s2_prefill()`, appelée par `kmain` sous `#if CONFIG_KTEST` juste avant `gdt_init`, écrit `0x5F2A11ED` dans les 1024 mots de S2 ;
  - suite « K0.7 S2 initialisation proof » (7 vérifications : motif présent avant, sentinelles posées, 1020 mots nuls et sans motif, alignement, TSS au sommet) ;
  - mutation `s2_zeroing_removed` **détectée** (3 échecs nommés), alors qu'elle ne l'était pas en R1g ;
  - production : `ktest_s2_prefill` absent ; code identique à `f5a4446`, à 4 opérandes `__LINE__` près.
- **F4 (panique récursive) :**
  - mode `recursive_panic` (`%s` vers une adresse non projetée pendant la première panique) ;
  - cas QEMU : statut 37 en 0,19 s, « RECURSIVE PANIC » exactement une fois, aucune double faute ni triple fault, environ 712 octets de S1 utilisés ;
  - observation : le texte de la première panique est perdu (formatage en tampon).
- **F6 (filtre d'intrus) :**
  - défaut démontré : `boot_stack_intruder`, dans la garde haute, était accepté ;
  - liste exacte de bornes autorisées, uniquement à l'adresse de début de chaque garde ;
  - contre-épreuves : images normales sans fausse alerte, intrus et alias non listés rejetés ;
  - mutation `stack_layout_prefixed_intruder` détectée.
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | 133 `[PASS]`, 0 `[FAIL]`, 39 cas QEMU sur 39 |
| autotests | 571 vérifications |
| `make test-mutation` | 13 mutations sur 13 |
| auto-test du moteur | 12 situations conformes |
| `make test-repro` | réussi |
| `make -j8` depuis zéro | réussi |
| gardes | absentes dans les deux images (`info mem`) |

- **Recommandation :** R1 peut être clôturé ; clôture en attente de l'autorisation du développeur. P4 reste ouvert.

### K0.7-R1 — Clôture du durcissement des piles (autorisée par le développeur)

- **Document :** `docs/review/K0.7-R1-closure.md`. Il contient les objectifs, les étapes R1a à R1h avec leurs commits, les protections mises en œuvre, la matrice des preuves P1 à P15 (invariant, code, test, contre-épreuve, résultat, limites ; propriétés reposant sur la seule analyse signalées), les défauts corrigés et les risques résiduels acceptés.
- **Validation finale sur arbre propre** (`ddff10d`), sans aucun écart avec R1h :
  - `make test` : 133 `[PASS]`, 0 `[FAIL]`, 39 cas QEMU sur 39, 571 vérifications ;
  - 13 mutations sur 13 ; auto-tests des outils 12 et 5 conformes ;
  - `make test-repro` et `make -j8` réussis ;
  - 11 fonctions instrumentées en production.
- **Statut :** R1 clôturé ; K0.7 en cours ; **P4 non résolu** ; K1 non commencé. Aucun code modifié.

### K0.7-P4a — Audit de conception : protection des structures critiques (sans changement de code)

- **Document :** `docs/design/K0.7-P4a-critical-structures.md`. Il contient la carte des structures critiques (adresse, taille, alignement, propriétaire, droits), la matrice des écritures légitimes (initialisation, exécution, écritures implicites du CPU), les chemins de corruption, la comparaison des stratégies A à D, les dépendances, les risques de régression, la recommandation et le découpage P4b à P4f.
- **Constats :**
  - le répertoire (`0x193000`) et les tables de pages (32 tables contiguës dès `0x194000` à 128 MiB, 256 à 1 GiB) sont `rw` par l'identité ;
  - la page `0x109000` mélange la GDT, les TSS, l'IDT, les pointeurs de gestionnaires, les racines du VMM et les compteurs écrits à chaque interruption ; une séparation préalable est donc nécessaire.
- **Prototype jetable** (copie isolée, supprimée) :
  - **GDT et TSS en lecture seule, puis double faute : triple fault**. Le CPU y écrit lors de la commutation de tâche ; ils doivent rester inscriptibles ;
  - **IDT en lecture seule** : 571 vérifications passent ; seuls les modes de test qui la modifient fautent ;
  - **tables en lecture seule dans l'identité** : démarrage complet, et le bit A est posé par le CPU (accès physique) ; la première écriture logicielle de PTE faute ;
  - toutes les PTE des tables sont dans la première table, ce qui impose un ordre des mises à jour.
- **Recommandation :** séparation (C), scellement (A) de l'IDT, des gestionnaires et des racines du VMM, fonction de passage B1 (désactivation temporaire de CR0.WP, interruptions masquées, validation stricte de la cible) pour les tables de pages. GDT et TSS restent inscriptibles (limite imposée par la porte de tâche). P4 sera **réduit, pas éliminé**.
- **État de référence :** `make test` donne 133 `[PASS]`, 39 cas QEMU sur 39 ; `make test-mutation` 13 sur 13 ; `make test-repro` réussi.

### K0.7-P4b — Séparation physique des structures critiques (droits inchangés)

- **Changement :**
  - `SAMOS_SECTION` ;
  - quatre sections `.bss.p4_*`, rangées par `linker.ld` en tête de `.bss`, une page chacune : `kcpu` (GDT et deux TSS, qui doivent rester RW), `kro_idt` (IDT), `kro_handlers` (`g_handlers`, `g_irq_handlers`), `kro_vmm` (`g_idmap_top`, `g_pd`, écrits une seule fois par `paging_init`) ;
  - 10 assertions du linker ;
  - compteurs, `g_tables` et PMM restent dans `.bss`.
  - **Aucun changement de droits**, aucun alias, CR0.WP inchangé.
- **Production :** `kcpu` `0x109000`, `kro_idt` `0x10a000`, `kro_handlers` `0x10b000`, `kro_vmm` `0x10c000`, compteurs à partir de `0x10d000` ; `.stacks` à `0x18e000` ; CR3 `0x196000`. Coût : +12 KiB réservés ; fichier inchangé ; démarrage à 1 792 KiB conservé.
- **Tests :**
  - `check_elf.sh` : contenu exact de chaque page ;
  - suite « K0.7 critical layout (P4b) » (10 vérifications : géométrie, `sgdt`, `sidt`, `str`, séparation des objets RW, quatre pages encore `rw`, CR3) ;
  - mutations `p4b_rw_in_vmm_page` et `p4b_rw_in_handlers_page` détectées (intrus nommés).
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | 135 `[PASS]`, 0 `[FAIL]`, 39 cas QEMU sur 39 |
| autotests | 581 vérifications |
| `make test-mutation` | 15 mutations sur 15 |
| auto-test du moteur | 12 situations conformes |
| `make test-repro` | réussi |
| `make -j8` depuis zéro | réussi |
| `info tlb` et `info mem` | quatre pages `rw` ; `.text` et `.rodata` `r-` ; trois gardes absentes |

### K0.7-P4c — Passage d'écriture contrôlé : conception et prototype (image de test seulement)

- **Document :** `docs/design/K0.7-P4c-controlled-writes.md`. Il contient l'analyse de CR0.WP (ce que `cli` ne bloque pas : NMI, #MC, exceptions synchrones), la comparaison A, B et C, la décision, le prototype, les contre-épreuves, les risques et les conditions préalables à P4d et P4e.
- **Décision :** fenêtre CR0.WP (A). C'est la seule approche sans alias inscriptible résiduel, sans dépendance circulaire ni coût mémoire. B et C laissent une table inscriptible en permanence.
- **Prototype** (`kernel/tests/test_ptgate.c`, `TEST_SRCS` uniquement) :
  - `ptgate_write_entry` : validation complète **avant** ouverture, IRQ masquées, WP = 0, un stockage, WP rétabli avant IF, relecture ;
  - `ptgate_set_pte` : politique des pages interdites et `INVLPG`.
  - **Le VMM ne l'utilise pas** ; les tables ne sont pas scellées.
- **Isolement :** symboles du prototype interdits en production ; nouvelle règle de `check_elf.sh` : **une seule écriture de CR0 en production**, dans `paging_init` ; la même règle appliquée à l'image de test signale le prototype.
- **Tests :**
  - suite P4c (40 vérifications) : cibles `KERNEL`, `ALLOCATED`, `RESERVED`, hors limites, non alignées, `PTE_USER`, gardes et page nulle refusées ; imbrication `EBUSY` ; erreur après validation, avec restauration ;
  - écriture sur une table scellée pour le test : valeur relue, TLB effectif, retrait avec IF = 1 conservé, `INVLPG` effectif ;
  - après fermeture, une écriture directe faute.
- **Défauts de test corrigés :**
  - comparaison qui ignorait les bits A et D posés par le CPU ;
  - scénario non conditionné à ses préconditions, ce qui rendait INVALIDE la mutation E5 (lecture de `0x4`).
- **Mutations :** `ptgate_type_check_removed`, `ptgate_wp_not_restored` et `ptgate_nesting_allowed` détectées.
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | 136 `[PASS]` (règle CR0 comprise), 0 `[FAIL]`, 39 cas QEMU sur 39 |
| autotests | 621 vérifications |
| `make test-mutation` | 18 mutations sur 18 |
| `make test-repro` | réussi |
| `make -j8` depuis zéro | réussi |
| faible RAM, droits (`info mem`) | inchangés |

- **Risques non résolus :** NMI et #MC pendant la fenêtre (WP = 0, puis panique) ; validation dépendante de l'intégrité du PMM ; P5.

### K0.7-P4d — Scellement des structures critiques immuables

- **Document :** `docs/design/K0.7-P4d-sealed-structures.md`.
- **Audit des écritures :** aucune adresse n'échappe en écriture vers les trois pages ; en production, la dernière écriture est `irq_register(1)` dans `keyboard_init`. Le CPU n'écrit jamais l'IDT. En test : `test_cpu`, et les modes `df`, `dfsentinel` et `nested`.
- **Mécanisme :**
  - `paging_seal_identity` : à sens unique, toutes les préconditions vérifiées avant modification, pages `FRAME_KERNEL` en identité seulement, aucune référence PMM touchée ;
  - `paging_seal_critical` : trois pages ; appelée dans `kmain` après `keyboard_init`.
  - **Aucune fenêtre CR0.WP en production** (`check_elf.sh` : une seule écriture de CR0, dans `paging_init`).
- **Image de test :** suites, puis scellement, puis suite « K0.7 sealed structures (P4d) » (31 vérifications). Modes destructifs scellés, sauf `df`, `dfsentinel` et `nested`.
- **Carte :** production `0x10a000-0x10d000 r-` (IDT, gestionnaires, racines du VMM) ; `kcpu` `0x109000`, `.bss`, CR3 `0x196000` et les tables restent `rw` ; CR0 = `0x80010011`.
- **Preuves :**
  - écriture refusée (CR2 exact, code 3) ; une seule projection par cadre, sans W ;
  - IRQ0 livrées, #PF traitée, VMM fonctionnel en état scellé ;
  - cas QEMU `sealidt`, `sealhandlers` et `sealvmm` (#PF code 0x3, diagnostic dédié, statut 37, CR2 lu par `nm`) ;
  - `exc_ud`, `stack_overflow`, `canary_smash` et `keyboard_e2e` exécutés en état scellé.
- **Mutations :** `seal_idt_removed`, `seal_handlers_removed` et `seal_vmm_removed` détectées. Les sondes restaurent la valeur si elles réussissent sous mutation.
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | 142 `[PASS]`, 0 `[FAIL]`, 45 cas QEMU sur 45 |
| autotests | 652 vérifications |
| `make test-mutation` | 21 mutations sur 21 |
| `make test-repro` | réussi |
| `make -j8` depuis zéro | réussi |
| faible RAM | inchangée |

- **Risque principal restant :** les tables de pages, inscriptibles, permettent de remettre W sur une page scellée (P4e).

### K0.7-P4e-1 — Passage d'écriture des PDE/PTE intégré au VMM (tables encore inscriptibles)

- **Document :** `docs/design/K0.7-P4e1-vmm-write-gate.md`. Il contient l'audit des chemins, les chemins migrés et non migrés, les invariants, les risques de la fenêtre WP et les préconditions de P4e-2.
- **Décision du développeur :** fenêtre CR0.WP de P4c autorisée en production, **uniquement** dans un passage dédié aux PDE/PTE. Aucune API générale de désactivation de WP ; tables **non** scellées (P4e-2 non autorisé).
- **Mécanisme** (`paging.c`) :
  - `pt_gate_write(kind, va, value)`, `SAMOS_NOINLINE`. L'entrée est calculée à partir de `(kind, va)` et du répertoire actif, jamais passée en adresse.
  - Validation complète avant ouverture : réentrance `EBUSY` ; alignement ; répertoire `PAGETABLE` et égal à CR3 ; jamais U ; PDE exactement P | W vers une table `PAGETABLE` distincte du répertoire ; PTE avec drapeaux parmi P, W, A, D ; page nulle et gardes interdites ; identité exacte, jamais effacée, jamais W sur `.text`, `.rodata` ou une page P4d ; hors identité, cadre `ALLOCATED`.
  - Fenêtre : `irq_save`, WP = 0, **un** stockage 32 bits, WP rétabli **avant** `irq_restore`. Aucun appel dans la fenêtre. Refus si WP est déjà absent à l'entrée.
  - Avec PG = 0 (`paging_init`) : écriture directe, sans toucher à CR0.
- **Chemins migrés :**
  - `get_table` : PDE, `KASSERT` sur refus ;
  - `map_page` : identité au démarrage et `vmm_map` ; référence PMM rendue sur refus ;
  - `vmm_unmap` : refus avant `*pa`, `INVLPG` et `pmm_unref` ;
  - seconde passe de `paging_seal_identity` : erreur renvoyée au lieu d'un `KASSERT`, `kmain` panique toujours.
- **Chemins non migrés :**
  - remise à zéro d'une nouvelle table avant sa liaison (`pmm_alloc_pagetable`) ;
  - prototype P4c (test seulement) ;
  - écritures A/D du MMU.
- **Outils :**
  - `check_elf.sh --production` : CR0 écrit **seulement** par `paging_init` (1 écriture, après `or $0x80010000`) et par `pt_gate_write` (2 écritures : `and $0xfffeffff` sur le même registre, une seule instruction `mov %eX,(%eY)` entre les deux, `cli` avant, pas de `sti` entre). Toute autre fonction est un échec.
  - `paging_gate_test_write` et `paging_gate_test_set_active` ajoutés à `TEST_ONLY_SYMS`.
- **Tests :**
  - nouvelle suite « K0.7 P4e-1 VMM write gate » (`kernel/tests/test_vmmgate.c`, 39 vérifications) : nombre de fenêtres par chemin, nouvelle table, A/D, 14 refus nommés, réentrance, IF conservé, pages P4d et `.text` intactes ;
  - invariant global dans `ktest_check` : CR0.WP = 1 hors fenêtre, vérifié à chaque assertion, rétabli après signalement (image de test seulement) ;
  - le scellement de l'image de test est une assertion (`seal_err == SAMOS_OK`), plus une sortie immédiate.
- **Défauts de test corrigés pendant le jalon :**
  - **M1 (E5) :** faute de page dans la nouvelle suite (écriture sur une projection échouée). Accès désormais conditionnés.
  - **M17 et M25 :** WP laissé à 0, puis `KASSERT` du passage refusé, ce qui donnait une panique (statut 37). C'est maintenant l'invariant global nommé.
  - **M2 (E3) :** masqué fonctionnellement par la revalidation du passage (défense en profondeur). Il est recalé sur les assertions « aucun effet de bord », qui observent les tables créées à tort.
  - **M23 :** sa première version rendait `va_read_only_identity` inutilisée, donc la compilation échouait (INVALIDE). Le remplacement conserve l'appel.
  - **`selftest.sh`, situation `panic` :** texte cible devenu ambigu (deux `KASSERT(e == SAMOS_OK);`). La cible est maintenant le `KASSERT` unique de `vmm_unmap`.
- **Mutations nouvelles :** M22 `gate_user_bit_allowed`, M23 `gate_ro_identity_writable`, M24 `gate_nesting_allowed`, M25 `gate_wp_not_restored`, M26 `gate_window_not_unitary` (`mutant_tool`).
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | 142 `[PASS]`, 0 `[FAIL]`, 45 cas QEMU sur 45 |
| autotests | 16 suites, 692 vérifications, 0 échec |
| `make test-mutation` | 26 mutations sur 26 |
| `tests/mutation/selftest.sh` | 12 situations conformes |
| `make test-repro` | réussi |
| `make -j8` depuis zéro | réussi |
| 1 728 KiB / 1 792 KiB | `ENOMEM` (panique d'initialisation) / démarrage scellé |
| `info registers`, `info tlb`, `info mem` (deux images) | CR0 = `0x80010011` ; `.text`/`.rodata` et pages P4d `r-` ; trois gardes absentes ; répertoire et tables toujours `W` |

- **Risques restants :**
  - NMI et #MC pendant la fenêtre (exécutés avec WP = 0) ;
  - validation dépendante de l'intégrité du PMM ;
  - tables de pages encore inscriptibles (P4e-2) ;
  - code ring 0 arbitraire (P5).

### K0.7-P4e-2 — Scellement du répertoire de pages et des tables actives

- **Document :** `docs/design/K0.7-P4e2-page-table-sealing.md`. Il contient l'audit, l'ordre de scellement, les cycles de vie, les preuves, les mutations, la carte avant et après, les limites et les préconditions de P4f.
- **Audit :**
  - un seul alias par cadre de pagination (l'identité) ;
  - aucune table non publiée ;
  - A/D écrits par le MMU par adresse physique (non soumis aux droits de l'alias) ;
  - **un cadre `PAGETABLE` n'est jamais libéré ni réutilisé** (`pmm_core_free` et `pmm_core_reserve_as` le refusent).
- **Mécanisme** (`paging.c`) :
  - `seal_table_frame(t)` (`SAMOS_NOINLINE`) : validation, W retiré de l'alias d'identité par `pt_gate_write`, puis `INVLPG`.
  - `seal_paging_structures` est appelé par `paging_init` juste après PG | WP. Ordre : tables, puis T0 (la table de l'alias du répertoire), puis le répertoire. Toute erreur fait échouer `paging_init`, et `kmain` panique.
  - `get_table` devient une lecture seule ; la création passe par `table_create` : allocation `PAGETABLE`, remise à zéro, **scellement avant publication**, PDE par le passage.
  - Le passage refuse W sur l'identité d'un cadre `PAGETABLE` : scellement à sens unique.
  - `paging_audit` (production et test) : `kmain` panique sur toute table inscriptible, tout alias, tout PDE non conforme, toute table non publiée, puis affiche `page directory and N page tables sealed read-only (0 writable, 0 aliases)`.
  - `paging_classify` : nouveau diagnostic « write to a sealed page table or page directory ».
- **Outils :**
  - `tests/tools/pt_write_lint.sh` (nouveau, dans `make test-tools`) : aucune écriture directe de PDE/PTE dans les sources hors du passage ;
  - `check_elf.sh` : règle CR0 étendue à l'image de test (prototype P4c, invariant WP de `ktest`) ; `seal_table_frame` doit contenir un `invlpg` après l'appel du passage (deux images).
- **Tests :**
  - nouvelle suite « K0.7 P4e-2 page-table sealing » (`kernel/tests/test_ptseal.c`, 35 vérifications) : état scellé, écritures directes en faute, sens unique, cycle de vie complet d'une table créée après le scellement, A/D matériels, audit ;
  - modes QEMU `test=sealpd`, `test=sealpt`, `test=sealnewpt` (#PF code 3, CR2 égal à l'adresse affichée) ;
  - nouveaux cas de RAM : 1 728 KiB, 1 792 KiB, 2 MiB et 1 GiB.
- **Tests existants adaptés :**
  - `test_canary` et `test_sealed` : le répertoire et la première table sont désormais RO ;
  - `test_vmmgate` : une table neuve coûte 5 fenêtres ;
  - `test_ptgate` : X est déjà scellée par le VMM.
- **Constats et corrections pendant le jalon :**
  - **INVLPG :** QEMU vide tout le TLB à chaque changement de CR0.WP. La mutation « INVLPG oublié » n'était donc pas détectée à l'exécution ; elle est désormais vérifiée sur le code machine.
  - **M25 (WP non rétabli) :** le noyau s'arrête maintenant dans `paging_init`, avant les autotests (refus fermé). La détection passe par `check_elf.sh` (`pt_gate_write:1`).
  - **M1 :** lecture d'un cadre nul dans la nouvelle suite ; l'accès est désormais conditionné.
- **Mutations :** 33 sur 33. Nouvelles : `seal_pd_removed`, `seal_tables_removed`, `seal_t0_removed`, `seal_new_table_removed`, `seal_invlpg_removed`, `gate_pagetable_w_allowed`, `pt_direct_write`.
- **Validation :**

| Commande | Résultat |
|---|---|
| `make clean && make test` | 156 `[PASS]`, 0 `[FAIL]`, 55 cas QEMU sur 55 |
| autotests | 17 suites, 727 vérifications, 0 échec |
| `make test-mutation` | 33 mutations sur 33 |
| `tests/mutation/selftest.sh` | 12 situations conformes |
| `make test-repro` | réussi |
| `make -j8` depuis zéro | réussi |
| RAM | 1 728 KiB `ENOMEM` ; 1 792 KiB, 2 MiB, 16 MiB, 128 MiB, 512 MiB et 1 GiB scellés, « 0 writable, 0 aliases » |
| `info mem` (production, 128 MiB) | répertoire et 32 tables `0x197000-0x1b8000 r-` (en P4e-1 : `rw`) ; pages P4d `r-` ; gardes absentes ; CR0 = `0x80010011` |

- **Risques restants :**
  - NMI et #MC dans la fenêtre WP ;
  - table du PMM et GDT/TSS inscriptibles ;
  - INVLPG non observable sous QEMU (matériel réel : **NOT TESTED**) ;
  - analyse statique heuristique ;
  - monoprocesseur ;
  - P5.
