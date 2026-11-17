# SAMOS / AtlasOS — Details

**Document technique et académique.** Il explique les fondements théoriques de chaque mécanisme implémenté dans SAMOS, les raisons des choix, les alternatives, les limites et les implications de sécurité. Il est cumulatif : chaque jalon y ajoute ses chapitres.

Le texte distingue systématiquement :

- **[ARCH]** : ce qu'impose l'architecture ou une spécification (Intel SDM, Multiboot, ELF, ABI) ;
- **[CHOIX]** : une décision d'implémentation propre à SAMOS, discutable et révisable.

---

## Sommaire

1. Démarrage d'un PC x86 : du reset au noyau
2. Le protocole Multiboot 1
3. Chaîne de compilation freestanding
4. Le format ELF : sections et segments
5. Registres et modes du processeur i386
6. La pile, la convention d'appel cdecl et l'alignement
7. De l'assembleur au C : préparer l'environnement d'exécution
8. Arrêter le processeur en sécurité
9. Entrées/sorties : ports d'E/S et canal de diagnostic QEMU
10. Sécurité du jalon K0.1
11. Stratégie de vérification
12. Questions de jury et réponses (K0.1)
13. Le script de liaison en profondeur (K0.2)
14. Disposition mémoire du noyau et modèle de permissions (K0.2)
15. Pile de démarrage : zone de garde et peinture (K0.2)
16. Sécurité du jalon K0.2
17. Questions de jury et réponses (K0.2)
18. Affichage et diagnostic : VGA, UART 16550, journal, panique (K0.3)
19. Formatage de texte et arithmétique 64 bits sans libgcc (K0.3)
20. Sécurité du jalon K0.3
21. Questions de jury et réponses (K0.3)
- Références

---

## 1. Démarrage d'un PC x86 : du reset au noyau

### 1.1 Le vecteur de reset

**[ARCH]** Après une mise sous tension ou un reset, un processeur x86 démarre en **mode réel**, avec `CS.selector = 0xF000`, une base cachée de CS valant `0xFFFF0000` et `EIP = 0xFFF0`. La première instruction est donc lue à l'adresse physique `0xFFFFFFF0`, 16 octets sous les 4 Gio (Intel SDM vol. 3A, §9.1.4 « First Instruction Executed »). Le chipset y projette la mémoire flash du firmware.

On peut le constater dans nos journaux QEMU (`-d cpu_reset`) : le second reset enregistré montre `EIP=0000fff0`.

### 1.2 Firmware : BIOS et UEFI

| | BIOS « legacy » | UEFI |
|---|---|---|
| Mode d'exécution | mode réel 16 bits | mode protégé ou long (32/64 bits) |
| Services | interruptions logicielles (`int 0x10`, `int 0x15`…) | tables de services (Boot Services, Runtime Services) |
| Démarrage | lit le secteur 0 (MBR, 512 octets, signature `0x55AA`) à `0x7C00` | lit une application PE/COFF sur une partition FAT (ESP) |
| Carte mémoire | `int 0x15, eax=0xE820` | `GetMemoryMap()` |
| Affichage texte | mode VGA texte à `0xB8000` | framebuffer graphique (GOP), sans mode texte garanti |

Le BIOS effectue le POST (Power-On Self-Test), initialise le matériel minimal, puis passe la main à un **chargeur d'amorçage**. QEMU utilise par défaut SeaBIOS, un BIOS libre.

### 1.3 Rôle du chargeur d'amorçage

Écrire un chargeur qui tient dans 512 octets, active la ligne A20, passe en mode protégé, lit un système de fichiers et charge un ELF est un projet en soi. **[CHOIX]** SAMOS délègue ce travail à un chargeur conforme **Multiboot** (GRUB, ou le chargeur intégré de QEMU via `-kernel`). C'est autorisé : un protocole de démarrage documenté n'est pas un raccourci d'exécution, et le noyau reste entièrement indépendant.

Alternative écartée pour K0 : écrire son propre chargeur BIOS en 16 bits. C'est très formateur, mais orthogonal à l'objectif K0, et rendu obsolète par UEFI sur le matériel récent.

### 1.4 La ligne A20

Sur le 8086, les adresses dépassant 1 Mio « bouclaient » vers 0. Par compatibilité, l'IBM AT a ajouté une porte logique qui force à 0 le bit d'adresse 20. Tant qu'elle est fermée, l'adresse `0x100000` (1 Mio) est un alias de `0x0`. **[ARCH]** Multiboot garantit que A20 est activée (§3.2). C'est indispensable puisque notre noyau est chargé à 1 Mio.

---

## 2. Le protocole Multiboot 1

### 2.1 Pourquoi un protocole standard ?

Un protocole de démarrage est un **contrat** entre chargeur et noyau. Il précise comment le noyau se déclare, où il est chargé, dans quel état se trouve le CPU au moment du transfert, et comment les informations sur la machine lui sont transmises. Multiboot (spécification 0.6.96) a été conçu pour que n'importe quel chargeur conforme puisse démarrer n'importe quel noyau conforme.

### 2.2 L'en-tête Multiboot

**[ARCH]** (spéc. §3.1) L'en-tête doit se trouver **entièrement dans les 8192 premiers octets** du fichier, **aligné sur 4 octets** (32 bits). Il commence par trois mots de 32 bits :

| Offset | Champ | Valeur SAMOS |
|---|---|---|
| 0 | `magic` | `0x1BADB002` |
| 4 | `flags` | `0x00000003` |
| 8 | `checksum` | `0xE4524FFB` |

Drapeaux demandés :

- **bit 0** : les modules éventuels sont chargés alignés sur 4 Kio ;
- **bit 1** : le chargeur doit fournir `mem_lower/mem_upper` et, si possible, la carte mémoire. Elle sera indispensable en K0.6.

Le **bit 16** (« a.out kludge ») n'est pas positionné : le chargeur lit alors les *program headers* ELF pour savoir où charger le noyau.

**Calcul de la somme de contrôle.** La spécification exige :

$$\text{magic} + \text{flags} + \text{checksum} \equiv 0 \pmod{2^{32}}$$

donc :

$$\text{checksum} = 2^{32} - (\text{magic} + \text{flags}) = 2^{32} - \texttt{0x1BADB005} = \texttt{0xE4524FFB}$$

En NASM, `-(MB_HEADER_MAGIC + MB_HEADER_FLAGS)` produit exactement ce résultat : en complément à deux sur 32 bits, $-x \equiv 2^{32} - x$. Le script `tools/check_elf.sh` recalcule cette somme sur le fichier produit.

**Pourquoi l'en-tête est-il à l'offset 4096 ?** Le premier segment `PT_LOAD` commence à l'offset 0x1000 du fichier, ce qui découle de `-z max-page-size=0x1000` et de l'alignement des segments. L'en-tête est le premier contenu de ce segment, grâce à `KEEP(*(.multiboot))` placé en tête de `.text`. On a bien 4096 < 8192. Avec une taille de page maximale de 64 Kio, réglage par défaut sur certaines architectures, l'en-tête serait passé au-delà de 8192 et le noyau n'aurait pas été reconnu. C'est pourquoi l'option est explicite et vérifiée.

### 2.3 État de la machine au transfert

**[ARCH]** (spéc. §3.2) Au moment où le chargeur saute au point d'entrée ELF :

| Élément | Garantie |
|---|---|
| EAX | `0x2BADB002` : preuve qu'un chargeur Multiboot a démarré le noyau |
| EBX | adresse **physique** de la structure `multiboot_info` |
| CS | segment de code 32 bits, lecture/exécution, base 0, limite `0xFFFFFFFF` |
| DS, ES, FS, GS, SS | segment de données 32 bits, lecture/écriture, base 0, limite `0xFFFFFFFF` |
| A20 | activée |
| CR0 | PE = 1 (mode protégé), PG = 0 (pas de pagination) |
| EFLAGS | VM = 0, IF = 0 |
| **ESP** | **indéfini** : le noyau doit créer sa pile immédiatement |
| **GDTR** | **peut être invalide** : les sélecteurs fonctionnent grâce aux caches de descripteurs, mais tout rechargement d'un registre de segment est interdit avant d'avoir installé sa propre GDT (K0.4) |
| **IDTR** | indéfini : aucune interruption ni exception ne doit survenir |
| Autres bits d'EFLAGS | indéfinis, notamment **DF** |

Ces garanties dictent chaque instruction de `_start` (§7).

### 2.4 La structure `multiboot_info`

Le champ `flags` indique quels champs sont valides. Exemple observé sous QEMU : `flags = 0x24F`, soit les bits 0 (mem), 1 (boot_device), 2 (cmdline), 3 (mods), 6 (mmap) et 9 (nom du chargeur). Les offsets sont fixés par la spécification. `kernel/include/samos/multiboot.h` les vérifie à la compilation avec `_Static_assert` : si le compilateur insérait un octet de remplissage, la compilation échouerait au lieu de produire un noyau qui lit des champs décalés.

L'élément de carte mémoire a un piège : son champ `size` **n'inclut pas** les 4 octets du champ `size` lui-même, et un chargeur peut fournir des éléments plus grands que 20 octets. L'élément suivant est donc à `adresse + size + 4`. Ce point sera traité en K0.6.

### 2.5 Multiboot 1, Multiboot 2 et protocoles modernes

| | Multiboot 1 | Multiboot 2 | Limine / UEFI direct |
|---|---|---|---|
| Mode du noyau | 32 bits protégé | 32 bits protégé (ou EFI) | 64 bits possible |
| QEMU `-kernel` | ✅ | ❌ (GRUB et image ISO nécessaires) | ❌ |
| Informations | structure fixe | liste de *tags* extensible | protocole propre |
| Framebuffer UEFI | limité | oui | oui |

