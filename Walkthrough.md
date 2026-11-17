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
