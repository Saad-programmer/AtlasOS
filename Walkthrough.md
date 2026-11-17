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
make clean && make test        # 23 vérifications [PASS] attendues, aucune [FAIL]
make run                       # affiche les 4 lignes de la section F ; Ctrl+C pour quitter
```