**[CHOIX]** Multiboot 1 a été retenu pour K0 : c'est le chemin le plus court vers un noyau testable automatiquement. La migration x86-64 imposera de revoir ce choix (Multiboot 2 avec un trampoline 32→64 bits, ou un autre protocole). La question est reportée à la revue de fin de K0.

---

## 3. Chaîne de compilation freestanding

### 3.1 Hébergé ou autonome (*hosted / freestanding*)

La norme C (ISO/IEC 9899:2011, §4) définit deux environnements :

- **hébergé** : la bibliothèque standard complète existe (`printf`, `malloc`, fichiers…), et le programme commence par `main` ;
- **autonome** (*freestanding*) : seuls certains en-têtes sont exigés (`<float.h>`, `<iso646.h>`, `<limits.h>`, `<stdalign.h>`, `<stdarg.h>`, `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`, `<stdnoreturn.h>`), et le point d'entrée est défini par l'implémentation.

Un noyau est par nature autonome : il n'y a sous lui aucun système pour fournir `malloc` ou `printf`. **[CHOIX]** SAMOS s'autorise `stdint.h`, `stddef.h`, `stdbool.h` et `stdarg.h`. Ils ne contiennent que des définitions de types et de macros fournies par le compilateur, sans aucun code exécutable.

### 3.2 Pourquoi pas le GCC de l'hôte avec `-m32` ?

Un compilateur hôte est configuré pour Linux. Il peut par défaut :

- produire du code PIE (position-independent) ;
- insérer des protections de pile (`__stack_chk_fail`, une fonction de la libc) ;
- insérer des instructions CET (`endbr32`) ;
- chercher les en-têtes dans `/usr/include` (ceux de la glibc, faux pour un noyau).

**[CHOIX]** Clang est nativement un compilateur croisé : `--target=i686-unknown-none-elf` sélectionne une cible sans système d'exploitation (`none`) et produit de l'ELF 32 bits, sans aucune installation supplémentaire.

### 3.3 Signification des options

| Option | Effet | Pourquoi |
|---|---|---|
| `--target=i686-unknown-none-elf` | génère pour i686, sans OS, en ELF | pas d'hypothèse « Linux » |
| `-march=i686` | jeu d'instructions Pentium Pro (CMOV…) | supporté par le CPU `qemu32` |
| `-ffreestanding` | environnement autonome ; implique `-fno-builtin` | le compilateur ne suppose pas l'existence de la libc |
| `-nostdlibinc` | ignore `/usr/include` mais garde les en-têtes internes de Clang | `stdint.h` vient de Clang, pas de la glibc |
| `-fno-pic -fno-pie` | code à adresse absolue | le noyau est lié à 1 Mio, il n'y a pas de chargeur dynamique |
| `-fno-stack-protector` | pas de canari de pile | il faudrait fournir `__stack_chk_guard/fail` ; à reconsidérer plus tard |
| `-fcf-protection=none` | pas d'`endbr32` | inutile sans CET ; simplifie le désassemblage |
| `-mgeneral-regs-only` | interdit x87, MMX et SSE | l'état FPU/SSE n'est ni initialisé ni sauvegardé lors des interruptions ; une instruction SSE produirait #UD ou corromprait l'état d'un futur processus |
| `-mstack-alignment=16` | maintient la pile alignée sur 16 | voir §3.4 |
| `-fno-omit-frame-pointer` | conserve la chaîne EBP | permettra les traces de pile dans `panic()` (K0.3/K0.4) |
| `-fno-asynchronous-unwind-tables` | pas de `.eh_frame` | pas d'exceptions C++ ni de déroulement de pile |
| `-fno-common` | les variables globales non initialisées vont dans `.bss` | des définitions multiples deviennent une erreur de liaison |
| `-O2 -g` | optimisation et informations de débogage | le noyau testé est le noyau optimisé ; GDB reste utilisable |
| `-Wall -Wextra -Werror …` | avertissements traités comme erreurs | qualité, et détection précoce du comportement indéfini |

Côté NASM, `-f elf32 -g -F dwarf -w+all -w+error`. Côté ld :

- `-m elf_i386 -T linker.ld -nostdlib` ;
- `-z max-page-size=0x1000` (voir §2.2) ;
- `--build-id=none` : sinon une section `.note.gnu.build-id` pourrait précéder l'en-tête Multiboot ;
- `--fatal-warnings` : ld 2.39+ avertit en cas de segment RWX, ce qui devient alors une erreur.

### 3.4 Constat expérimental : l'alignement de pile par défaut de Clang

L'ABI i386 System V d'origine (4e éd.) n'exigeait qu'un alignement de 4 octets. Linux et GCC l'ont ensuite porté à **16 octets** au point d'appel, pour que les variables SSE (`movaps`) puissent être alignées sur la pile. Dans LLVM, la valeur par défaut dépend de l'OS cible : 16 pour Linux et Darwin, mais **4 pour une cible sans OS** comme `i686-unknown-none-elf`.

Expérience réalisée (voir `Walkthrough.md`, étape 0) :

```text
sans option              : sub $0x40,%esp ; push %eax ; call h   -> ESP non multiple de 16
-mstack-alignment=16     : sub $0x48,%esp ; mov %eax,(%esp) ; call h  -> ESP multiple de 16
```

**[CHOIX]** SAMOS adopte la règle des 16 octets partout. Ce n'est pas indispensable sans SSE, mais c'est cohérent avec l'ABI x86-64 (qui l'impose), sans ambiguïté pour le code assembleur qui appelle du C, et cela prépare l'arrivée du calcul flottant en espace utilisateur.

### 3.5 libgcc et les opérations 64 bits

Sur i386, une division `uint64_t / uint64_t` n'a pas d'instruction native : le compilateur appelle `__udivdi3`, normalement fournie par libgcc ou compiler-rt. **[CHOIX]** SAMOS ne lie aucune bibliothèque. Toute dépendance de ce type apparaîtrait comme symbole indéfini, ce que `check_elf.sh` vérifie avec `nm -u`. Les divisions 32 bits, comme dans `qemu_dbg_put_dec32`, utilisent l'instruction `DIV` native. Si une division 64 bits devient nécessaire, la routine sera écrite et documentée dans SAMOS.

### 3.6 Le piège de `memset` récursif

Un optimiseur peut reconnaître une boucle qui remplit de la mémoire et la remplacer par un appel à `memset`. Dans l'implémentation de `memset` elle-même, cela produirait une **récursion infinie**. Avec Clang, `-ffreestanding` implique `-fno-builtin`, ce qui pose l'attribut `no-builtins` et désactive cette transformation. Ce n'est pas présumé, c'est **vérifié** : `check_elf.sh` contrôle par désassemblage que `memset`, `memcpy` et `memmove` ne contiennent aucun `call` vers elles-mêmes.

---

## 4. Le format ELF : sections et segments

### 4.1 Deux vues d'un même fichier

ELF (*Executable and Linkable Format*, spéc. TIS 1.2) offre deux vues :

- la **vue de liaison** (*sections*, décrites par la *section header table*) : `.text` (code), `.rodata` (constantes), `.data` (variables initialisées), `.bss` (variables initialisées à zéro), `.debug_*`… Elle est utilisée par l'éditeur de liens et le débogueur ;
- la **vue d'exécution** (*segments*, décrits par les *program headers*) : chaque `PT_LOAD` indique au chargeur de copier `p_filesz` octets depuis l'offset `p_offset` vers l'adresse `p_paddr`/`p_vaddr`, puis de **compléter de zéros jusqu'à `p_memsz`**. Les permissions sont données par `p_flags` (R, W, X).

Un segment regroupe une ou plusieurs sections. Le chargeur Multiboot ne lit que les segments.

### 4.2 La disposition de SAMOS

```text
Adresse physique
0x00100000 ┌──────────────────────┐  __kernel_start, __text_start
           │ .multiboot (12 o)    │  segment "text"   R-X
           │ .text                │
0x00102000 ├──────────────────────┤  __rodata_start
           │ .rodata              │  segment "rodata" R--
0x00103000 ├──────────────────────┤  __data_start
           │ .data                │  segment "data"   RW-
0x00104000 ├──────────────────────┤  __bss_start
           │ .bss                 │  (NOBITS : pas d'octets dans le fichier)
           │   boot_stack (16 Kio)│  0x104000 – 0x108000
           │   g_bootinfo, ...    │
           └──────────────────────┘  __bss_end
                                     __kernel_end (aligné 4 Kio)
```

Les adresses exactes dépendent de la taille du code ; `build/samos.map` les donne pour chaque build.

**Pourquoi aligner chaque section sur 4 Kio ?** La pagination (K0.6) attribue des droits par page de 4 Kio. Si `.text` et `.data` partageaient une page, celle-ci devrait être à la fois exécutable et inscriptible, ce qui viole W^X.

**Pourquoi trois segments ?** Avec un seul segment RWX, ld 2.42 émettrait un avertissement (rendu fatal ici), et l'intention de protection serait perdue. Les `PHDRS` explicites documentent le modèle de permissions que la pagination appliquera.

**Symboles du linker.** `__kernel_start` et ses semblables n'occupent aucun espace mémoire : un symbole de linker est une **adresse**, pas une variable. En C, on les déclare `extern char __kernel_start[];` et on n'utilise que leur adresse. Lire `__kernel_start[0]` lirait le premier octet du noyau, pas une valeur stockée.

---

## 5. Registres et modes du processeur i386

### 5.1 Les registres utilisés en K0.1

| Registre | Rôle |
|---|---|
| EAX, EBX, ECX, EDX, ESI, EDI | registres généraux 32 bits |
| ESP | pointeur de pile (*stack pointer*) |
| EBP | pointeur de cadre (*frame pointer*), par convention |
| EIP | pointeur d'instruction |
| EFLAGS | indicateurs d'état et de contrôle |
| CR0 | registre de contrôle : PE (bit 0, mode protégé), WP (bit 16), PG (bit 31, pagination) |
| CS, DS, SS… | sélecteurs de segment (étudiés en K0.4) |

Bits d'EFLAGS pertinents (SDM vol. 1, §3.4.3) :

- **IF** (bit 9) : autorise les interruptions matérielles masquables ;
- **DF** (bit 10) : direction des instructions de chaîne (`movs`, `stos`…) ; 0 = adresses croissantes ;
- le **bit 1** est réservé et vaut toujours 1. Après `push 0 ; popfd`, EFLAGS vaut donc `0x00000002`.

### 5.2 Mode réel, mode protégé, mode long

| Mode | Adresses | Protection | Entrée |
|---|---|---|---|
| Réel | 20 bits (segment × 16 + offset) | aucune | état au reset |
| Protégé 32 bits | 32 bits, segmentation par descripteurs, pagination optionnelle | anneaux 0-3, droits par segment/page | CR0.PE = 1 |
| Long (IA-32e) | 48 bits virtuels (4 niveaux) | pagination obligatoire, segmentation quasi plate | EFER.LME = 1 + CR0.PG = 1 |

Dans SAMOS K0, la transition mode réel → mode protégé est faite **par le chargeur**. Le noyau démarre directement en mode protégé.

---

## 6. La pile, la convention d'appel cdecl et l'alignement

### 6.1 Sémantique des instructions de pile

**[ARCH]** La pile x86 croît vers les adresses **décroissantes** :

```text
push x  :  ESP ← ESP − 4 ; [ESP] ← x
pop  r  :  r ← [ESP] ; ESP ← ESP + 4
call f  :  push (adresse de retour) ; EIP ← f
ret     :  pop EIP
```

ESP est **indéfini** à l'entrée Multiboot. La toute première instruction utile de `_start` est donc `mov esp, boot_stack_top`. Le moindre `push` avant cette ligne écrirait à une adresse arbitraire, peut-être dans le code du noyau ou dans la structure `multiboot_info`.

### 6.2 La convention cdecl (ABI i386 System V)

- Arguments empilés **de droite à gauche** : le premier argument est à l'adresse la plus basse, `[ESP+4]` à l'entrée de la fonction.
- Valeur de retour dans EAX (EDX:EAX pour 64 bits).
- Registres **préservés par l'appelé** (*callee-saved*) : EBX, ESI, EDI, EBP (et ESP). Registres que l'appelé peut détruire : EAX, ECX, EDX.
- C'est l'appelant qui retire les arguments.
- **DF = 0** à l'entrée et à la sortie de chaque fonction.

Appliqué à `kmain(uint32_t magic, uint32_t mbi_phys)` :

```text
        ┌──────────────┐ boot_stack_top (≡ 0 mod 16)
        │  (8 o vides) │ ← sub esp, 8
        │   EBX (mbi)  │ ← push ebx    : 2e argument
ESP →   │   ESI (magic)│ ← push esi    : 1er argument     ESP ≡ 0 mod 16 avant call
        │   retour     │ ← poussé par call ; à l'entrée de kmain ESP ≡ 12 mod 16
        └──────────────┘
```

### 6.3 Démonstration de l'alignement

Posons $T$ = `boot_stack_top`, avec $T \equiv 0 \pmod{16}$ (garanti par `align=16` et une taille de pile multiple de 16).

- `mov esp, T` donne $\text{ESP} = T$.
- `sub esp, 8` donne $\text{ESP} \equiv 8$.
- Deux `push` (−8) donnent $\text{ESP} \equiv 0$. **C'est la condition ABI au point d'appel.**
- `call` (−4) : à l'entrée de `kmain`, $\text{ESP} \equiv 12 \pmod{16}$, c'est-à-dire $(\text{ESP}+4) \equiv 0$.
- Prologue `push ebp ; mov ebp, esp` : $\text{EBP} \equiv 8$, donc $(\text{EBP}+8) \equiv 0$.

Les deux dernières égalités sont exactement ce que vérifient les tests :

- GDB, arrêté sur la première instruction de `kmain`, contrôle `ESP % 16 == 12` ;
- l'autotest noyau contrôle `(kmain_frame_address + 8) % 16 == 0`, où `kmain_frame_address = __builtin_frame_address(0)` vaut EBP.

`entry.asm` contient en plus une garde d'exécution (`test esp, 0xF ; jnz .halt`) : si une modification future cassait l'alignement, le noyau s'arrêterait au lieu d'exécuter du C hors ABI.

### 6.4 Pourquoi forcer DF = 0 ?

Le compilateur suppose DF = 0 et peut utiliser `rep movsb`/`rep stosd` pour copier ou initialiser des structures. Avec DF = 1, ces instructions progresseraient vers les adresses décroissantes et corrompraient la mémoire située *avant* la destination. Multiboot ne garantit pas DF. `push 0 ; popfd` remet tout EFLAGS dans un état connu, ce qui est plus fort qu'un simple `cld`.

### 6.5 Taille de la pile

**[CHOIX]** 16 Kio. En K0 il n'y a ni récursion profonde ni gros tableau local. Une interruption consommera quelques dizaines d'octets de cadre CPU, plus les registres sauvegardés. **Limite connue :** il n'y a pas de page de garde. Un débordement descendrait sous `boot_stack_bottom` et écraserait silencieusement les autres variables de `.bss`. La pagination de K0.6 permettra une page non présente sous la pile, transformant ce débordement en faute de page détectable.

---

## 7. De l'assembleur au C : préparer l'environnement d'exécution

Le C suppose un environnement que le matériel ne fournit pas de lui-même. `_start` le construit dans un ordre dicté par les dépendances :

| Ordre | Action | Dépend de | Raison |
|---|---|---|---|
| 1 | `cli` | — | aucune IDT : une interruption provoquerait un triple fault |
| 2 | `mov esp, boot_stack_top` | — | toute instruction suivante peut utiliser la pile |
| 3 | `push 0 ; popfd` | pile | DF = 0 (ABI) ; état EFLAGS déterministe |
| 4 | effacer `.bss` | DF = 0 | norme C : objets statiques sans initialiseur = 0 |
| 5 | arguments + alignement | pile | ABI cdecl |
| 6 | `call kmain` | tout ce qui précède | — |

**Pourquoi effacer `.bss` alors que le chargeur doit le faire ?** Un chargeur ELF conforme remplit bien de zéros la zone `p_memsz − p_filesz`. **[CHOIX]** SAMOS ne s'appuie pas sur un comportement qu'il peut garantir lui-même pour un coût négligeable (environ 21 Kio écrits une fois). C'est de la défense en profondeur. L'opération ne perd rien : EAX (magic) est copié dans ESI avant `rep stosd`, qui utilise EAX, ECX et EDI ; EBX n'est pas touché ; la pile, située dans `.bss`, est vide à cet instant.

**Pourquoi la structure Multiboot n'est-elle pas écrasée ?** Le chargeur doit placer `multiboot_info` hors de l'image du noyau, `.bss` comprise, puisque l'image est définie par `p_memsz`. QEMU la place à `0x9500`, sous 1 Mio. `bootinfo_region_ok` vérifie de toute façon l'absence de chevauchement.

---

## 8. Arrêter le processeur en sécurité

**[ARCH]** `HLT` arrête l'exécution jusqu'à la prochaine interruption externe, NMI, SMI, INIT ou reset. Avec IF = 0, les interruptions masquables ne réveillent plus le CPU, mais **un NMI le peut** : c'est une interruption non masquable, déclenchée par exemple par une erreur matérielle. Un `hlt` seul pourrait donc « tomber » sur la suite du code, c'est-à-dire sur des octets quelconques.

D'où la boucle :

```nasm
.halt:  cli
.loop:  hlt
        jmp .loop
```

`cpu_halt_forever()` en C applique le même principe (`for(;;) { cli; hlt; }`).

**Limite :** aujourd'hui, un NMI pendant `hlt` trouverait une IDT absente et produirait un triple fault. Cette limite disparaîtra avec l'IDT de K0.4, qui comprendra un gestionnaire NMI (vecteur 2).

---

## 9. Entrées/sorties : ports d'E/S et canal de diagnostic QEMU

### 9.1 Deux espaces d'adresses

**[ARCH]** x86 dispose de deux mécanismes pour dialoguer avec les périphériques (SDM vol. 1, chap. 19) :

- **E/S projetées en mémoire** (MMIO) : le périphérique répond à des adresses physiques, et on y accède par des `mov` ordinaires. Exemple : le tampon VGA texte à `0xB8000` (K0.3).
- **Espace d'E/S séparé** : 65 536 ports de 8 bits, accessibles uniquement par `IN`/`OUT`. Exemples : COM1 à `0x3F8`, le PIC à `0x20/0x21`, le PIT à `0x40-0x43`.

`OUT` n'accepte la donnée que dans AL/AX/EAX, et le numéro de port soit en immédiat 8 bits, soit dans DX. C'est ce qu'expriment les contraintes `"a"` et `"Nd"` de `arch/io.h`. **[ARCH]** En mode protégé, `IN`/`OUT` sont autorisés seulement si CPL ≤ IOPL, ou si la bitmap d'E/S du TSS le permet. Le noyau en CPL 0 y a toujours accès. Les futurs programmes en Ring 3 n'y auront pas accès, et c'est une barrière de sécurité essentielle.

### 9.2 Le canal debugcon (port 0xE9)

QEMU et Bochs implémentent un port spécial : chaque octet écrit sur `0xE9` est transmis à l'hôte (`-debugcon stdio` ou `-debugcon file:...`). Aucune initialisation n'est requise. **[CHOIX]** C'est le premier canal de sortie de SAMOS, choisi parce qu'il fonctionne avant tout pilote et qu'il est capturable automatiquement par les scripts de test. **Limite :** il n'existe pas sur du matériel réel. Le port série (K0.3) prendra le relais pour le diagnostic sur machine physique.

### 9.3 Le périphérique `isa-debug-exit`

`-device isa-debug-exit,iobase=0xf4,iosize=0x04` : écrire une valeur $v$ sur le port `0xF4` termine QEMU avec le code $(v \ll 1) \mid 1$. Le statut est toujours impair, ce qui le distingue d'une fin « normale » de QEMU.

**[CHOIX]** Valeurs utilisées : `0x10` (succès → 33), `0x11` (échec → 35), `0x12` (panic → 37). La valeur 0 a été volontairement évitée : elle donnerait le statut 1, qui est aussi le code d'erreur générique de QEMU. Un plantage de l'émulateur ne pourra donc jamais être pris pour une réussite.

---

## 10. Sécurité du jalon K0.1

Conformément à la méthode du cahier des charges (§10), chaque sous-système définit ses actifs, frontières, menaces, invariants, protections, tests et limites.

| Aspect | K0.1 |
|---|---|
| **Actifs** | intégrité de l'image du noyau (code et données), de la pile, de l'état CPU |
| **Frontière de confiance** | chargeur → noyau. La structure `multiboot_info`, la ligne de commande et le nom du chargeur sont des **entrées externes** |
| **Menaces** | chargeur non conforme ou bogué ; pointeurs nuls ou hors limites ; chaîne non terminée ; structure placée dans l'image du noyau ; démarrage sans chargeur Multiboot ; état EFLAGS inattendu |
| **Invariants** | (1) pas d'utilisation de `multiboot_info` sans magic valide ; (2) toute région externe est non nulle, sans débordement 32 bits et hors de `[__kernel_start, __kernel_end)` ; (3) aucune lecture de chaîne externe au-delà de `BOOTINFO_CMDLINE_MAX − 1` octets ; (4) le C s'exécute avec une pile valide, alignée, DF = 0, IF = 0 |
| **Protections** | `bootinfo_init` + `bootinfo_region_ok` ; copies bornées (`strnlen`) dans des tampons du noyau de taille fixe ; arrêt explicite en cas d'échec ; garde d'alignement dans `_start` ; segments séparés R-X/R--/RW- |
| **Tests** | rejet de l'adresse nulle, du débordement (`0xFFFFFFF0 + 0x20`), du chevauchement avec `.text`, acceptation de la limite exacte (`0xFFFFFFF0 + 0x10`) ; test négatif du magic invalide via GDB ; `strnlen` sous ASan sur un tampon sans terminateur |
| **Limites connues** | pas de pagination : les permissions ELF ne sont **pas** appliquées par le CPU, et un bogue du noyau peut écrire dans `.text` ; pas de page de garde de pile ; un `mmap_addr` « plausible » n'est pas pour autant prouvé correct (la validation du contenu viendra en K0.6) ; `copy_untrusted_string` lit un octet supplémentaire pour détecter la troncature (sans danger sans pagination, mais à revoir si la cmdline était copiée après activation de la pagination) |

Aucune garantie de sécurité n'est revendiquée. Ces mesures réduisent des risques identifiés, sans prouver l'absence de défaut.

---

## 11. Stratégie de vérification

Plusieurs niveaux complémentaires sont nécessaires, car aucun ne suffit seul :

| Niveau | Outil | Ce qu'il prouve | Ce qu'il ne prouve pas |
|---|---|---|---|
| Statique ELF | `tools/check_elf.sh` | structure du fichier, en-tête, absence de dépendances cachées, absence de W+X | le comportement à l'exécution |
| Unitaire hébergé | GCC + ASan + UBSan | absence d'accès hors limites et de comportement indéfini détectable dans le code portable | le comportement sur i386 freestanding |
| Autotests noyau | QEMU `-append test` | les invariants observés depuis le noyau lui-même | l'état avant l'entrée en C |
| Instruction | GDB | état exact des registres à une instruction donnée ; chemins d'erreur impossibles à provoquer autrement | le comportement sur matériel réel |
| Boot complet | QEMU + `-d cpu_reset` | absence de triple fault, messages attendus | — |

---

## 12. Questions de jury et réponses (K0.1)

**Q1. Que se passe-t-il entre l'appui sur le bouton et l'exécution de `_start` ?**
Le CPU démarre en mode réel à `0xFFFFFFF0` et exécute le firmware. Le BIOS fait le POST puis charge le chargeur d'amorçage. Le chargeur Multiboot active A20, passe en mode protégé, lit les *program headers* ELF, copie les segments à 1 Mio, prépare `multiboot_info`, place le magic dans EAX et l'adresse dans EBX, puis saute au point d'entrée ELF (`_start`).

**Q2. Pourquoi votre noyau est-il chargé à 1 Mio et pas à 0 ?**
Le premier Mio est occupé par des structures historiques : la table des vecteurs d'interruption du mode réel (0–0x3FF), la BDA, l'EBDA, la mémoire vidéo (0xA0000–0xBFFFF) et les ROM (0xC0000–0xFFFFF). À partir de 1 Mio commence la mémoire étendue, libre sur tout PC.

**Q3. À quoi sert le magic `0x2BADB002` dans EAX ?**
À prouver que le noyau a été démarré par un chargeur conforme Multiboot, et donc que le contenu d'EBX et l'état machine respectent la spécification. Sans cette preuve, EBX pourrait contenir n'importe quoi. Le noyau refuse alors de démarrer, comme le démontre le test GDB.

**Q4. Pourquoi ne pas écrire directement `kmain` en C comme point d'entrée ?**
Parce qu'à l'entrée ESP est indéfini, alors que le code C compilé utilise la pile dès le prologue (`push ebp`). Il faut au moins une instruction assembleur pour créer la pile, et il faut aussi garantir DF = 0 et l'alignement, ce que le C ne peut pas exprimer.

**Q5. Pourquoi `sub esp, 8` avant de pousser les arguments ?**
Pour que ESP soit multiple de 16 au moment du `call` : 16 − 8 (padding) − 8 (deux arguments) ≡ 0. C'est la convention i386 System V telle qu'appliquée par GCC sous Linux, et imposée ici par `-mstack-alignment=16`. La démonstration complète est au §6.3, et sa vérification se fait par GDB.

**Q6. Différence entre une section et un segment ?**
La section est une unité logique pour l'éditeur de liens (`.text`, `.data`…). Le segment est une unité de chargement pour le chargeur (`PT_LOAD`, avec adresse, taille et permissions). Plusieurs sections forment un segment ; le chargeur ignore les sections.

**Q7. Pourquoi `.bss` n'occupe-t-elle pas de place dans le fichier ?**
Elle ne contient que des zéros, qu'il est inutile de stocker. Le segment déclare simplement `p_memsz > p_filesz`, et la différence est remplie de zéros au chargement. Notre pile de 16 Kio est dans `.bss` : le fichier ELF reste petit.

**Q8. Que se passerait-il si une exception survenait maintenant ?**
Il n'y a pas d'IDT valide. Le CPU ne peut pas livrer l'exception, ce qui provoque une double faute (#DF). Celle-ci ne peut pas être livrée non plus, d'où un **triple fault** : le processeur se réinitialise. Sous QEMU avec `-no-reboot`, l'émulateur s'arrête, et notre script de test détecte ce reset supplémentaire.

**Q9. Pourquoi `hlt` dans une boucle ?**
Parce qu'un NMI peut réveiller le CPU même avec IF = 0. Sans boucle, le processeur exécuterait les octets qui suivent `hlt`.

**Q10. Pourquoi interdire SSE dans le noyau ?**
Les registres XMM ne sont pas initialisés : CR0.EM, CR0.MP et CR4.OSFXSR ne sont pas configurés, et une instruction SSE provoquerait #UD. Plus tard, chaque interruption devrait de plus sauvegarder et restaurer l'état SSE, ce qui serait coûteux. La plupart des noyaux (Linux compris) s'interdisent les flottants hors sections explicitement protégées.

**Q11. Comment savez-vous que votre en-tête Multiboot est valide ?**
Par trois preuves indépendantes : (1) `check_elf.sh` le retrouve à l'offset 4096 (< 8192), aligné, avec une somme nulle modulo $2^{32}$ ; (2) QEMU, qui implémente la spécification, accepte et démarre le noyau ; (3) au démarrage, EAX contient bien `0x2BADB002` (vérifié par GDB).

**Q12. Votre noyau est-il sûr ?**
Non, et aucune affirmation de ce type n'est faite. Il valide ses entrées externes et s'arrête en cas d'anomalie, mais sans pagination le CPU n'applique aucune protection mémoire : tout code noyau peut écrire partout. Les protections réelles commencent en K0.6.

---

## 13. Le script de liaison en profondeur (K0.2)

### 13.1 Rôle de l'éditeur de liens

Le compilateur produit des **fichiers objets relogeables** : leurs sections commencent à l'adresse 0 et leurs références externes sont des *relocations* en attente. L'éditeur de liens :

1. **fusionne** les sections d'entrée de même nature (`.text` de chaque `.o`) en sections de sortie ;
2. **attribue une adresse** à chaque section de sortie ;
3. **résout les symboles** et applique les relocations (par exemple `R_386_32` : écrire l'adresse absolue d'un symbole, ou `R_386_PC32` : écrire un déplacement relatif à EIP) ;
4. produit les *program headers* qui disent au chargeur quoi charger, et où.

Sans script, ld utilise un script par défaut prévu pour les exécutables Linux, qui chargerait à `0x08048000` avec un segment pour l'interpréteur dynamique. Un noyau a besoin d'un contrôle total, d'où `-T linker.ld`.

### 13.2 Le compteur de position `.`

Dans `SECTIONS`, le symbole `.` est le **compteur de position** : l'adresse courante (VMA). `. = 0x00100000;` le place à 1 Mio. Chaque section de sortie avance `.` de sa taille. `ALIGN(PAGE_SIZE)` arrondit au multiple de 4096 supérieur :

$$\text{ALIGN}(a, n) = \left\lceil \frac{a}{n} \right\rceil \cdot n = (a + n - 1) \mathbin{\&} \lnot(n-1) \quad (n \text{ puissance de 2})$$

`. += PAGE_SIZE;` dans `.bootstack` réserve 4 Kio sans y placer aucun contenu : c'est ainsi qu'est créée la zone de garde.

### 13.3 VMA et LMA

Chaque section a une **VMA** (*Virtual Memory Address*, l'adresse à laquelle le code s'exécute et pour laquelle il est relogé) et une **LMA** (*Load Memory Address*, l'adresse où le chargeur la dépose, reflétée dans `p_paddr`). **[CHOIX]** En K0 elles sont égales (identité), ce que `check_elf.sh` vérifie (`vaddr == paddr`). Un noyau *higher-half* aurait VMA = `0xC0100000` et LMA = `0x00100000`, avec `AT(...)` dans le script et une pagination activée dans le code d'entrée avant tout accès absolu. Cette évolution est reportée (ADR 0001, décision 6).

### 13.4 Directives utilisées

| Directive | Signification | Usage dans SAMOS |
|---|---|---|
| `OUTPUT_FORMAT("elf32-i386")`, `OUTPUT_ARCH(i386)` | format et architecture de sortie | refuse de lier un objet 64 bits par erreur |
| `ENTRY(_start)` | valeur de `e_entry` dans l'en-tête ELF | adresse à laquelle le chargeur saute |
| `PHDRS { ... }` | déclaration explicite des *program headers* | 3 segments R-X, R--, RW- |
| `:text`, `:data` | affectation d'une section de sortie à un segment | regroupement par permission |
| `KEEP(...)` | interdit l'élimination par `--gc-sections` | l'en-tête Multiboot n'est référencé par aucun code |
| `(NOLOAD)` | section de type NOBITS | pile et zone de garde : aucun octet dans le fichier |
| `*(COMMON)` | symboles « communs » (globales non initialisées à l'ancienne) | vide grâce à `-fno-common`, gardé par robustesse |
| `/DISCARD/` | sections supprimées | `.comment`, `.note*`, `.eh_frame` |
| `ASSERT(expr, msg)` | échec de la liaison si `expr` est faux | invariants vérifiés **à chaque build** |

### 13.5 Les assertions de liaison

Une assertion de liaison transforme une hypothèse en erreur de compilation. SAMOS vérifie :

- `__kernel_start == 0x00100000` : le noyau commence à 1 Mio ;
- `.bss` alignée sur 4 au début et à la fin : nécessaire à `rep stosd`, qui écrit des mots de 4 octets ;
- `boot_stack_bottom == __stack_guard_end` : la pile suit **immédiatement** la zone de garde, sans trou ;
- `boot_stack_top % 16 == 0` : condition de départ de la démonstration d'alignement (§6.3) ;
- `__kernel_end <= 0x00400000` : l'image tient sous 4 Mio. **[CHOIX]** C'est une hypothèse simplificatrice pour K0.6, révisable. Elle évite qu'une croissance silencieuse du noyau ne viole les hypothèses de la pagination initiale.

---

## 14. Disposition mémoire du noyau et modèle de permissions (K0.2)

### 14.1 Carte de l'image (build actuel)

Valeurs réellement affichées au démarrage par `kernel_layout_print()` **au jalon K0.2** (les adresses évoluent avec la taille du code ; en K0.3, `.text` occupe 4 pages et l'image se termine à `0x0010d000`) :

```text
SAMOS: kernel image 0x00100000-0x0010a000
SAMOS:   0x00100000-0x00102000 r-x  .text
SAMOS:   0x00102000-0x00103000 r--  .rodata
SAMOS:   0x00103000-0x00105000 rw-  .data+.bss
SAMOS:   0x00105000-0x00106000 ---G stack-guard
SAMOS:   0x00106000-0x0010a000 rw-  boot-stack
```

L'image occupe 40 Kio en mémoire. Le fichier n'en contient qu'environ 16 Kio de contenu chargé, car `.bss` et `.bootstack` sont NOBITS.

### 14.2 Le principe W^X

**W^X** (*write XOR execute*) : aucune page ne doit être à la fois inscriptible et exécutable. Si un attaquant, ou un bogue, peut écrire dans une zone exécutable, il peut injecter du code. Si une zone de code est non inscriptible et une zone de données non exécutable, l'injection directe de code devient impossible et il faut recourir à des techniques plus complexes, comme le ROP.

**[ARCH]** Sur i386 sans PAE, la pagination ne connaît que le bit **R/W**. Il n'y a **pas** de bit « non exécutable » : toute page présente est exécutable. Le bit NX (*Execute Disable*, bit 63 des entrées) n'existe qu'avec PAE ou en mode long (Intel SDM vol. 3A, §4.6). Conséquence importante pour K0.6 :

| Protection | i386 sans PAE (K0) | PAE / x86-64 (K1) |
|---|---|---|
| `.text` non inscriptible | ✅ (R/W = 0 + CR0.WP = 1) | ✅ |
| `.rodata` non inscriptible | ✅ | ✅ |
| données non exécutables | ❌ impossible | ✅ (NX) |
| zone de garde non présente | ✅ (P = 0) | ✅ |

Les permissions `LAYOUT_*` de `layout.h` sont donc un **modèle d'intention**. En K0.6, seule une partie sera appliquée par le matériel. C'est une limite documentée, et l'un des arguments techniques en faveur de la migration x86-64.

### 14.3 L'API de disposition

`kernel_layout()` renvoie un tableau de régions contiguës, alignées sur 4 Kio, sans chevauchement, couvrant exactement `[__kernel_start, __kernel_end)`. Ces propriétés sont vérifiées par `kernel_layout_valid()` à chaque démarrage, et le noyau s'arrête si elles sont violées. Cette table servira deux fois en K0.6 :

- pour **réserver** l'image du noyau dans l'allocateur de frames physiques, afin de ne jamais allouer une page du noyau ;
- pour **créer les entrées de pages** avec les bons droits.

---

## 15. Pile de démarrage : zone de garde et peinture (K0.2)

### 15.1 Le problème

Une pile n'a pas de limite matérielle en mode protégé à segments plats : `push` décrémente ESP sans contrôle. Un débordement, par récursion ou par gros tableau local, écrit sous `boot_stack_bottom`. En K0.1, cette zone était le reste de `.bss`, notamment `g_bootinfo` : la corruption aurait été **silencieuse**.

### 15.2 La solution en deux temps

1. **K0.2 (logiciel).** La pile a sa propre section `.bootstack`, précédée d'une **zone de garde de 4 Kio** qui ne contient aucune donnée. Un débordement modéré (< 4 Kio) n'écrase que la garde. Celle-ci est **peinte** au démarrage avec le motif `0x5AFE57AC`, et `boot_stack_guard_intact()` vérifie qu'elle est intacte.
2. **K0.6 (matériel).** La page de garde sera **non présente** (P = 0) dans les tables de pages. Le premier accès provoquera une faute de page (#PF, vecteur 14) **au moment même** du débordement, et non a posteriori.

**Limites de la détection logicielle :**

- elle est **a posteriori** : on constate la corruption après coup, sans savoir quand elle a eu lieu ;
- un débordement de plus de 4 Kio sauterait la garde sans forcément l'écrire (par exemple un grand tableau local non initialisé suivi d'une écriture lointaine) ;
- une écriture légitime égale au motif n'est pas détectée. La probabilité est faible mais non nulle.

### 15.3 Mesure de la profondeur maximale (*high-water mark*)

La pile entière est peinte avant l'appel de `kmain`. Comme elle croît vers le bas, l'adresse la plus basse dont le motif a été écrasé donne la profondeur maximale atteinte :

$$\text{profondeur} = \texttt{boot\_stack\_top} - \min\{\,a \mid \text{mot}(a) \neq \text{motif}\,\}$$

Mesure réelle après démarrage normal : **116 octets** sur 16 384. L'autotest vérifie que l'appel d'une fonction utilisant un tableau local de 2 Kio fait bien monter la mesure à au moins 2048 octets. L'instrument est donc réellement sensible. Cette mesure sera précieuse en K0.4/K0.5 pour dimensionner la pile face aux cadres d'interruption.

### 15.4 Ordre des opérations dans `_start`

La peinture a lieu **après** l'effacement de `.bss` et **avant** la préparation des arguments, quand la pile est vide. Écrire le motif sur des cases déjà utilisées détruirait des données. À ce moment, la seule utilisation précédente est le couple `push 0 / popfd`, terminé.

---

## 16. Sécurité du jalon K0.2

| Aspect | K0.2 |
|---|---|
| **Actifs** | séparation code/données, intégrité de la pile et des variables globales |
| **Menaces** | débordement de pile corrompant `.bss` ; section mal placée créant une page W+X ; croissance du noyau violant les hypothèses mémoire ; dépendance PIC/dynamique introduite par une option de compilation |
| **Invariants** | régions contiguës, alignées sur 4 Kio, sans chevauchement ; aucune région W+X ; garde de 4 Kio immédiatement sous la pile ; image < 4 Mio ; aucune section `.got`/`.plt`/`.dynamic`/`.interp`/`.rel*` |
| **Protections** | `PHDRS` à permissions séparées ; `ASSERT` de liaison ; `kernel_layout_valid()` au démarrage ; zone de garde peinte ; `check_elf.sh` (sections, ordre, NOBITS) |
| **Tests** | 18 vérifications de la suite K0.2, dont un **test négatif** (écriture simulée dans la garde → détectée, puis restaurée) ; tests négatifs de `check_elf.sh` (somme corrompue, magic absent → rejet) |
| **Limites** | permissions non appliquées par le CPU avant K0.6 ; pas de NX possible en i386 sans PAE ; détection de débordement a posteriori seulement |

---

## 17. Questions de jury et réponses (K0.2)

**Q1. Pourquoi ne pas laisser ld utiliser son script par défaut ?**
Il est conçu pour un programme Linux : adresse de base `0x08048000`, segment `PT_INTERP`, sections dynamiques. Un noyau doit contrôler son adresse de chargement, l'ordre des sections (l'en-tête Multiboot en premier), les permissions des segments et les symboles de disposition.

**Q2. Qu'est-ce qu'un symbole `__bss_start` en C ?**
Une adresse attribuée par l'éditeur de liens, sans stockage associé. On le déclare comme un tableau (`extern char __bss_start[]`) et on n'utilise que son adresse. Lire son contenu reviendrait à lire le premier octet de `.bss`.

**Q3. Pourquoi aligner chaque section sur 4 Kio, au prix de la mémoire perdue ?**
Parce que la protection mémoire s'applique par page. Une page partagée entre `.text` et `.data` devrait être à la fois inscriptible et exécutable. Le coût est d'au plus 4 Kio par section, soit environ 16 Kio, négligeable devant les 128 Mio de la machine de test.

**Q4. Votre noyau applique-t-il W^X ?**
Pas encore. Le fichier ELF le déclare (segments séparés) et le noyau le vérifie sur son modèle, mais sans pagination le CPU n'applique rien. En K0.6, `.text` et `.rodata` deviendront non inscriptibles. En revanche, l'i386 sans PAE ne permet pas de rendre les données non exécutables : il faudra PAE ou le mode long pour le bit NX.

**Q5. Comment détectez-vous un débordement de pile aujourd'hui ? Et demain ?**
Aujourd'hui, par une zone de garde peinte, vérifiée par logiciel : la détection est a posteriori. Demain (K0.6), par une page non présente qui provoque une faute de page au moment exact du débordement. Ce mécanisme matériel est fiable même pour du code non instrumenté.

**Q6. Comment mesurez-vous l'utilisation de la pile ?**
Par la technique de peinture (*stack painting*) : on remplit la pile d'un motif connu, puis on cherche la case la plus basse qui ne le contient plus. L'autotest prouve la sensibilité de la mesure avec une consommation contrôlée de 2 Kio.

**Q7. Différence entre VMA et LMA ?**
La VMA est l'adresse d'exécution, pour laquelle le code est relogé. La LMA est l'adresse de chargement physique. Elles sont égales en K0 (identité) et différeraient dans un noyau *higher-half*.

## 18. Affichage et diagnostic : VGA, UART 16550, journal, panique (K0.3)

### 18.1 Le mode texte VGA

**Historique.** La norme VGA (IBM, 1987) a hérité du CGA/EGA un **mode texte** dans lequel la carte graphique génère elle-même les glyphes à partir d'une police en ROM. Le processeur n'écrit que des codes de caractères. Le BIOS initialise ce mode (mode 3 : 80×25, 16 couleurs) avant de démarrer le chargeur.

**[ARCH] Le tampon de texte.** En mode couleur, la carte décode les adresses physiques `0xB8000`–`0xBFFFF`. Le premier écran occupe $80 \times 25 \times 2 = 4000$ octets. La cellule $(l, c)$ se trouve à :

$$\text{adresse}(l, c) = \texttt{0xB8000} + 2\,(80\,l + c)$$

Format d'une cellule de 16 bits (petit-boutiste) :

```text
 15  14 13 12  11 10 9 8   7 6 5 4 3 2 1 0
┌───┬────────┬──────────┬─────────────────┐
│ B │  fond  │  texte   │ code caractère  │
└───┴────────┴──────────┴─────────────────┘
  B = clignotement (ou fond intense, selon le registre de mode de l'ATC)
```

Exemple vérifié par un autotest : jaune (14 = `0xE`) sur bleu (1) donne l'attribut `0x1E`.

**Pourquoi `volatile` ?** Pour le compilateur, écrire deux fois dans la même case, ou écrire sans jamais relire, est une opération éliminable. Or chaque écriture vers la mémoire vidéo a un **effet observable** (l'affichage). `volatile` impose au compilateur d'émettre chaque accès, dans l'ordre (ISO C11, §6.7.3 et §5.1.2.3).

**Le curseur matériel.** Il est piloté par le **CRTC** (*Cathode Ray Tube Controller*), accessible par deux ports d'E/S : un registre d'index (`0x3D4`) et un registre de données (`0x3D5`). On écrit le numéro de registre dans l'index, puis la valeur dans les données. La position du curseur est un index linéaire $80\,l + c$ réparti sur les registres `0x0E` (octet fort) et `0x0F` (octet faible). Le bit 0 du *Miscellaneous Output Register* (lu au port `0x3CC`) indique si la carte est configurée en couleur (ports `0x3Dx`) ou en monochrome (`0x3Bx`). Le pilote le lit au lieu de supposer.

**Défilement.** Quand on écrit un retour à la ligne sur la dernière ligne, les lignes 1 à 24 sont copiées sur 0 à 23 et la ligne 24 est effacée. Le coût est de $80 \times 24 = 1920$ copies de 16 bits. **Alternative non retenue :** modifier le registre CRTC *Start Address* (`0x0C/0x0D`) pour faire défiler sans copie, ce qui est plus rapide mais complique le retour au début du tampon.

**[CHOIX] Optimisation mesurée et justifiée :** `vga_write` ne met à jour le curseur matériel (4 E/S, coûteuses sur un vrai bus ISA) qu'une fois par appel, et non une fois par caractère.

**Limite fondamentale :** un firmware UEFI pur ne fournit pas le mode texte VGA, seulement un framebuffer graphique (GOP). Sur l'EliteBook du développeur, l'écran VGA ne fonctionnera probablement pas. Un pilote de framebuffer avec une police bitmap serait nécessaire (hors K0).

### 18.2 L'UART 16550 (port série COM1)

**Principe.** Une UART (*Universal Asynchronous Receiver-Transmitter*) sérialise des octets sur une ligne. Chaque trame comprend un bit de start (0), 5 à 8 bits de données (bit de poids faible en premier), un bit de parité optionnel et 1 ou 2 bits de stop (1). Il n'y a pas d'horloge partagée : les deux extrémités doivent être réglées sur le **même débit** (*baud rate*). Le 16550 (National Semiconductor) est le modèle standard des PC. Il possède des FIFO de 16 octets.

**Calcul du débit.** L'UART divise une horloge de 1,8432 MHz par 16 (suréchantillonnage de chaque bit), puis par un **diviseur** programmable sur 16 bits :

$$\text{débit} = \frac{1\,843\,200}{16 \times \text{diviseur}} \quad\Rightarrow\quad \text{diviseur} = 1 \Rightarrow 115\,200 \text{ bauds}$$

Le choix de 1,8432 MHz donne des débits standards exacts : $1\,843\,200 / 16 = 115\,200 = 2^9 \cdot 3^2 \cdot 5^2$.

**Séquence d'initialisation (`serial_init`) et justification :**

| Étape | Écriture | Pourquoi |
|---|---|---|
| 0 | SCR ← `0x5A`, relecture | test de présence : sur un bus sans périphérique, la lecture renvoie en général `0xFF` |
| 1 | IER ← 0 | aucune interruption : le noyau n'a pas encore d'IDT |
| 2 | LCR ← `0x80` (DLAB = 1) | les registres 0 et 1 deviennent DLL/DLM (diviseur) |
| 3 | DLL ← 1, DLM ← 0 | 115 200 bauds |
| 4 | LCR ← `0x03` | 8 bits, pas de parité, 1 stop (« 8N1 »), DLAB = 0 |
| 5 | FCR ← `0xC7` | FIFO activée et vidée, seuil de 14 octets |
| 6 | MCR ← `0x1E`, envoi de `0xAE`, attente de LSR.DR, relecture | **boucle locale** : l'émetteur est relié au récepteur dans la puce. On vérifie de bout en bout que la puce répond |
| 7 | MCR ← `0x0F` | mode normal (DTR, RTS, OUT1, OUT2) |

**Émission par scrutation (*polling*).** Avant chaque octet, on attend que LSR.THRE (bit 5) indique que le registre d'émission est vide. **[CHOIX] Attente bornée :** après 100 000 lectures sans succès, le caractère est abandonné et compté (`serial_dropped()`). Un UART défaillant ne doit jamais bloquer le noyau, et en particulier pas une panique qui cherche à s'afficher.

**Fin de ligne.** Les terminaux série attendent `CR LF` (`\r\n`) : `\n` seul ferait descendre le curseur sans le ramener en colonne 0. Le pilote insère `\r`.

### 18.3 Le journal multi-puits

`console_write()` diffuse chaque octet vers trois puits : debugcon (toujours), série (si le test de boucle locale a réussi) et VGA. `kprintf()` formate dans un **tampon de pile** de 256 octets.

**[CHOIX] Pas d'allocation dynamique.** Il n'existe pas encore d'allocateur, et un journal qui alloue peut échouer précisément quand la mémoire est le problème. Une ligne trop longue est tronquée, puis le marqueur ` [...truncated]` est ajouté et un compteur incrémenté. La troncature n'est donc jamais silencieuse, ce que vérifie un autotest.

**Coût mesuré sur la pile :** la profondeur maximale au démarrage est passée de 116 octets (K0.2) à 792 octets (K0.3), à cause du tampon de 256 octets et des tampons de conversion du formateur. C'est l'utilité de l'instrument de K0.2 : la consommation reste inférieure à 5 % des 16 Kio.

### 18.4 La panique et la trace de pile

**Rôle.** Une panique signifie qu'un invariant est violé et que poursuivre risquerait de corrompre des données ou de masquer le défaut. **[CHOIX]** SAMOS suit le principe *fail-stop* : il s'arrête de façon visible et diagnosticable.

**Séquence de `panic_at()` :**

1. `cli` : aucune interruption ne doit exécuter de code par-dessus un état incohérent. Sans effet en K0.3, mais essentiel à partir de K0.5.
2. **Garde de récursion.** Si `panic` est appelé pendant `panic` (par exemple parce que le journal est lui-même défectueux), une sortie minimale sur debugcon est faite, puis le CPU s'arrête. Cela évite une boucle infinie de paniques.
3. Message, fichier et ligne (macro `panic()` qui injecte `__FILE__` et `__LINE__`), en blanc sur rouge à l'écran.
4. **Trace de pile.** Elle est obtenue en parcourant la chaîne des EBP.
5. En mode test : `qemu_exit(0x12)`, soit le statut 37. Sinon `cpu_halt_forever()`.

**[ARCH] La chaîne des pointeurs de cadre.** Avec `-fno-omit-frame-pointer`, chaque fonction commence par `push ebp ; mov ebp, esp`. Donc `[EBP]` contient l'EBP de l'appelant et `[EBP+4]` l'adresse de retour. Les cadres forment une liste chaînée qu'on remonte :

```text
EBP courant ──► [ EBP appelant ] ──► [ EBP grand-appelant ] ──► ... ──► 0
                [ retour       ]     [ retour             ]
```

`_start` met EBP à 0 avant `call kmain` : c'est le **terminateur** de la liste. Sans cela, la remontée suivrait la valeur résiduelle laissée par le chargeur.

**Validation défensive de chaque cadre.** La trace est produite quand le système est déjà dans un état anormal : la pile elle-même peut être corrompue. `stack_collect()` exige donc, pour chaque cadre :

- qu'il soit dans la pile de démarrage ;
- qu'il soit aligné sur 4 ;
- que le cadre suivant soit **strictement plus haut** dans la pile, ce qui garantit la terminaison même sur une chaîne cyclique ;

le tout avec au plus 16 cadres. Les tests vérifient le refus d'un cadre hors pile, non aligné ou nul.

**Résultat réel** du test `panic_path`, symbolisé avec `addr2line -f -e build/samos.elf` :

```text
#0 0x001025c9  deliberate_panic_inner
#1 0x001025ab  deliberate_panic
#2 0x0010251f  ktest_run_all
#3 0x001001e3  kmain
#4 0x0010005d  _start (instruction suivant call kmain)
```

**Limites :**

- les fonctions *feuilles* très optimisées peuvent ne pas créer de cadre ;
- une adresse de retour désigne l'instruction **suivant** l'appel, d'où la ligne parfois décalée d'une unité dans `addr2line` ;
- après K0.4, une exception survenant pendant une interruption produira une chaîne qui traverse le cadre matériel de l'interruption, qui n'a pas la forme d'un cadre C. La remontée s'y arrêtera ou devra être spécialisée.

### 18.5 Journalisation et interruptions (préparation de K0.5)

`kprintf` n'est **pas réentrant** vis-à-vis de l'écran : si une interruption journalisait pendant qu'un `kprintf` modifie `g_row`/`g_col` dans le pilote VGA, l'état du curseur serait corrompu. **[CHOIX]** Règle pour K0.5 : les gestionnaires d'IRQ ne journalisent pas. Ils déposent des événements dans une file circulaire, que le code normal traite. Une panique depuis une interruption reste permise, car elle ne revient jamais.

---

## 19. Formatage de texte et arithmétique 64 bits sans libgcc (K0.3)

### 19.1 Pourquoi écrire son propre `printf` ?

Sans libc, il n'y a pas de `printf`. Le formateur est aussi un composant **critique pour la sécurité** : il manipule des tampons, des longueurs et des arguments variadiques. Les bogues de formatage (*format string bugs*, débordements de `sprintf`) sont une classe de vulnérabilités historiquement majeure (CWE-134, CWE-120).

### 19.2 Conception

**Invariant central.** Tous les caractères passent par `out_char()`, **seule** fonction qui écrit dans le tampon. Elle n'écrit que si `len < cap − 1`, et compte toujours le caractère. Il en découle que :

1. aucune écriture ne dépasse `buf[size − 1]` ;
2. le terminateur `'\0'` est toujours écrit si `size > 0` ;
3. la valeur de retour est la longueur **complète** qui aurait été produite, comme le spécifie C11 §7.21.6.5 pour `snprintf`. L'appelant détecte la troncature par `ret >= size`.

Analyse d'une spécification `%[drapeaux][largeur][.précision][longueur]conversion` : c'est un automate qui lit les caractères dans cet ordre. Les nombres de largeur et de précision sont bornés à 100 000 pour exclure tout débordement d'`int`.

**Arguments variadiques.** `va_arg(ap, T)` lit le prochain argument en supposant le type `T`. Lire un `long long` (8 octets) quand l'appelant a passé un `int` (4 octets) désynchronise tous les arguments suivants. C'est un comportement indéfini. La protection est l'attribut `format(printf, …)` (`SAMOS_PRINTF`) : le compilateur vérifie à **chaque appel** la concordance entre les conversions et les types. Avec `-Wformat=2 -Werror`, une discordance, ou un format qui n'est pas une chaîne littérale (`-Wformat-nonliteral`), devient une erreur de compilation. Cela interdit aussi, par construction, l'attaque *format string* où une donnée non fiable serait utilisée comme format.

**Écarts volontaires et documentés par rapport à la libc :**

| Cas | glibc | SAMOS | Raison |
|---|---|---|---|
| `%p` | `0x1000`, `(nil)` | `0x00001000` (largeur fixe) | colonnes alignées dans les journaux d'adresses |
| conversion inconnue (`%q`) | comportement indéfini | recopiée telle quelle, aucun argument consommé | pas d'UB dans le noyau |
| `%s` avec `NULL` | `(null)` (extension) | `(null)` | défensif |
| flottants `%f` | supportés | absents | aucun flottant dans le noyau (`-mgeneral-regs-only`) |

### 19.3 Vérification différentielle

**Méthode.** Pour 32 formats (certains combinant plusieurs conversions) couvrant les extrêmes (`INT32_MIN`, `INT64_MIN`, `UINT64_MAX`), les drapeaux, la largeur, la précision et les arguments `*` négatifs, on compare **octet par octet**, valeur de retour comprise, la sortie de `ksnprintf` à celle du `snprintf` de la glibc, prise comme référence C11. C'est un **oracle de test** indépendant. Le tout tourne sous ASan et UBSan. Résultat : 0 différence.

Exemple de subtilité vérifiée : `%.0d` avec 0 ne produit **aucun chiffre** (C11 §7.21.6.1 : « the result of converting a zero value with a precision of zero is no characters »), et une précision explicite annule le drapeau `0`.

**La magnitude d'un entier négatif.** Pour afficher `INT64_MIN`, calculer `-v` en signé déborde, ce qui est un comportement indéfini, car $2^{63}$ n'est pas représentable. Le formateur calcule `(uint64_t)0 - (uint64_t)v`, qui est défini (arithmétique modulo $2^{64}$) et donne exactement $2^{63}$.

### 19.4 Division 64 bits sans libgcc

**[ARCH] Le problème.** L'instruction `DIV r/m32` divise EDX:EAX (64 bits) par un opérande 32 bits et produit un quotient **32 bits** (exception #DE si le quotient déborde). Il n'existe pas de division 64/64 en mode 32 bits. Pour `uint64_t / uint32_t`, le compilateur appelle `__udivdi3`, fournie par libgcc. **[CHOIX]** SAMOS ne lie pas libgcc. Le formateur utilise donc `u64_divmod_u32()`, écrite dans SAMOS.

**Algorithme : division longue binaire avec restauration.** C'est la division « à la main » en base 2. Pour $i = 63$ jusqu'à $0$ :

$$r \leftarrow 2r + n_i ;\qquad \text{si } r \ge d :\ r \leftarrow r - d,\ q_i \leftarrow 1$$

**Invariant :** avant chaque itération, $0 \le r < d$. Après le décalage, $r < 2d \le 2^{33}$, ce qui tient dans un `uint64_t`, sans débordement. Après la soustraction éventuelle, $r < d$ de nouveau. À la fin, $n = q \cdot d + r$ avec $0 \le r < d$, ce qui est la définition de la division euclidienne.

**Complexité :** 64 itérations, chacune faite de décalages et de comparaisons 64 bits que le compilateur traduit en instructions natives (`shld`, `cmp/sbb`, `sub/sbb`), sans aucun appel externe. Un chemin rapide utilise directement `DIV` quand $n < 2^{32}$. Pour la base 16, le formateur utilise des décalages de 4 bits et aucune division.

**Vérification :**

1. 100 000 divisions pseudo-aléatoires (générateur xorshift64) comparées à la division native de l'hôte ;
2. le cas $d = 0$ est **défini**, et non UB : il renvoie `UINT64_MAX` et un reste de 0 ;
3. `nm -u` sur le noyau reste vide, preuve qu'aucun `__udivdi3` n'a été introduit ;
4. `%llu` de `UINT64_MAX` et `%lld` de `INT64_MIN` sont vérifiés **dans le noyau i386**.

---

## 20. Sécurité du jalon K0.3

| Aspect | K0.3 |
|---|---|
| **Actifs** | intégrité de la mémoire du noyau face au code d'affichage ; disponibilité du diagnostic en cas de panne ; exactitude des journaux |
| **Frontières** | matériel (UART, VGA) → noyau : un périphérique absent ou défaillant ; appelants → formateur : formats et arguments |
| **Menaces** | écriture hors du tampon VGA (curseur hors bornes) ; débordement du tampon de formatage ; format non littéral (*format string attack*) ; désynchronisation des arguments variadiques ; UART bloqué figeant le noyau ; panique récursive ; trace de pile suivant un pointeur corrompu ; troncature silencieuse d'un message |
| **Invariants** | `g_row < 25`, `g_col < 80` à la sortie de toute fonction VGA ; aucune écriture au-delà de `buf[size−1]` ; chaque attente matérielle est bornée ; chaque cadre parcouru est dans la pile, aligné, croissant ; au plus 16 cadres |
| **Protections** | bornage dans `vga_set_pos` et `vga_cell` ; `out_char()` unique point d'écriture ; `-Wformat=2 -Werror` ; boucles de scrutation bornées avec compteur de pertes ; garde de récursion de `panic` ; validation de `stack_collect` ; marqueur de troncature |
| **Tests** | 27 vérifications noyau K0.3, dont : bornes VGA, cellule hors écran, refus de cadres invalides, troncature du journal ; test différentiel hébergé du formateur sous ASan/UBSan ; division 64 bits sur 100 000 cas ; chemin de panique de bout en bout (statut 37 + trace) ; mode de test inconnu refusé |
| **Limites** | journal non réentrant (règle : pas de journal en IRQ) ; la trace de pile ne fait pas foi si la pile est délibérément falsifiée ; aucune confidentialité des journaux (les adresses du noyau sont affichées, ce qui faciliterait un attaquant face à une future ASLR, sans objet en K0) ; série en émission seulement |

---

## 21. Questions de jury et réponses (K0.3)

**Q1. Pourquoi deux mécanismes différents pour l'écran (mémoire) et le port série (ports d'E/S) ?**
C'est un choix historique de conception matérielle. Le tampon VGA est une **mémoire** que la carte lit en continu pour générer l'image : on le projette donc dans l'espace d'adresses physiques. L'UART expose des **registres de contrôle** dont la lecture a des effets de bord (lire RBR retire un octet de la FIFO) : ils sont dans l'espace d'E/S, accessibles par `IN`/`OUT`.

**Q2. Comment savez-vous que votre pilote série fonctionne réellement ?**
Par trois preuves :
1. le test de boucle locale interne à l'UART à l'initialisation ;
2. le test QEMU `serial_output`, qui cherche les messages **uniquement** dans le fichier capturé sur COM1 ;
3. `serial_dropped() == 0`, qui montre qu'aucun caractère n'a été perdu par délai dépassé.

**Q3. Comment avez-vous vérifié l'affichage sans regarder l'écran ?**
Les autotests relisent la mémoire vidéo depuis le noyau. Surtout, le test GDB arrête QEMU après le démarrage, **extrait les 4000 octets à `0xB8000` depuis l'extérieur du noyau** et décode un octet sur deux : le texte de démarrage y figure. Cela vérifie ce que l'émulateur de carte VGA affiche réellement.

**Q4. Pourquoi votre `printf` renvoie-t-il une longueur supérieure à la taille du tampon ?**
C'est la sémantique de `snprintf` en C11 : la valeur de retour est la longueur de la sortie complète. L'appelant peut ainsi détecter la troncature (`ret >= size`) et, s'il le souhaite, réessayer avec un tampon plus grand.

**Q5. Qu'est-ce qu'une vulnérabilité de chaîne de format, et comment l'évitez-vous ?**
Si une donnée contrôlée par un attaquant est passée comme format (`kprintf(user_string)`), les `%x` qu'elle contient lisent des valeurs arbitraires sur la pile, et `%n` permet même d'écrire. SAMOS l'évite de deux façons : il n'implémente pas `%n`, et `-Wformat=2 -Werror` rejette à la compilation tout format non littéral.

**Q6. Pourquoi ne pas simplement lier libgcc pour la division 64 bits ?**
Ce serait légitime techniquement. Le projet a choisi de ne pas lier de bibliothèque d'exécution et d'écrire les routines nécessaires, ici une seule, pour pouvoir les expliquer. L'algorithme est la division longue binaire, avec un invariant ($r < d$) qui garantit l'absence de débordement. Il est vérifié sur 100 000 cas.

**Q7. Comment fonctionne votre trace de pile ? Est-elle fiable ?**
Elle remonte la liste chaînée des cadres formée par `push ebp ; mov ebp, esp`. Elle est fiable pour du code compilé avec pointeurs de cadre et une pile intacte. Elle se protège contre une pile corrompue (chaque cadre est validé, la chaîne doit être strictement croissante, au plus 16 cadres), mais elle ne prouve rien si la pile a été falsifiée intentionnellement.

**Q8. Que se passe-t-il si `panic` est appelé pendant `panic` ?**
Le drapeau `g_in_panic` le détecte. On écrit alors un message minimal sur debugcon, le canal le plus simple et sans état, puis on arrête le CPU. On évite ainsi une récursion infinie, par exemple si c'est le pilote VGA qui provoque la faute.

**Q9. Pourquoi la pile a-t-elle grossi de 116 à 792 octets ?**
À cause du tampon de ligne de 256 octets de `kprintf` et des tampons de conversion du formateur (2 × 64 octets). Cette valeur a été mesurée par l'instrument de peinture de pile de K0.2, et non estimée.

## Références

1. Free Software Foundation, *Multiboot Specification*, version 0.6.96, §3.1 (en-tête), §3.2 (état machine), §3.3 (structure d'information).
2. Intel Corporation, *Intel® 64 and IA-32 Architectures Software Developer's Manual* :
   - vol. 1, §3.4.3 (EFLAGS), chap. 19 (Input/Output) ;
   - vol. 2 (jeu d'instructions : `HLT`, `IN`, `OUT`, `PUSHFD/POPFD`, `STOS`) ;
   - vol. 3A, §2.5 (registres de contrôle), §9.1 (état d'initialisation du processeur).
3. *System V Application Binary Interface — Intel386 Architecture Processor Supplement*, 4e éd., chap. 3 (convention d'appel, pile).
4. TIS Committee, *Executable and Linking Format (ELF) Specification*, version 1.2.
5. ISO/IEC 9899:2011 (C11), §4 (conformité, implémentations autonomes) et §6.7.2.1.
6. GNU Binutils, *ld manual*, chapitre « Linker Scripts » (`SECTIONS`, `PHDRS`, `KEEP`, `NOLOAD`, `ASSERT`, `AT`).
7. Documentation QEMU : options `-kernel` (chargeur Multiboot), `-debugcon`, périphérique `isa-debug-exit`, option `-d cpu_reset`.
8. A. S. Tanenbaum, H. Bos, *Modern Operating Systems*, 4e éd., Pearson, 2014, chap. 1.
9. Intel SDM vol. 3A, §4.3 (pagination 32 bits), §4.6 (droits d'accès, bit XD/NX), §2.5 (CR0.WP).
10. National Semiconductor, *PC16550D Universal Asynchronous Receiver/Transmitter with FIFOs*, fiche technique (registres, diviseur, mode loopback).
11. IBM, *Personal System/2 and Personal Computer BIOS Interface Technical Reference* et documentation VGA (registres CRTC, tampon texte `0xB8000`).
12. ISO/IEC 9899:2011, §7.16 (`<stdarg.h>`), §7.21.6.1 (`fprintf` : drapeaux, largeur, précision), §7.21.6.5 (`snprintf`).
13. MITRE, CWE-134 *Use of Externally-Controlled Format String* ; CWE-120 *Buffer Copy without Checking Size of Input*.
14. D. E. Knuth, *The Art of Computer Programming*, vol. 2 (*Seminumerical Algorithms*), §4.3.1 (arithmétique multiprécision, division).
