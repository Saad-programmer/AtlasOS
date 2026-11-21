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
22. Segmentation, GDT et TSS (K0.4)
23. Interruptions, exceptions et IDT (K0.4)
24. Anatomie d'un gestionnaire : cadre, stubs, IRETD (K0.4)
25. Double faute, porte de tâche et triple fault (K0.4)
26. Sécurité du jalon K0.4
27. Questions de jury et réponses (K0.4)
28. Interruptions matérielles : PIC 8259A, PIT 8254, couche IRQ (K0.5)
29. Concurrence entre interruption et code normal : file SPSC (K0.5)
30. Contrôleur PS/2 8042 et décodage du clavier (K0.5)
31. Sécurité du jalon K0.5
32. Questions de jury et réponses (K0.5)
33. La mémoire physique et sa carte (K0.6)
34. Allocateur de cadres physiques par bitmap (K0.6)
35. Pagination 32 bits : traduction, droits, TLB (K0.6)
36. Fautes de page, sondes et table de correction d'exceptions (K0.6)
37. Sécurité du jalon K0.6
38. Questions de jury et réponses (K0.6)
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

## 22. Segmentation, GDT et TSS (K0.4)

### 22.1 La segmentation en mode protégé

**[ARCH]** En mode protégé, chaque accès mémoire passe par un **segment**. Une adresse logique `sélecteur:offset` est convertie en **adresse linéaire** = base du segment + offset, après vérification de la limite et des droits. Ensuite, si la pagination est active (K0.6), l'adresse linéaire est traduite en adresse physique.

```text
adresse logique ──segmentation──► adresse linéaire ──pagination──► adresse physique
 (CS:EIP, DS:EAX…)    GDT/LDT            (32 bits)        CR3, tables       (bus)
```

Un **sélecteur** (16 bits, valeur des registres CS, DS, SS…) a la forme `index(13) | TI(1) | RPL(2)` :

- `0x08` est l'index 1 de la GDT (TI = 0), avec un RPL de 0 ;
- `0x10` est l'index 2.

Chaque registre de segment a une **partie cachée** (*descriptor cache*) : au chargement du sélecteur, le CPU lit le descripteur dans la table et en garde une copie (base, limite, droits). C'est pourquoi un chargeur peut laisser un GDTR invalide sans que le noyau plante, comme dit au §2.3 : les caches restent valables tant qu'on ne recharge aucun segment.

### 22.2 Le modèle plat

**[CHOIX]** SAMOS utilise, comme Linux, Windows et presque tous les systèmes 32 bits modernes, le **modèle plat** : code et données ont une base 0 et une limite de 4 Gio. La segmentation devient une identité, et toute la protection mémoire est confiée à la pagination, plus fine (par page de 4 Kio) et seule disponible en mode 64 bits, où la segmentation est presque entièrement désactivée. On ne peut pas désactiver la segmentation en mode protégé 32 bits : on la neutralise.

### 22.3 Le descripteur de segment

**[ARCH]** Un descripteur fait 8 octets (SDM vol. 3A, figure 3-8). La base (32 bits) et la limite (20 bits) y sont fragmentées pour des raisons de compatibilité avec le 80286 :

| Octets | Contenu |
|---|---|
| 0-1 | limite[15:0] |
| 2-4 | base[23:0] |
| 5 | **accès** : P (présent), DPL (2 bits, privilège), S (1 = code/données), Type (4 bits) |
| 6 | **drapeaux** : G (granularité), D/B (32 bits), L (64 bits), AVL, puis limite[19:16] |
| 7 | base[31:24] |

**Calcul de la limite effective :** avec G = 1, la limite est en unités de 4 Kio.

$$\text{limite effective} = \text{limite} \times 4096 + 4095 = \texttt{0xFFFFF} \times 4096 + 4095 = \texttt{0xFFFFFFFF}$$

**Octets d'accès de SAMOS**, vérifiés par autotest :

| Segment | Accès | Décodage |
|---|---|---|
| code noyau (0x08) | `0x9B` | P = 1, DPL = 0, S = 1, type 1011 = code, exécutable, lisible, **accédé** |
| données noyau (0x10) | `0x93` | P = 1, DPL = 0, S = 1, type 0011 = données, inscriptible, **accédé** |
| TSS principal (0x18) | `0x8B` après LTR | S = 0, type 1011 = TSS 32 bits **occupé** |
| TSS #DF (0x20) | `0x89` | type 1001 = TSS 32 bits disponible |
| drapeaux code/données | `0xCF` | G = 1, D = 1 (32 bits), limite[19:16] = `0xF` |

**Constat expérimental : le processeur écrit dans la GDT.** La première version déclarait les données en `0x92`. Après chargement de DS, l'autotest a lu `0x93` : le CPU positionne lui-même le bit *Accessed* (SDM vol. 3A §3.4.5.1), **en écrivant en mémoire** dans la table. Pour le segment de code, QEMU n'a pas positionné ce bit lors du saut lointain : une différence d'émulation qu'il ne faut pas tenir pour une garantie. **[CHOIX]** Le bit est pré-positionné : le processeur n'a plus jamais besoin d'écrire dans la GDT. C'est la condition pour pouvoir projeter la GDT en lecture seule après K0.6, et la valeur lue devient déterministe.

### 22.4 Recharger les segments : LGDT puis saut lointain

`LGDT` ne fait que charger le registre GDTR (base + limite). Les registres de segment gardent leurs anciens caches. Il faut les recharger :

- **CS** ne peut pas être chargé par `MOV`. On utilise un **saut lointain** `jmp 0x08:.reload_cs`, qui charge CS et EIP en une seule instruction ;
- **DS, ES, FS, GS, SS** se rechargent par `MOV`. Un `MOV SS` inhibe les interruptions pour l'instruction suivante, pour qu'un couple `mov ss / mov esp` soit atomique (SDM vol. 2, MOV).

### 22.5 Pas de segments utilisateur en K0

**[CHOIX]** Aucun descripteur DPL 3 n'est créé. Aucun code ring 3 n'existe, et un descripteur inutilisé est une surface d'attaque qui ne sert à rien (principe du moindre privilège appliqué à la configuration). Ils seront ajoutés en K1 avec le passage en ring 3.

### 22.6 Le TSS

Le **TSS** (*Task State Segment*, 104 octets) est la structure du **multitâche matériel** de l'i386 : il contient l'état complet d'une tâche (registres, segments, CR3, EIP, EFLAGS) et les piles de privilège (`ss0:esp0`…). Les systèmes modernes n'utilisent plus la commutation de tâches matérielle, plus lente et non portable en 64 bits, mais le TSS reste indispensable pour :

1. **`ss0:esp0`** : la pile noyau chargée lors d'une interruption survenant en ring 3 (K1) ;
2. la **bitmap d'E/S** : `iomap_base = 104` (taille du TSS) signifie qu'il n'y a pas de bitmap, donc tout accès aux ports depuis un ring > IOPL est refusé ;
3. en 32 bits uniquement, les **portes de tâche**, qui servent ici à la double faute (§25).

`LTR` charge le registre de tâche (TR) et marque le descripteur « occupé », d'où le passage de `0x89` à `0x8B`, que vérifie l'autotest.

---

## 23. Interruptions, exceptions et IDT (K0.4)

### 23.1 Trois sources d'événements

| Source | Exemples | Synchrone ? |
|---|---|---|
| **Exceptions** (le CPU détecte une condition) | division par zéro, opcode invalide, faute de page | oui, liée à une instruction |
| **Interruptions matérielles** (IRQ) | timer, clavier (K0.5) | non, asynchrone |
| **Interruptions logicielles** | `INT n`, `INT3` | oui |

Les 32 premiers vecteurs (0-31) sont **réservés par Intel** aux exceptions. C'est pourquoi les IRQ du PIC, qui occupent par défaut 8-15 en mode réel, devront être remappées (K0.5).

### 23.2 Fautes, trappes et abandons

**[ARCH]** La classe d'une exception détermine l'EIP sauvegardé (SDM vol. 3A §6.5) :

| Classe | EIP sauvegardé | Reprise | Exemples |
|---|---|---|---|
| **Faute** (*fault*) | l'instruction **fautive** | ré-exécution après correction | #DE, #UD, #GP, #PF |
| **Trappe** (*trap*) | l'instruction **suivante** | continuation | #BP (`INT3`), #OF |
| **Abandon** (*abort*) | indéfini ou peu fiable | aucune | #DF, #MC |

Cette distinction est **vérifiée expérimentalement** par les autotests K0.4 :

- `int3` : EIP sauvegardé = adresse du label **après** `int3` (trappe) ;
- `ud2` : EIP sauvegardé = adresse **de** `ud2`, et les octets lus à cet EIP sont `0F 0B` (faute) ;
- `div ecx` avec ECX = 0 : EIP = adresse de `div`, octets `F7 F1` (faute).

Pour reprendre après une faute, le gestionnaire de test avance EIP de 2 octets dans le cadre. `IRETD` reprend alors à l'instruction suivante. C'est le principe de l'émulation d'instructions par un noyau. Pour une faute de page, on ne corrige pas EIP : on corrige la cause (on mappe la page) puis on ré-exécute l'instruction.

### 23.3 Le descripteur de porte

**[ARCH]** L'IDT contient jusqu'à 256 **portes** de 8 octets : offset (32 bits, scindé en deux), sélecteur, octet type/attributs `P | DPL | 0 | type` :

| Type | Valeur | Effet |
|---|---|---|
| porte d'**interruption** 32 bits | `0xE` | IF ← 0 à l'entrée |
| porte de **trappe** 32 bits | `0xF` | IF inchangé |
| porte de **tâche** | `0x5` | commutation de tâche matérielle vers le TSS désigné |

**[CHOIX]** Toutes les exceptions utilisent des portes d'**interruption** (`0x8E` = P, DPL 0, type E). Un gestionnaire ne peut donc jamais être interrompu par une IRQ, ce qui supprime toute question d'imbrication en K0. Le coût : la latence des IRQ augmente pendant le traitement d'une exception, ce qui est négligeable ici. L'autotest vérifie IF = 0 dans le gestionnaire.

**DPL 0 sur toutes les portes :** une instruction `INT n` exécutée depuis le ring 3 (futur) produira #GP au lieu d'entrer dans le noyau. Seules les portes explicitement destinées aux appels système (K1) auront DPL 3.

### 23.4 Constat expérimental : #GP ou #NP ?

La première version supposait qu'un `INT 0x80` vers une entrée vide déclencherait #NP (« segment non présent »). Le test a montré **#GP(0x402)**. **Explication :** l'entrée 128 est entièrement nulle, donc son champ *type* vaut 0, qui n'est pas un type de porte valide. Le processeur valide le **type** avant le bit **P** (ordre des contrôles du pseudo-code de l'instruction `INT n`, SDM vol. 2 ; comportement confirmé ici sous QEMU), et un type invalide produit #GP. Une entrée de type valide mais avec P = 0 (vecteurs 32-47, préparés pour K0.5) produit bien #NP.

Les deux cas sont désormais testés :

- `test=np` avec `INT 0x20` donne #NP(0x102) ;
- `test=intbad` avec `INT 0x80` donne #GP(0x402).

**Décodage du code d'erreur** (« format sélecteur », SDM §6.13) : `0x402 = 1000 0000 0010₂`. Le bit 1 (IDT) vaut 1 et l'index vaut `0x402 >> 3 = 128`. Le gestionnaire affiche « selector error: IDT index 128 ». De même pour `MOV DS, 0x0F00` : `0xF00 >> 3 = 480`, avec TI = 0, d'où « GDT index 480 », un index qui dépasse les 5 entrées de la table.

---

## 24. Anatomie d'un gestionnaire : cadre, stubs, IRETD (K0.4)

### 24.1 Ce que fait le processeur

**[ARCH]** À l'entrée par une porte d'interruption **sans changement de privilège** (ring 0 → ring 0), le CPU :

1. empile EFLAGS, CS et EIP (et, avec changement de privilège, SS et ESP **avant** eux : K1) ;
2. empile un **code d'erreur** pour les vecteurs 8, 10-14, 17, 21, 29 et 30 ;
3. met IF à 0 (porte d'interruption), ainsi que TF, NT, RF et VM ;
4. charge CS:EIP depuis la porte.

Il **ne modifie pas DF**. Si le code interrompu avait exécuté `STD`, le gestionnaire C hériterait de DF = 1, ce qui violerait l'ABI.

### 24.2 Les stubs assembleur

Le C ne peut pas servir directement de gestionnaire : il faut sauvegarder **tous** les registres (et pas seulement les registres *caller-saved*), uniformiser le cadre et terminer par `IRETD` au lieu de `RET`. Chacun des 48 stubs générés par macro NASM :

- empile un code d'erreur **factice** (0) si le CPU n'en fournit pas, afin que le cadre ait toujours la même forme ;
- empile son **numéro de vecteur** (un seul `isr_common` sert pour tous) ;
- saute vers `isr_common`.

`isr_common` :

```nasm
pushad                  ; 8 registres généraux
push ds / es / fs / gs  ; segments
mov ax, 0x10 ; mov ds..gs, ax
cld                     ; ABI : DF = 0
mov ebx, esp            ; EBX = &interrupt_frame (callee-saved)
and esp, 0xFFFFFFF0     ; alignement 16
sub esp, 12 ; push ebx  ; ESP ≡ 0 mod 16 avant call
call isr_dispatch
mov esp, ebx
pop gs/fs/es/ds ; popad
add esp, 8              ; vecteur + code d'erreur
iretd
```

### 24.3 La structure `interrupt_frame`

Elle décrit la pile **de l'adresse la plus basse à la plus haute**, soit l'ordre inverse des `push` :

```text
adresse basse  ┌─────────────┐ ← EBX = argument de isr_dispatch
               │ gs fs es ds │ isr_common
               │ edi … eax   │ pushad (EDI empilé en dernier)
               │ vector      │ stub
               │ err_code    │ stub (0) ou CPU
               │ eip cs      │ CPU
adresse haute  │ eflags      │ CPU
               └─────────────┘ ← ESP du code interrompu (ring 0)
```

`_Static_assert(sizeof == 68)` garantit la concordance avec l'assembleur. Le gestionnaire peut **modifier** le cadre, par exemple EIP : `popad`/`iretd` rechargent alors ces valeurs. Le champ ESP de `pushad` n'est **pas** restauré par `popad` (SDM : « the ESP value is discarded »). L'ESP réel du code interrompu se calcule comme `&eflags + 4`.

### 24.4 Pourquoi réaligner la pile ?

La profondeur du cadre varie : 4 ou 5 mots poussés par le CPU selon le code d'erreur, et une pile interrompue d'alignement quelconque. Un gestionnaire C compilé avec `-mstack-alignment=16` suppose ESP ≡ 0 (mod 16) au point d'appel. `and esp, -16` arrondit à la frontière inférieure. EBX, préservé par l'appelé selon cdecl, mémorise l'ancienne valeur. L'autotest vérifie dans le gestionnaire que `(EBP + 8) % 16 == 0` (même démonstration qu'au §6.3).

### 24.5 IRETD

**[ARCH]** `IRETD` dépile EIP, CS et EFLAGS (plus ESP et SS en cas de retour vers un privilège inférieur). Il restaure donc **IF et DF d'origine**. L'autotest le montre : DF = 1 avant `int3`, DF = 0 dans le gestionnaire (après `cld`), DF = 1 de nouveau après le retour.

### 24.6 Gestionnaires enregistrables

`interrupt_register(vector, handler)` refuse un gestionnaire nul ou un vecteur déjà occupé : un module ne peut pas remplacer silencieusement le gestionnaire d'un autre. Toute exception **sans** gestionnaire produit un rapport complet (nom, code d'erreur décodé, registres, segments, CR0/CR2/CR3), puis `panic()`. Rien n'est ignoré.

---

## 25. Double faute, porte de tâche et triple fault (K0.4)

### 25.1 Quand survient une double faute ?

**[ARCH]** Si une exception survient **pendant la livraison** d'une autre, le CPU combine les deux selon leurs classes (SDM vol. 3A, tableau 6-5) :

| Première \ Seconde | bénigne | contributive (#DE, #TS, #NP, #SS, #GP) | faute de page |
|---|---|---|---|
| bénigne | séquentiel | séquentiel | séquentiel |
| contributive | séquentiel | **#DF** | séquentiel |
| faute de page | séquentiel | **#DF** | **#DF** |

Si une troisième exception survient pendant la livraison de #DF, c'est un **triple fault** : le processeur passe en état *shutdown*, et la carte mère le réinitialise.

**Scénario critique pour K0.6.** Un débordement de pile vers la page de garde non présente provoque une faute de page. Pour livrer la #PF, le CPU doit **empiler** EFLAGS/CS/EIP… sur la pile qui vient de déborder, ce qui produit une nouvelle #PF, donc une #DF. Si le gestionnaire de #DF utilise **la même pile**, il échoue lui aussi : triple fault, reset, **aucun diagnostic**.

### 25.2 La solution i386 : une porte de tâche

**[CHOIX]** Le vecteur 8 est une **porte de tâche** vers un TSS dédié (`0x20`), qui définit sa **propre pile** de 4 Kio, son propre EIP (`double_fault_task`), ses segments et son CR3. Lors de la #DF :

1. le CPU **sauvegarde** l'état courant dans le TSS désigné par TR (le TSS principal, d'où l'obligation d'avoir exécuté `LTR`) ;
2. il **charge** l'état complet depuis le TSS de double faute : nouvelle pile valide, quel que soit l'état de l'ancienne ;
3. il empile le code d'erreur de #DF (0) sur la nouvelle pile ;
4. il exécute `double_fault_task`, qui lit l'état interrompu dans le TSS principal et l'affiche, puis panique.

**Alignement à l'entrée de la tâche.** `tss_df.esp` = sommet de la pile (aligné sur 16). Le CPU y empile 4 octets (le code d'erreur), donc à l'entrée ESP = sommet − 4 : $(\text{ESP}+4) \equiv 0 \pmod{16}$, comme pour une fonction appelée normalement. Le code d'erreur occupe la place d'une adresse de retour, que la fonction, `_Noreturn` et sans paramètre, n'utilise jamais.

**Alternative en x86-64 :** les portes de tâche n'existent plus. On utilise l'**IST** (*Interrupt Stack Table*) du TSS 64 bits : jusqu'à 7 piles de secours, sélectionnées par un champ de la porte d'interruption. C'est l'un des points de la migration K1.

### 25.3 Le test de double faute

`test=df` rend la porte de #NP non présente, puis exécute `INT 0x20` :

1. la porte 32 est non présente, d'où **#NP** (contributive) ;
2. pour livrer #NP, le CPU consulte la porte 11, elle aussi non présente, d'où une nouvelle **#NP** (contributive) ;
3. contributive puis contributive donne **#DF**, et la porte de tâche est prise.

Sortie réelle :

```text
*** DOUBLE FAULT (#DF) caught by task gate, running on dedicated stack
    interrupted task: EIP=0x00104850 ESP=0x0010ff70 EBP=0x0010ff88 EFLAGS=0x00000002
*** KERNEL PANIC: double fault
```

L'EIP interrompu se trouve dans `ktest_fatal_mode`, sur l'instruction `int $0x20`. Le test exige en outre l'**absence** de rapport « CPU exception 11 », ce qui prouve qu'aucun gestionnaire de #NP n'a été exécuté. La trace de pile est vide (0 cadre) : `stack_collect` refuse correctement les cadres situés hors de la pile de démarrage, puisque la tâche s'exécute sur sa pile dédiée.

### 25.4 Constat expérimental : détecter un triple fault sous QEMU

`test=triple` charge une IDT de limite 0 puis exécute `INT3`. La porte 3 est hors limite, d'où #GP. La porte 13 l'est aussi, d'où #DF. La porte 8 l'est encore, d'où le triple fault. Le journal `-d cpu_reset,int` de QEMU le confirme :

```text
v=0d e=001a ...                 #GP, code 0x1a = (3 << 3) | 2 : IDT index 3
check_exception old: 0xd new 0xd   -> #DF
v=08 ...
check_exception old: 0x8 new 0xd
Triple fault
```

**Défaut du banc de test découvert grâce à ce test négatif.** Depuis K0.1, la détection d'un triple fault comptait les lignes `CPU Reset`. Or, avec `-no-reboot`, QEMU **s'arrête** au lieu de réinitialiser le CPU : il écrit `Triple fault`, mais aucun nouveau `CPU Reset`. Le détecteur par comptage n'aurait donc jamais rien détecté. Les résultats antérieurs restent valides, car la vérification du statut (124 ou 33 attendus, 0 en cas d'arrêt) aurait échoué. Mais le détecteur dédié était inopérant. **Correction :** on recherche la chaîne `Triple fault`. **Preuve de correction :** le cas `exc_triple` exige désormais sa présence et réussit, et les 11 autres cas exigent son absence. Leçon méthodologique : **un détecteur n'est validé que lorsqu'on l'a vu détecter**.

---

## 26. Sécurité du jalon K0.4

| Aspect | K0.4 |
|---|---|
| **Actifs** | intégrité du flux de contrôle du noyau ; disponibilité du diagnostic ; état des registres du code interrompu |
| **Frontières** | toute exception est une entrée imprévue dans le noyau ; en K1, les interruptions seront le point de passage ring 3 → ring 0 |
| **Menaces** | exception ignorée laissant le noyau dans un état incohérent ; gestionnaire exécuté avec DF = 1 ou une pile mal alignée ; corruption des registres du code interrompu ; remplacement silencieux d'un gestionnaire ; pile épuisée rendant le diagnostic impossible (triple fault) ; `INT n` depuis un code non privilégié (K1) ; GDT modifiée par le CPU |
| **Invariants** | toute exception sans gestionnaire mène à `panic` ; `isr_common` sauvegarde et restaure tous les registres généraux et de segment ; DF = 0 et ESP aligné sur 16 dans tout gestionnaire C ; IF = 0 dans tout gestionnaire ; un vecteur n'a qu'un gestionnaire ; #DF s'exécute toujours sur une pile dédiée ; aucune porte DPL 3, aucun segment DPL 3 |
| **Protections** | portes d'interruption DPL 0 ; stubs uniformes ; `cld` + réalignement ; `interrupt_register` sans écrasement ; porte de tâche #DF ; bit Accessed pré-positionné ; TSS sans bitmap d'E/S |
| **Tests** | 40 vérifications noyau K0.4 (GDT, TR, IDT, trappe/faute, préservation des registres, DF, IF, alignement) ; 7 démarrages fatals : #UD, #DE, #GP (GDT), #NP, #GP (IDT, type invalide), #DF par porte de tâche, triple fault détecté |
| **Limites** | un gestionnaire enregistré est du code de confiance (pas d'isolation entre modules du noyau monolithique) ; NMI non géré spécifiquement (il produit une panique) ; #MC non initialisé ; pas de protection contre une exception pendant `isr_common` lui-même (elle mènerait à #DF, qui est capturée) ; la tâche #DF partage le CR3 initial, qu'il faudra mettre à jour en K0.6 via `gdt_set_double_fault_cr3` |

---

## 27. Questions de jury et réponses (K0.4)

**Q1. Pourquoi une GDT si vous utilisez la pagination pour protéger la mémoire ?**
En mode protégé, la segmentation ne peut pas être désactivée : chaque accès passe par un segment. Le noyau doit donc fournir des descripteurs valides et contrôlés, ceux du chargeur n'étant pas garantis. On choisit des segments plats (base 0, limite 4 Gio) qui neutralisent la segmentation. La GDT porte aussi le TSS, indispensable aux transitions de privilège et à la porte de tâche de double faute.

**Q2. Pourquoi faut-il un saut lointain après LGDT ?**
LGDT ne change que le registre GDTR. CS garde son descripteur en cache, et il ne peut être rechargé que par un transfert de contrôle lointain (`jmp`, `call` ou `ret` lointain, `iret`), qui charge CS et EIP ensemble. Les autres segments se rechargent par MOV.

**Q3. Différence entre une faute et une trappe ? Comment l'avez-vous montré ?**
Pour une faute, EIP sauvegardé pointe sur l'instruction fautive, qui sera ré-exécutée. Pour une trappe, il pointe sur la suivante. Les autotests le mesurent : après `int3`, EIP sauvegardé = adresse du label suivant ; après `ud2` et `div` par zéro, EIP pointe sur l'instruction elle-même, dont on relit les octets (`0F 0B`, `F7 F1`).

**Q4. Pourquoi tous les stubs empilent-ils un code d'erreur factice ?**
Pour qu'il n'existe qu'une seule forme de cadre et un seul `isr_common`. Sans cela, il faudrait deux structures et le code C devrait savoir lesquelles des exceptions empilent un code d'erreur, ce qui serait source d'erreurs.

**Q5. Pourquoi `cld` dans le gestionnaire ?**
Une interruption ne modifie pas DF. Si le code interrompu a exécuté `STD`, le C hériterait de DF = 1, et un `memcpy` compilé en `rep movsb` copierait à rebours, corrompant la mémoire. L'autotest le prouve : DF vaut 1 dans le cadre, 0 dans le gestionnaire, et 1 de nouveau après IRETD.

**Q6. Porte d'interruption ou de trappe ?**
Une porte d'interruption met IF à 0 à l'entrée, une porte de trappe ne touche pas IF. SAMOS utilise des portes d'interruption : aucune IRQ ne peut interrompre un gestionnaire, ce qui évite les problèmes de réentrance, notamment dans le journal.

**Q7. Qu'est-ce qu'une double faute ? Comment survivez-vous à une pile épuisée ?**
C'est une exception survenue pendant la livraison d'une autre, selon le tableau des classes contributives et des fautes de page. Si la pile est épuisée, le CPU ne peut même pas empiler le cadre de #DF. SAMOS utilise une porte de tâche : le CPU charge un TSS dédié avec sa propre pile, si bien que le gestionnaire s'exécute toujours dans un état sain. Le test `df` le démontre. En x86-64, on utilisera l'IST.

**Q8. Comment savez-vous qu'un triple fault serait détecté par vos tests ?**
Parce qu'on en a provoqué un volontairement (IDT de limite 0) et vérifié que le banc le détecte. Ce test a d'ailleurs révélé que l'ancienne méthode de détection (compter les resets) était inopérante avec `-no-reboot`. Elle a été corrigée.

**Q9. Pourquoi `INT 0x80` donne-t-il #GP et pas #NP ?**
L'entrée 128 est nulle, donc son type de porte est invalide. Le CPU vérifie le type avant la présence, et un type invalide donne #GP. Une porte de type valide mais non présente (vecteurs 32-47) donne #NP. Les deux comportements sont testés.

**Q10. Pourquoi le bit « Accessed » est-il pré-positionné ?**
Sinon le processeur écrit dans la GDT au premier chargement d'un segment (constaté : `0x92` devenait `0x93`). Une table que le matériel modifie ne peut pas être protégée en écriture. Pré-positionné, ce bit rend la table stable et protégeable.

## 28. Interruptions matérielles : PIC 8259A, PIT 8254, couche IRQ (K0.5)

### 28.1 Pourquoi des interruptions ?

Sans interruptions, le noyau doit **scruter** chaque périphérique (*polling*), comme le fait le pilote série en émission. C'est simple, mais le CPU gaspille son temps à attendre, et il manque les événements qui surviennent pendant qu'il fait autre chose. Avec les interruptions, le **périphérique** signale qu'il a besoin d'attention, et le CPU suspend ce qu'il fait pour exécuter un gestionnaire. C'est la base de tout système réactif : horloge, clavier, disque, carte son pour l'interface vocale future.

### 28.2 Le 8259A

**Architecture.** Le PC/AT utilise deux 8259A en **cascade** :

```text
 IRQ0 PIT ──┐
 IRQ1 clav. ─┤  MAÎTRE   INT ──────────► broche INTR du CPU
 IRQ2 ◄──────┤ 0x20/0x21
   ...       │               ┌── IRQ8 RTC
 IRQ7 ───────┘               ├── ...
              ▲ cascade      │ ESCLAVE
              └──────────────┤ 0xA0/0xA1
                             └── IRQ15
```

Chaque contrôleur a trois registres de 8 bits :

- **IRR** (*Interrupt Request*) : lignes qui demandent une interruption ;
- **ISR** (*In-Service*) : interruptions acquittées par le CPU mais pas encore terminées ;
- **IMR** (*Interrupt Mask*) : lignes masquées.

**Cycle d'une IRQ :**

1. la ligne passe à 1, et le bit de l'IRR est positionné ;
2. si la ligne n'est pas masquée et qu'aucune IRQ de priorité supérieure n'est en service, le PIC active INTR ;
3. si IF = 1, le CPU termine l'instruction courante et répond par deux cycles INTA ;
4. le PIC transfère le bit de l'IRR vers l'ISR et place le **numéro de vecteur** sur le bus ;
5. le CPU entre par la porte IDT correspondante ;
6. le gestionnaire **doit** envoyer un **EOI** (*End Of Interrupt*, commande `0x20`). Sinon le bit de l'ISR reste positionné, et le PIC ne délivre plus aucune IRQ de priorité égale ou inférieure.

**Le remappage (ICW1-ICW4).** Le BIOS configure le maître sur les vecteurs 8-15, qui **coïncident** avec les exceptions du mode protégé : l'IRQ 0 du timer arriverait sur le vecteur 8 (#DF) et l'IRQ 5 sur le vecteur 13 (#GP). Le noyau ne pourrait plus distinguer une IRQ d'une exception. La séquence d'initialisation de `pic_init()` :

| Mot | Maître | Esclave | Signification |
|---|---|---|---|
| ICW1 | `0x11` | `0x11` | initialisation, ICW4 attendu, cascade, déclenchement sur front |
| ICW2 | `0x20` | `0x28` | vecteur de base (multiple de 8) |
| ICW3 | `0x04` | `0x02` | maître : bitmap des esclaves (bit 2) ; esclave : son identité (2) |
| ICW4 | `0x01` | `0x01` | mode 8086, EOI manuel |
| OCW1 | `0xFF` | `0xFF` | tout masquer |

**[CHOIX] Tout masquer, puis démasquer explicitement.** `irq_register()` démasque une ligne seulement quand un gestionnaire existe. Une IRQ sans gestionnaire est impossible par construction ; le compteur `irq_unhandled_count()` le vérifie et vaut 0.

`io_wait()` (écriture sur le port `0x80`) entre les mots ICW laisse au 8259A, un composant lent sur un vrai bus ISA, le temps de traiter chaque octet.

### 28.3 Le PIT 8254 et le temps

Le PIT reçoit une horloge de $1\,193\,182$ Hz, soit $14{,}31818 \text{ MHz} / 12$. Ce quartz NTSC de l'IBM PC d'origine a été conservé pour la compatibilité. En **mode 2** (*rate generator*), le compteur 16 bits du canal 0 est rechargé avec le diviseur $D$ à chaque passage à 1, ce qui produit une IRQ 0 :

$$f = \frac{1\,193\,182}{D},\qquad D = \operatorname{round}\!\left(\frac{1\,193\,182}{100}\right) = 11\,932 \;\Rightarrow\; f = 99{,}998\ \text{Hz}$$

**Dérive due à l'arrondi.** Supposer 100 Hz exactement ferait dériver l'horloge de $2 \times 10^{-5}$, soit 1,7 s par jour. `pit_uptime_ms()` calcule donc avec le diviseur réel :

$$t_{\text{ms}} = \frac{\text{ticks} \times D \times 1000}{1\,193\,182}$$

Cette division est faite par `u64_divmod_u32` (§19.4), sans libgcc. **[CHOIX]** 100 Hz est un compromis : une résolution de 10 ms, suffisante pour K0, contre un coût d'environ 100 interruptions par seconde.

**Étalonnage indépendant.** Le test mesure le nombre de ticks entre deux changements de seconde de l'**horloge CMOS** (RTC MC146818, ports `0x70/0x71`), une source de temps physiquement distincte. Résultat observé : **100 ticks par seconde**. Sous QEMU, PIT et RTC dérivent tous deux de l'horloge virtuelle, ce qui rend la mesure stable même sur un hôte chargé. Sur matériel réel, l'écart serait mesuré entre deux quartz différents.

### 28.4 Lecture « déchirée » d'un compteur 64 bits

Le compteur de ticks est un `uint64_t`. Sur i386, sa lecture prend **deux** instructions 32 bits. Si l'IRQ 0 survient entre les deux, au moment où la moitié basse passe de `0xFFFFFFFF` à 0, on peut lire « ancienne moitié haute + nouvelle moitié basse », soit une valeur fausse de $2^{32}$. `pit_ticks()` lit donc sous `irq_save()/irq_restore()`. Ces fonctions sauvegardent EFLAGS et ne réactivent IF que s'il était actif, ce qui permet l'imbrication. Le gestionnaire, lui, incrémente avec IF = 0 (porte d'interruption) : son écriture n'est jamais interrompue sur un monoprocesseur.

### 28.5 La boucle d'inactivité sans réveil perdu

Le code naïf contient une **course** :

```c
if (file_vide()) {        // (1) vide
                          // (2) ← l'IRQ clavier arrive ICI et dépose une touche
    sti(); hlt();         // (3) le CPU dort alors qu'une touche attend
}
```

Le CPU ne se réveillera qu'à la prochaine interruption, jusqu'à 10 ms plus tard (timer), voire jamais si aucune autre IRQ n'arrive. **Solution :** tester avec IF = 0, puis exécuter `sti; hlt`. **[ARCH]** `STI` n'autorise les interruptions qu'**après l'instruction suivante** (*interrupt shadow*, SDM vol. 2, STI). Aucune interruption ne peut donc être livrée entre `STI` et `HLT`. Une IRQ arrivée pendant le test reste **en attente** au PIC et réveille `HLT` immédiatement. C'est `cpu_idle()`.

La première version de `idle_loop()` contenait justement une variante de cette course (`sti` après le test, puis `cli; sti; hlt`). Elle a été détectée à la relecture et corrigée avant toute exécution.

### 28.6 IRQ parasites et INT logiciel

**IRQ parasite.** Si une ligne retombe pendant le cycle INTA, le 8259A délivre quand même un vecteur, celui de sa ligne de plus basse priorité (7 ou 15), **sans** positionner le bit correspondant dans l'ISR. Envoyer un EOI acquitterait alors à tort une **autre** IRQ réellement en service. `irq_dispatch` lit l'ISR (OCW3 `0x0B`) pour les IRQ 7 et 15, et ignore l'interruption si le bit est absent. Pour une IRQ 15 parasite, le maître a bien vu une IRQ 2 réelle et reçoit un EOI. **Statut : NOT TESTED.** Le chemin ne peut pas être déclenché de façon déterministe sous QEMU. L'autotest vérifie seulement que le compteur vaut 0 en fonctionnement normal.

**INT logiciel vers un vecteur d'IRQ.** Constat de K0.5 : les tests K0.4 utilisaient `INT 0x20` pour provoquer #NP. Depuis K0.5, le vecteur 0x20 est l'IRQ 0. Un `INT 0x20` logiciel exécute alors le gestionnaire du timer (un tick fantôme) et envoie au PIC un **EOI que personne n'attendait**, susceptible d'acquitter une IRQ réellement en service. En K0, seul le noyau exécute `INT`. En K1, le DPL 0 de ces portes fera échouer un tel `INT` depuis le ring 3 avec #GP. C'est une raison concrète de ne jamais donner DPL 3 aux portes d'IRQ. Les tests ont été déplacés sur le vecteur 0x2E (IRQ 14, inutilisée).

---

## 29. Concurrence entre interruption et code normal : file SPSC (K0.5)

### 29.1 Le problème

Le gestionnaire d'IRQ 1 et le code normal partagent les touches reçues. Un gestionnaire d'interruption **ne peut pas attendre** : il ne doit ni prendre un verrou détenu par le code qu'il interrompt (sur un monoprocesseur, ce serait un interblocage immédiat), ni bloquer sur une file pleine.

### 29.2 La file circulaire à indices libres

**Structure :** un tableau de capacité $C = 2^k$ et deux compteurs 32 bits `head` (écritures) et `tail` (lectures), **jamais ramenés modulo $C$**. Pour un élément d'indice $i$, la case est $i \mathbin{\&} (C-1)$, ce qui vaut $i \bmod C$ quand $C$ est une puissance de 2.

**Nombre d'éléments :** $n = (\text{head} - \text{tail}) \bmod 2^{32}$. L'égalité reste vraie après le débordement des compteurs, car la soustraction non signée est définie modulo $2^{32}$ (C11 §6.2.5 p9). Si $C$ divise $2^{32}$, ce qui est le cas de toute puissance de 2 inférieure à $2^{32}$, l'indexation reste cohérente au passage de $2^{32} - 1$ à 0.

**Avantage sur la version « une case vide » :** vide ($n = 0$) et plein ($n = C$) se distinguent sans sacrifier de case.

### 29.3 Pourquoi pas de verrou ?

**Règle de propriété :**

- seul le **producteur** (l'ISR) écrit `head` et les cases `[tail + n …]` ;
- seul le **consommateur** écrit `tail`.

Chaque variable n'a donc qu'un seul écrivain : il n'y a pas d'écriture concurrente, seulement des lectures concurrentes d'une valeur qui ne fait qu'augmenter.

**Ordre des accès.** Le producteur doit écrire la donnée **avant** de publier `head + 1`, sinon le consommateur pourrait lire une case pas encore remplie. Deux contraintes s'appliquent :

1. le **compilateur** ne doit pas réordonner ces écritures : c'est le rôle de la barrière `asm volatile("" ::: "memory")` et de `volatile` ;
2. le **processeur** : x86 ne réordonne pas deux écritures entre elles (modèle TSO, SDM vol. 3A §9.2). De plus l'ISR et le consommateur s'exécutent sur le **même cœur**. Aucune instruction de barrière matérielle (`mfence`) n'est nécessaire.

**Limite :** sur un multiprocesseur, ou sur une architecture à mémoire faiblement ordonnée (ARM), il faudrait des opérations atomiques C11 (`atomic_store_explicit(..., memory_order_release)`).

### 29.4 Politique en cas de file pleine

**[CHOIX]** Le producteur **abandonne** l'octet et incrémente `dropped`. Il ne bloque jamais. 256 octets représentent environ 128 frappes en attente. Une perte est visible : le test de bout en bout exige `dropped=0`.

### 29.5 Vérification par modèle

Le test hébergé applique **un million** d'opérations aléatoires à la file SAMOS et à un modèle de référence trivial, et compare à chaque pas le résultat de chaque opération, la valeur lue et le nombre d'éléments. Les compteurs démarrent à $2^{32} - 5$ pour **exercer le débordement**. Le test vérifie à la fin que `head` a bien franchi $2^{32}$. Résultat : 0 divergence, sous ASan et UBSan.

---

## 30. Contrôleur PS/2 8042 et décodage du clavier (K0.5)

### 30.1 Le 8042

Le « contrôleur clavier » de l'IBM AT est un micro-contrôleur Intel 8042. Il dialogue en série avec le clavier, et parfois la souris, et expose au CPU deux ports :

- `0x60` (données) ;
- `0x64` (état en lecture, commandes en écriture).

Bits d'état :

- **OBF** (bit 0) : un octet attend le CPU ;
- **IBF** (bit 1) : le contrôleur n'a pas encore consommé le dernier octet écrit.

**Toute écriture doit attendre IBF = 0, et toute lecture OBF = 1.** Chaque attente est **bornée** : un contrôleur absent, fréquent sur les machines modernes sans PS/2 réel, ne doit pas figer le noyau.

### 30.2 Séquence d'initialisation de `keyboard_init()`

| # | Action | Vérification | Raison |
|---|---|---|---|
| 1 | `0xAD`, `0xA7` | — | désactiver les ports pendant la configuration |
| 2 | vider le tampon (≤ 64 lectures) | — | octets résiduels laissés par le BIOS |
| 3 | `0x20` lire la config ; effacer IRQ1/IRQ12 ; activer la traduction (bit 6) ; couper l'horloge du port 2 ; `0x60` écrire | — | pas d'IRQ pendant l'initialisation |
| 4 | `0xAA` auto-test du contrôleur | réponse `0x55` | puis réécrire la config, que certains contrôleurs réinitialisent |
| 5 | `0xAB` test de l'interface du port 1 | réponse `0x00` | lignes horloge et données |
| 6 | `0xAE` activer le port 1 ; `0xFF` réinitialiser le clavier | `0xFA` (ACK) puis `0xAA` (BAT réussi) | auto-test du clavier lui-même |
| 7 | `0xF4` activer le balayage ; réécrire la config avec IRQ1 | `0xFA` | début des interruptions |

En cas d'échec, `keyboard_error()` indique l'étape. **[CHOIX]** L'absence de clavier n'est **pas** fatale : le noyau reste utilisable par le port série.

**Défaut réel rencontré et corrigé.** La première version recopiait à l'étape 7 l'octet de configuration lu à l'étape 3. Or la commande `0xAD` de l'étape 1 avait positionné le **bit 4** (horloge du port 1 coupée), et la réécriture annulait l'activation de l'étape 6. Symptôme : initialisation réussie, mais aucune frappe reçue (`bytes=0 irq1=0`). Correction : le bit 4 est effacé explicitement. Leçon : une copie locale d'un registre matériel devient **périmée** dès qu'une commande modifie ce registre.

### 30.3 Codes de balayage

Le clavier n'envoie pas des caractères mais des **codes de position** :

- un code *make* à l'appui ;
- un code *break* au relâchement ;
- la répétition automatique renvoie le *make*.

Le jeu 2, natif des claviers AT, est traduit par le 8042 en **jeu 1** (celui du PC/XT) quand le bit 6 de la configuration est positionné. En jeu 1 :

$$\text{break} = \text{make} \mathbin{|} \texttt{0x80}$$

La conversion en caractère se fait **logiciellement**, selon la disposition du clavier et l'état des modificateurs. C'est pourquoi la même touche donne « q » en QWERTY et « a » en AZERTY.

### 30.4 L'automate de décodage

États : `extended` (préfixe `0xE0` reçu), `skip` (octets de la séquence Pause à ignorer), et les modificateurs (Shift G/D, Ctrl G/D, Alt G/D, Verr. Maj).

```text
            0xE0                     octet c
  [normal] ──────► [étendu] ───────────────────► événement (0x100 | c&0x7F)
     │  0xE1                                      retour à [normal]
     └──────► [skip = 2] ── 2 octets ignorés ───► [normal]
     │  c
     └──────► événement (c & 0x7F), appui si c < 0x80
```

Règles notables, toutes testées :

- **Verr. Maj** ne bascule qu'à l'**appui**, pas au relâchement ni à la répétition, et n'affecte que les lettres : Verr. Maj + `1` donne `1`, et Shift + Verr. Maj + `a` donne `a` ;
- **Ctrl + lettre** produit un caractère de contrôle : Ctrl+C donne `0x03` (ETX), base du futur « interrompre la commande » ;
- **Shift fantôme** : certains claviers entourent les touches étendues de `E0 2A … E0 AA`. Ce n'est pas un vrai Shift, et il est ignoré ;
- **Octets de contrôle** (`0xFA` ACK, `0xFE` RESEND, `0xEE`, `0x00`/`0xFF` erreur) : ignorés. Ils ne coïncident avec aucun *break* valide du jeu 1.

**[CHOIX] Séparation des responsabilités.** L'ISR ne fait que lire le port et déposer l'octet. Le décodage, qui implique un état, des tables et des branchements, s'exécute hors interruption. L'automate est une fonction **pure**, testée sur l'hôte, notamment sur les 65 536 paires d'octets possibles sous ASan/UBSan, ce qui serait impossible dans le noyau.

### 30.5 Test de bout en bout

`test=kbd` combiné au moniteur QEMU `sendkey` : le script injecte `shift-s a m o s spc shift-k 0 dot 5 spc shift-1 ret`. QEMU les présente comme de vraies frappes au clavier PS/2 émulé, qui les transmet au 8042, qui lève l'IRQ 1 ; l'ISR les dépose dans la file, et le décodeur les traduit. Résultat :

```text
[TEST] kbd line: 'Samos K0.5 !' (bytes=31 dropped=0 irq1=31)
```

On a exactement une IRQ par octet, et aucune perte. Les 16 frappes, Shift compris, produisent 32 octets *make/break*. Le 32ᵉ, le relâchement d'Entrée, arrive après l'affichage de la ligne.

**Limite :** disposition US QWERTY uniquement. Un clavier AZERTY physique produira donc des caractères décalés. L'ajout d'une table ne change pas l'automate.

---

## 31. Sécurité du jalon K0.5

| Aspect | K0.5 |
|---|---|
| **Actifs** | cohérence de l'état du noyau face à des événements asynchrones ; disponibilité (le noyau ne doit pas se figer) ; intégrité des données clavier (futures commandes) |
| **Frontières** | périphériques → noyau : le PIT, le 8042 et le clavier produisent des événements asynchrones et des données non fiables (scancodes) |
| **Menaces** | IRQ confondue avec une exception (vecteurs non remappés) ; EOI oublié (plus aucune IRQ) ou parasite (IRQ acquittée à tort) ; tempête d'interruptions ; lecture déchirée de compteurs partagés ; réveil perdu ; file pleine ; contrôleur absent figeant l'initialisation ; séquences de scancodes malformées corrompant l'état du décodeur ; INT logiciel vers un vecteur d'IRQ |
| **Invariants** | vecteurs d'IRQ disjoints des exceptions ; toute ligne démasquée a un gestionnaire ; un EOI par IRQ réelle, aucun pour une IRQ parasite ; gestionnaires d'IRQ bornés, sans journal, sans attente ; un seul écrivain par variable de la file ; toute attente matérielle bornée ; le décodeur accepte n'importe quel octet dans n'importe quel état |
| **Protections** | remappage 0x20-0x2F ; masquage par défaut ; EOI centralisé dans `irq_dispatch` ; détection des IRQ parasites (ISR) ; `irq_save/irq_restore` ; `cpu_idle` (`sti; hlt`) ; file SPSC à perte comptée ; attentes bornées du 8042 ; automate total (tout octet traité) ; portes d'IRQ DPL 0 |
| **Tests** | 36 vérifications noyau K0.5 : masques, portes, EOI (ticks continus), étalonnage RTC, `sleep`, masquage effectif, refus de dormir avec IF = 0 ; 1 million d'opérations sur la file vs modèle, débordement inclus ; décodeur : 21 vérifications et 65 536 paires d'octets sous ASan/UBSan ; clavier de bout en bout avec de vraies frappes |
| **Limites** | IRQ parasites : NOT TESTED ; pas de protection contre une tempête d'IRQ (aucune limitation de débit) ; ordonnancement mémoire valable seulement sur un monoprocesseur x86 ; le PIC est obsolète sur les machines modernes (APIC/IOAPIC, MSI), il faudra un pilote APIC pour le multiprocesseur et x86-64 ; scancodes non authentifiés : un clavier USB émulé en PS/2 par le firmware (SMM) n'est pas sous le contrôle du noyau |

---

## 32. Questions de jury et réponses (K0.5)

**Q1. Pourquoi remapper le PIC ?**
Le BIOS place les IRQ 0-7 sur les vecteurs 8-15, réservés aux exceptions en mode protégé. Sans remappage, chaque tick du timer arriverait sur le vecteur 8 et serait pris pour une double faute. On déplace les IRQ sur 0x20-0x2F.

**Q2. Que se passe-t-il si on oublie l'EOI ?**
Le bit de l'ISR du PIC reste positionné : le PIC considère l'IRQ comme toujours en service et ne délivre plus aucune IRQ de priorité égale ou inférieure. Le système ne reçoit plus le timer. L'autotest qui exige 10 ticks consécutifs le prouve : sans EOI, il n'y en aurait qu'un.

**Q3. Pourquoi 11 932 et pas 11 931,82 ?**
Le diviseur est un entier de 16 bits. L'arrondi donne une fréquence réelle de 99,998 Hz. Le calcul du temps écoulé utilise le diviseur réel, ce qui évite une dérive de 1,7 s par jour.

**Q4. Comment savez-vous que votre timer bat à 100 Hz ?**
Par mesure contre une horloge indépendante, la RTC CMOS : on compte les ticks entre deux changements de seconde. Résultat : 100.

**Q5. Pourquoi masquer les interruptions pour lire un compteur ?**
Sur 32 bits, la lecture d'un entier 64 bits prend deux instructions. Une interruption entre les deux peut produire une valeur incohérente, fausse de $2^{32}$, lors d'une retenue entre les deux moitiés.

**Q6. Expliquez le problème du « réveil perdu » et sa solution.**
Si on teste « rien à faire » avec les interruptions actives, puis qu'on exécute `hlt`, une interruption arrivée entre les deux est traitée avant `hlt` et le CPU dort alors qu'il y a du travail. On teste donc avec IF = 0, puis on exécute `sti; hlt`. Grâce à l'*interrupt shadow* de STI, l'interruption en attente est délivrée **après** l'entrée dans HLT, qu'elle réveille.

**Q7. Votre file circulaire est sans verrou : pourquoi est-ce correct ?**
Chaque variable n'a qu'un écrivain (head : l'ISR ; tail : le consommateur). La donnée est écrite avant la publication de `head`, avec une barrière de compilation, et x86 ne réordonne pas deux écritures. Sur le même cœur, cela suffit. Sur un multiprocesseur, il faudrait des atomiques avec une sémantique *release/acquire*.

**Q8. Pourquoi ne pas décoder les touches dans l'ISR ?**
L'ISR s'exécute avec les interruptions masquées : plus il est long, plus la latence de toutes les autres IRQ augmente. Le décodage implique un état et des tables. Hors interruption, il est aussi testable sur l'hôte. C'est la séparation classique en « moitié haute / moitié basse » (*top half / bottom half*) des noyaux.

**Q9. Comment avez-vous testé le clavier sans clavier ?**
Avec le moniteur QEMU (`sendkey`), qui injecte des frappes dans le contrôleur PS/2 émulé. Le chemin complet est exercé : contrôleur, IRQ 1, ISR, file, décodeur. On vérifie la ligne obtenue (`'Samos K0.5 !'`), qu'il y a une IRQ par octet et qu'aucun octet n'est perdu.

**Q10. Quel bogue avez-vous rencontré avec le 8042 ?**
La réécriture de l'octet de configuration à partir d'une copie lue avant la commande d'activation du port recoupait l'horloge du clavier. L'initialisation semblait réussie, mais aucune touche n'arrivait. Le diagnostic est venu des compteurs (`bytes=0 irq1=0`) et de la relecture de la sémantique du bit 4.

## 33. La mémoire physique et sa carte (K0.6)

### 33.1 Mémoire physique, virtuelle, et allocation dynamique

Trois notions à ne pas confondre :

| Notion | Unité | Gérée par | Question à laquelle elle répond |
|---|---|---|---|
| **Mémoire physique** | cadre (*frame*) de 4 Kio | allocateur physique (PMM) | quelles cellules de RAM sont libres ? |
| **Mémoire virtuelle** | page de 4 Kio | pagination (tables de pages, MMU) | à quelle adresse physique correspond cette adresse ? avec quels droits ? |
| **Allocation dynamique du noyau** | octet (`kmalloc`) | tas du noyau (*heap*) | où placer cet objet de 48 octets ? |

K0.6 implémente les **deux premières**. Le tas du noyau (`kmalloc`), construit au-dessus des deux, n'est **pas** implémenté. C'est une décision de périmètre : il n'est utile qu'avec des objets de durée de vie dynamique, comme les processus de K1.

### 33.2 D'où vient la carte mémoire ?

La RAM d'un PC n'est pas contiguë. Le premier Mio contient des trous historiques (VGA à `0xA0000`, ROM à `0xC0000`-`0xFFFFF`), le firmware réserve des zones pour l'ACPI et le SMM, et les périphériques PCI occupent une fenêtre sous 4 Gio (le *PCI hole*). Seul le firmware connaît cette disposition. Il l'expose par l'appel BIOS **`INT 15h, EAX = E820h`**, ou par `GetMemoryMap()` en UEFI. Le chargeur Multiboot recopie le résultat dans `mmap_addr/mmap_length`.

Carte réelle observée sous QEMU avec `-m 128M` :

```text
0x000000000-0x00009fbff available        639 Kio de mémoire conventionnelle
0x00009fc00-0x00009ffff reserved         EBDA (Extended BIOS Data Area)
0x0000f0000-0x0000fffff reserved         ROM du BIOS
0x000100000-0x007fdffff available        mémoire étendue
0x007fe0000-0x007ffffff reserved         tables ACPI de QEMU
0x0fffc0000-0x0ffffffff reserved         flash du firmware (vecteur de reset)
```

Avec `-m 4G`, QEMU place une partie de la RAM **au-dessus de 4 Gio** (`0x100000000-0x13fffffff`), car la fenêtre PCI occupe `0xC0000000-0xFFFFFFFF`. Un noyau 32 bits sans PAE ne peut pas l'adresser : elle est conservée dans la carte mais ignorée par l'allocateur.

### 33.3 Analyse défensive

**[ARCH]** Format d'un élément : `size` (4 octets) suivi de `base_addr` (8), `length` (8) et `type` (4). Le champ `size` **n'inclut pas** ses propres 4 octets, et peut valoir plus que 20 : l'élément suivant se trouve à `adresse + size + 4`.

**Menaces :** la carte vient du firmware et du chargeur, donc de l'extérieur du noyau. `memmap_parse()` est écrite pour qu'**aucune** carte, même hostile, ne provoque une lecture hors du tampon :

| Cas | Comportement |
|---|---|
| `size < 20` | arrêt : l'élément suivant ne peut pas être localisé de façon sûre |
| `size > len − off − 4` (élément qui dépasse le tampon, y compris `size = 0xFFFFFFFF`) | arrêt, sans calculer `off + size`, qui déborderait |
| 1 à 3 octets résiduels | signalé comme malformé |
| `length = 0` | ignoré et compté |
| $\text{base} + \text{length} > 2^{64}$ | ignoré et compté (test sans débordement : $\text{length} - 1 > 2^{64} - 1 - \text{base}$) |
| plus de 64 éléments | surplus compté, jamais écrit hors du tableau |
| éléments non triés | tri par insertion (stable, $n \le 64$) |
| lectures non alignées | `memcpy` vers des variables locales |

**Vérification :** 10 scénarios ciblés et **20 000 cartes aléatoires**, chacune dans un tampon alloué à sa taille exacte sous AddressSanitizer. La moindre lecture d'un octet au-delà ferait échouer le test.

**Repli :** sans carte détaillée (bit 6 absent), `memmap_from_basic()` construit deux régions à partir de `mem_lower` et `mem_upper`.

---

## 34. Allocateur de cadres physiques par bitmap (K0.6)

### 34.1 Choix de la structure

| Structure | Allocation | Libération | Mémoire | Contiguïté |
|---|---|---|---|---|
| **Bitmap** (SAMOS) | $O(n/32)$ au pire | $O(1)$ (+ vérification des réserves) | 1 bit/cadre (128 Kio pour 4 Gio) | recherche possible |
| Pile de cadres libres | $O(1)$ | $O(1)$ | 4 octets/cadre libre | non |
| Liste chaînée intrusive | $O(1)$ | $O(1)$ | 0 (dans les cadres libres) | non ; exige d'accéder aux cadres libres |
| Buddy allocator (Linux) | $O(\log n)$ | $O(\log n)$ | ~ 1 bit/cadre/niveau | oui, par puissances de 2 |

**[CHOIX]** La bitmap : la plus simple à prouver correcte, et elle permet de **détecter une double libération** (le bit est déjà à 0). Ce n'est pas possible avec une pile ou une liste, qui accepteraient d'empiler deux fois le même cadre et le distribueraient ensuite à deux propriétaires. Le *buddy allocator* sera pertinent quand le noyau aura besoin de blocs physiques contigus (tampons DMA).

**Recherche *next-fit* par mots.** On parcourt la bitmap par mots de 32 bits à partir de la dernière position utilisée. Un mot qui vaut `0xFFFFFFFF` représente 32 cadres occupés, sautés en une comparaison. Coût mesuré : l'épuisement complet de 32 393 cadres, remise à zéro comprise, prend environ 710 ms sous QEMU (émulation TCG), un temps dominé par la remise à zéro de 129 Mio.

### 34.2 Construction de la bitmap

Ordre des opérations, qui garantit que les réservations l'emportent :

1. **tout marquer occupé** ;
2. **libérer** chaque cadre **entièrement contenu** dans une région *available* (arrondi **vers l'intérieur**) ;
3. **réoccuper et enregistrer comme réservé** chaque cadre **touché**, même partiellement, par une région d'un autre type (arrondi **vers l'extérieur**) ;
4. réserver le cadre 0, la mémoire basse (< 1 Mio), l'image du noyau (bitmap comprise) et les structures Multiboot.

**Pourquoi deux arrondis opposés ?** Une région disponible `[0x900800, 0x903800)` ne contient que les cadres `0x901` et `0x902` entiers. Le cadre `0x900` est à moitié hors de la région, et le distribuer reviendrait à utiliser 2 Kio non garantis. Inversement, une zone réservée qui touche un cadre, ne serait-ce que d'un octet, rend tout ce cadre inutilisable. Les deux cas sont testés.

**Formules (cadres de $2^{12}$ octets) :**

$$\text{intérieur : } \left[\left\lceil \tfrac{b}{4096} \right\rceil,\ \left\lfloor \tfrac{b + \ell}{4096} \right\rfloor\right) \qquad \text{extérieur : } \left[\left\lfloor \tfrac{b}{4096} \right\rfloor,\ \left\lfloor \tfrac{b + \ell - 1}{4096} \right\rfloor + 1\right)$$

Elles sont calculées en évitant tout débordement 64 bits : $b + \ell - 1$ (dernier octet) plutôt que $b + \ell$, et le cas $\text{last} = 2^{64} - 1$ est traité à part.

**Pourquoi réserver tout le premier Mio ?** Il contient l'IVT, la BDA, l'EBDA, la mémoire vidéo et les ROM, et sous QEMU aussi la structure Multiboot (`0x9500`). **[CHOIX]** C'est une approche prudente, au prix d'environ 600 Kio. Un futur pilote ACPI ou un trampoline de démarrage SMP pourra en réclamer une partie explicitement.

### 34.3 Invariants et leur vérification

| Invariant | Mécanisme | Test |
|---|---|---|
| **I1** un cadre réservé n'est jamais alloué ni libérable | réservations après les libérations ; `pmm_core_free` consulte la liste des réserves | épuisement : aucun cadre réservé ; libérer le noyau, la VGA ou le cadre 0 est refusé |
| **I2** jamais deux allocations sans libération | le bit passe à 1 dans `pmm_core_alloc` | épuisement : tous les cadres distincts |
| **I3** double libération détectée | bit déjà à 0, donc `SAMOS_ENOENT` et compteur | test noyau et test hébergé |
| **I4** `free_count` = nombre de 0 | toute modification passe par `mark_range`/`alloc`/`free` | comparaison avec un comptage complet après 200 000 opérations aléatoires |

**Remise à zéro à l'allocation.** Un cadre libéré peut contenir les données de son ancien propriétaire (fuite d'information entre futurs processus) ou d'anciennes entrées de table de pages (projections fantômes). `pmm_alloc_frame()` le remet donc à zéro. Le test salit un cadre, le libère, le réalloue (même cadre, par next-fit) et vérifie qu'il est nul.

**[CHOIX] Bitmap statique** de 128 Kio dans `.bss`. Elle couvre les $2^{20}$ cadres de 4 Gio et évite la question de l'œuf et de la poule (avec quoi allouer la structure de l'allocateur ?). Son coût est fixe.

---

## 35. Pagination 32 bits : traduction, droits, TLB (K0.6)

### 35.1 Traduction d'adresse

**[ARCH]** (SDM vol. 3A §4.3) Avec CR0.PG = 1, CR4.PAE = 0 et des pages de 4 Kio, une adresse linéaire de 32 bits se décompose ainsi :

```text
 31        22 21        12 11           0
┌────────────┬────────────┬──────────────┐
│ index PDE  │ index PTE  │   décalage   │
│  (10 bits) │  (10 bits) │   (12 bits)  │
└────────────┴────────────┴──────────────┘
CR3 ──► répertoire[1024] ──► table[1024] ──► cadre + décalage = adresse physique
```

$$\text{phys} = \text{PTE}\big[\text{PDE}[\text{CR3}][va_{31..22}]\big][va_{21..12}]_{31..12} \;\|\; va_{11..0}$$

On a $2^{10} \times 2^{10} \times 2^{12} = 2^{32}$ : un répertoire couvre 4 Gio, une table couvre 4 Mio. Exemple vérifié par autotest : l'adresse virtuelle `0xD0000123`, projetée sur le cadre $F$, se traduit en $F + \texttt{0x123}$.

**Format d'une entrée** (PDE ou PTE, 32 bits) : bits 31-12 = numéro de cadre ; bit 0 **P** (présent) ; bit 1 **R/W** ; bit 2 **U/S** ; bits 3-4 PWT/PCD (cache) ; bit 5 **A** (accédé) ; bit 6 **D** (modifié, PTE seulement) ; bit 7 PS (PDE : page de 4 Mio).

**Droits effectifs** (SDM §4.6) : l'accès est permis seulement si le **PDE et le PTE** le permettent tous les deux (intersection). **[CHOIX]** Les PDE de SAMOS sont permissifs (P | W), et c'est le PTE qui décide, à la granularité de 4 Kio.

### 35.2 L'espace d'adressage du noyau K0

```text
0x00000000 ┌───────────────┐ NON PROJETÉE (page nulle)
0x00001000 ├───────────────┤ RW   mémoire basse (BIOS, VGA 0xB8000)
0x00100000 ├───────────────┤ R    .text                    (kernel image)
           ├───────────────┤ R    .rodata
           ├───────────────┤ RW   .data, .bss (bitmap PMM, GDT, IDT, TSS)
           ├───────────────┤ NON PROJETÉE (page de garde)
           ├───────────────┤ RW   pile de démarrage
           ├───────────────┤ RW   RAM gérée (cadres, tables de pages)
idmap_top  ├───────────────┤
           │      ...      │ NON PROJETÉ
0xFFFFFFFF └───────────────┘
```

Tout est projeté **en identité** (virtuel = physique). L'activation de la pagination ne change donc l'adresse d'aucune instruction ni d'aucune donnée : l'instruction qui suit `mov cr0` s'exécute à la même adresse. **[CHOIX]** C'est le plus simple pour K0 (ADR 0001, décision 6). En contrepartie, le noyau et les futurs programmes devraient partager les mêmes adresses basses. Un noyau *higher-half* sera nécessaire pour la séparation noyau/utilisateur (voir la revue K0, `docs/review/K0-review.md`).

**Coût :** 1 table de pages pour 4 Mio, soit 32 tables (128 Kio) pour 128 Mio et 256 tables (1 Mio) pour la limite de 1 Gio.

### 35.3 CR0.WP : le noyau aussi doit respecter la lecture seule

**[ARCH]** Par défaut (CR0.WP = 0), le ring 0 **ignore** le bit R/W : le noyau peut écrire dans une page en lecture seule. Avec WP = 1 (i486 et suivants), la lecture seule s'applique aussi au superviseur. SAMOS active PG et WP ensemble. Sans WP, toutes les protections de `.text` et `.rodata` seraient décoratives. Les tests `wtext`/`wrodata` le démontrent : une écriture dans `.text` produit #PF avec le code `0x3` (P = 1, W = 1).

### 35.4 Pas de NX en i386 sans PAE

**[ARCH]** La pagination 32 bits classique n'a **aucun bit d'interdiction d'exécution** : toute page présente est exécutable. Le bit XD/NX (bit 63) n'existe qu'avec PAE (entrées de 64 bits) ou en mode long. **Conséquence :** les données du noyau (pile, `.data`, RAM) sont exécutables. Un attaquant capable d'écrire dans la pile pourrait y exécuter du code. Le W^X n'est donc réalisé qu'à **moitié** : le code n'est pas inscriptible, mais les données sont exécutables. C'est l'un des arguments majeurs pour PAE ou x86-64 (revue K0).

### 35.5 Le TLB et INVLPG

Le **TLB** (*Translation Lookaside Buffer*) est un cache des traductions récentes. Sans lui, chaque accès mémoire en coûterait trois (PDE, PTE, donnée). **[ARCH]** Le TLB n'est **pas** cohérent avec les tables en mémoire : modifier un PTE ne met pas à jour une traduction déjà en cache. Après avoir **retiré** ou **restreint** une projection, il faut invalider l'entrée par `INVLPG adresse`, ou recharger CR3, ce qui vide tout le TLB sauf les pages globales.

**Démonstration expérimentale :** le test projette `0xD0000000`, y écrit (la traduction entre dans le TLB), retire la projection avec `vmm_unmap`, qui exécute `INVLPG`, puis relit par une sonde. Il exige une faute. Sans `INVLPG`, la lecture pourrait réussir grâce à l'entrée périmée. Avec QEMU, le TLB est émulé, mais la règle architecturale est la même.

### 35.6 Activation

Ordre dans `paging_init()` :

1. allouer le répertoire (cadre remis à zéro : 1024 PDE non présents) ;
2. projeter page par page toute la RAM gérée avec ses droits, en allouant les tables au besoin ;
3. enregistrer le gestionnaire de #PF ;
4. donner le même CR3 à la tâche de double faute, **avant** l'activation. Une #DF survenant après la pagination chargerait sinon CR3 = 0 et ne verrait aucune table, d'où un triple fault ;
5. `mov cr3, pd` ;
6. `CR0 |= PG | WP`.

Les tables sont construites **avant** l'activation, quand les adresses physiques sont directement utilisables. Après, elles le restent grâce à l'identité.

---

## 36. Fautes de page, sondes et table de correction d'exceptions (K0.6)

### 36.1 La faute de page (#PF, vecteur 14)

**[ARCH]** Quand une traduction échoue (entrée non présente) ou viole les droits, le CPU lève #PF, **place l'adresse fautive dans CR2** et empile un code d'erreur :

| Bit | Nom | 0 | 1 |
|---|---|---|---|
| 0 | P | page non présente | violation de protection |
| 1 | W/R | lecture | écriture |
| 2 | U/S | superviseur | utilisateur |
| 3 | RSVD | — | bit réservé positionné dans une entrée |
| 4 | I/D | — | lecture d'instruction (avec NX) |

C'est une **faute** : EIP pointe sur l'instruction fautive, qui sera ré-exécutée si le gestionnaire corrige la cause. C'est le fondement de la mémoire virtuelle à la demande (*demand paging*), de la copie sur écriture (*copy-on-write*) et du *swap*. **En K0, toute #PF est une erreur du noyau.** Le gestionnaire produit un diagnostic contextuel (`paging_classify`) puis panique :

| Condition | Diagnostic |
|---|---|
| CR2 < 4096 | `NULL pointer dereference` |
| CR2 dans la page de garde | `kernel stack overflow` |
| P = 1, W = 1, CR2 dans `.text` / `.rodata` | `write to read-only kernel code / data` |
| P = 0, CR2 ≥ fin de l'identité | `access to an unmapped address` |

### 36.2 Le débordement de pile : #PF, puis #DF

Récursion infinie (`test=stack`), observée :

1. un `push` écrit sous `boot_stack_bottom`, dans la page de garde non présente : **#PF** ;
2. pour livrer cette #PF, le CPU doit **empiler** EFLAGS, CS, EIP et le code d'erreur… sur la pile, c'est-à-dire dans la page de garde : **seconde #PF pendant la livraison d'une #PF**, donc **#DF** (tableau du §25.1) ;
3. la porte de tâche de #DF (K0.4) bascule sur un TSS dont la pile est valide et dont le CR3 est le bon ;
4. `double_fault_task` lit CR2 = `0x00131ffc` (le **dernier mot** de la page de garde, exactement là où le CPU a tenté d'empiler) et diagnostique `kernel stack overflow`.

**Sans la porte de tâche de K0.4, ce scénario finissait en triple fault** et en redémarrage muet. Les jalons K0.2 (page de garde), K0.4 (TSS de #DF) et K0.6 (pagination) n'ont de valeur de sécurité **qu'ensemble**. Le test le vérifie de bout en bout, y compris l'**absence** de triple fault.

### 36.3 Tester une protection sans paniquer : la table de correction

Pour vérifier dans **un seul** démarrage que la page nulle, `.text`, `.rodata`, la garde et la mémoire non projetée sont protégées, il faut provoquer une faute **et reprendre l'exécution**. Problème : le gestionnaire ne connaît pas la longueur de l'instruction fautive, qu'on ne peut donc pas simplement « sauter ».

**Solution (principe de la table `__ex_table` du noyau Linux, utilisée par `copy_from_user`) :**

1. Les seuls accès autorisés à fauter sont deux instructions **connues**, écrites en assembleur et dont l'adresse est exportée : `mem_probe_read_insn: mov edx, [ecx]` et `mem_probe_write_insn: mov [ecx], edx`.
2. Si une #PF survient avec EIP égal à l'une de ces adresses, le gestionnaire mémorise CR2 et le code d'erreur, puis **remplace EIP** par `mem_probe_fixup` (`mov eax, -1 ; ret`).
3. L'instruction fautive n'a pas modifié ESP (une faute est précise : aucun effet partiel). Le `ret` de la correction revient donc normalement à l'appelant C, qui reçoit −1.

Toute autre #PF reste fatale : la correction est strictement limitée à des adresses **déclarées**. On peut ainsi vérifier non seulement qu'un accès est refusé, mais aussi **pourquoi**, avec CR2 et le code d'erreur exacts :

| Accès sondé | Résultat attendu (vérifié) |
|---|---|
| lecture de `kmain` (`.text`) | succès |
| écriture dans une variable de `.data` | succès, valeur modifiée |
| lecture de `0x0` | faute, CR2 = 0, P = 0, W = 0 |
| écriture dans `kmain` | faute, CR2 = `&kmain`, P = 1, W = 1 ; code intact |
| écriture dans `.rodata` | faute, valeur intacte |
| lecture dans la page de garde | faute, CR2 exact, P = 0 |
| lecture de `0xE0000000` | faute |
| `0xD0000000` après `vmm_unmap` | faute (INVLPG effectif en K0.6 ; depuis K0.7-P4e-1, la fenêtre CR0.WP vide le TLB sous QEMU et cette observation ne prouve plus l'INVLPG : voir `docs/review/K0.7-P4f-B4-tlb-invalidation-evidence.md`) |
| écriture dans une projection en lecture seule | faute ; la lecture réussit |

Ce mécanisme est aussi le **prototype** de l'accès sûr du noyau à la mémoire des futurs programmes utilisateur (K1) : un pointeur fourni par un programme non fiable sera lu par une sonde, et non par un déréférencement direct.

---

## 37. Sécurité du jalon K0.6

| Aspect | K0.6 |
|---|---|
| **Actifs** | intégrité du code et des constantes du noyau ; pile ; tables de pages ; mémoire libre (confidentialité de son ancien contenu) ; exactitude de la comptabilité physique |
| **Frontières** | firmware/chargeur → noyau (carte mémoire non fiable) ; futur : utilisateur → noyau (pointeurs non fiables, préfiguré par les sondes) |
| **Menaces** | carte mémoire malformée ou hostile (lecture hors tampon, débordements) ; allocation d'un cadre réservé (corruption du noyau ou du firmware) ; double allocation et double libération ; fuite du contenu d'un cadre recyclé ; écriture dans `.text` (injection de code) ; déréférencement de NULL silencieux ; débordement de pile silencieux ; projection périmée dans le TLB ; écrasement silencieux d'une projection ; #DF sans tables de pages |
| **Invariants** | I1-I4 (§34.3) ; page 0 jamais projetée ; `.text`/`.rodata` jamais inscriptibles (WP = 1) ; page de garde jamais projetée ; toute suppression de projection suivie d'INVLPG ; `vmm_map` n'écrase jamais ; le CR3 de la tâche #DF est celui du noyau ; la RAM gérée est entièrement projetée (chaque cadre distribué est accessible) |
| **Protections** | parseur défensif ; arrondis intérieur/extérieur ; réserves prioritaires ; bitmap à détection de double libération ; remise à zéro ; PTE par région ; CR0.WP ; garde non présente + porte de tâche #DF ; INVLPG ; `SAMOS_EEXIST` ; table de correction limitée à deux instructions déclarées |
| **Tests** | 60 vérifications noyau K0.6 ; 5 démarrages fatals (null, wtext, wrodata, unmapped, stack) ; 4 tailles de RAM (16 Mio, 512 Mio, 1,5 Gio, 4 Gio) + autotests à 512 Mio ; hébergés : 20 000 cartes aléatoires, 200 000 opérations de PMM contre un modèle |
| **Limites** | **pas de NX** : données exécutables (i386 sans PAE) ; identité : toute la RAM est inscriptible par le noyau ; mémoire basse projetée en RW ; GDT, IDT et TSS inscriptibles (le TSS doit l'être : le CPU y écrit lors des commutations de tâche) ; PMM monoprocesseur et sans verrou (correct tant qu'il n'est pas appelé en interruption) ; aucun compteur de références : libérer un cadre encore projeté n'est pas détecté ; `vmm_map` accepte n'importe quelle adresse physique (API interne de confiance) ; RAM au-delà de 1 Gio ignorée ; pas d'espaces d'adressage séparés (K1) |

> **Mise à jour K0.7 (en cours, branche `claude/k0.7-hardening`).** Cette table décrit l'état de K0.6 et est conservée comme historique. Plusieurs limites y sont **dépassées** en K0.7 :
> - « aucun compteur de références » : les cadres ont désormais des références, et un cadre projeté n'est plus libérable (E5) ;
> - « `vmm_map` accepte n'importe quelle adresse physique » : seuls les cadres `ALLOCATED` sont acceptés, avec des droits `0` ou `PTE_WRITE` (E3) ;
> - les piles disposent de canaris, de sentinelles et de trois pages de garde (sous-jalon **R1 clôturé**, `docs/review/K0.7-R1-closure.md`).
>
> Restent vrais : **pas de NX**, GDT et TSS inscriptibles, PMM monoprocesseur sans verrou. Voir `docs/design/K0.7-*.md` et `docs/review/K0.7-R1g-audit.md`.
>
> **Mise à jour K0.7-P4 (clôturé dans son périmètre).** L'IDT, les gestionnaires, les racines du VMM, le répertoire et les tables de pages ne sont plus inscriptibles par l'identité (§39). Le risque N3 (libération d'un cadre encore projeté avec des références falsifiées) restait **ouvert** à cette date.
>
> **Mise à jour K1.0-N3 (2026-11-19).** N3 est corrigé dans le modèle actuel (un seul espace d'adressage, monoprocesseur), §40. K0.7 n'est pas clôturé dans son ensemble (`docs/review/K0.7-final-closure-audit.md`).

---

## 38. Questions de jury et réponses (K0.6)

**Q1. Différence entre mémoire physique, mémoire virtuelle et `malloc` ?**
La mémoire physique, ce sont les cellules de RAM, gérées par cadres de 4 Kio. La mémoire virtuelle, ce sont les adresses utilisées par le code, traduites par la MMU via les tables de pages. `malloc` découpe des pages en petits objets. SAMOS K0 implémente les deux premières, pas encore un allocateur d'objets.

**Q2. Pourquoi ne pas utiliser simplement mem_upper ?**
Parce que la RAM n'est pas contiguë : trous historiques, zones ACPI, fenêtre PCI. Seule la carte E820 indique précisément quelles régions sont utilisables. Utiliser des zones réservées corromprait le firmware ou les tables ACPI.

**Q3. Comment votre allocateur détecte-t-il une double libération ?**
Le bit du cadre est déjà à 0 lors de la seconde libération. L'appel est refusé (`SAMOS_ENOENT`) et compté. C'est un avantage de la bitmap sur une liste de cadres libres, qui accepterait le doublon.

**Q4. Pourquoi remettre les cadres à zéro ?**
Pour qu'aucun ancien contenu (données, anciennes tables de pages) ne fuie vers le nouveau propriétaire. C'est indispensable dès qu'il y aura plusieurs processus.

**Q5. Décrivez la traduction d'une adresse virtuelle.**
Les 10 bits de poids fort indexent le répertoire (CR3), les 10 suivants indexent la table, et les 12 derniers sont le décalage dans le cadre. Le résultat est le numéro de cadre du PTE concaténé avec le décalage. Les droits effectifs sont l'intersection de ceux du PDE et du PTE.

**Q6. Pourquoi CR0.WP ?**
Sans lui, le ring 0 ignore la lecture seule : le noyau pourrait écrire dans son propre code sans faute. Avec WP, une écriture dans `.text` produit #PF (code 0x3), ce que le test démontre.

**Q7. Votre noyau applique-t-il W^X ?**
Partiellement. Le code et les constantes ne sont pas inscriptibles. Mais la pagination 32 bits n'a pas de bit NX : les données restent exécutables. Il faudra PAE ou le mode long.

**Q8. Qu'est-ce que le TLB et pourquoi INVLPG ?**
Le TLB est un cache matériel des traductions, non cohérent avec les tables en mémoire. Après avoir retiré une projection, il faut invalider l'entrée correspondante, sinon l'ancienne traduction reste utilisable. Le test le vérifie : accès, retrait, puis accès qui doit fauter.

**Q9. Que se passe-t-il exactement lors d'un débordement de pile ?**
L'écriture dans la page de garde provoque #PF. Sa livraison nécessite d'empiler sur cette même page, d'où une seconde #PF, donc #DF. La porte de tâche de #DF bascule sur une pile dédiée avec le bon CR3. Le gestionnaire voit dans CR2 une adresse de la page de garde et diagnostique le débordement. Sans cette porte, ce serait un triple fault.

**Q10. Comment testez-vous qu'une écriture est interdite sans planter ?**
Avec une table de correction d'exception : l'accès est fait par une instruction assembleur dont l'adresse est connue du gestionnaire de #PF. Le gestionnaire redirige alors EIP vers un code de correction qui renvoie −1. C'est le principe de `copy_from_user` sous Linux. Toute autre faute reste fatale.

**Q11. Pourquoi une projection en identité ? Quelles limites ?**
Elle est simple : activer la pagination ne déplace rien, et le noyau accède directement aux cadres. Limites : seule la RAM projetée (≤ 1 Gio) est gérable, et l'espace bas est occupé par le noyau, ce qui empêche de donner aux programmes les mêmes adresses. Un noyau *higher-half* (au-dessus de `0xC0000000`, ou de `0xFFFF800000000000` en x86-64) résoudra ces deux points.


## 39. Protection des structures critiques et des tables de pages (K0.7-P4)

> **Statut :** P4 est clôturé **dans le périmètre de K0.7** (`docs/review/K0.7-P4-closure.md`), avec des risques résiduels explicitement acceptés. Ce n'est pas une preuve de sécurité générale.

### 39.1 Le problème

Avec une projection en identité, chaque cadre physique est accessible en écriture par le ring 0 à l'adresse virtuelle égale à son adresse physique. Les structures qui gouvernent la traduction et les interruptions (IDT, tables de gestionnaires, répertoire, tables de pages) sont donc **modifiables par une seule écriture fautive**. Modifier une PTE suffit à rendre `.text` inscriptible, ou à projeter n'importe quel cadre. P4 retire ces **alias inscriptibles permanents**.

### 39.2 Principe : lecture seule et passage contrôlé

- Les pages concernées sont projetées **sans W**. Avec CR0.WP = 1, le ring 0 lui-même faute en écrivant (SDM vol. 3A §4.6).
- Une écriture légitime d'une PDE ou d'une PTE passe par `pt_gate_write` :
  1. validation complète de la cible et de la valeur ;
  2. masquage des IRQ ;
  3. WP = 0 ;
  4. **une** écriture de 32 bits, alignée, donc atomique pour le MMU ;
  5. WP = 1 ;
  6. restauration de IF.
- Le code machine est vérifié par `check_elf.sh` : une seule instruction entre les deux écritures de CR0, pas d'appel.
- **Pourquoi pas un alias inscriptible ou une projection temporaire ?** Ces approches laissent une table inscriptible en permanence, ou créent une dépendance circulaire (il faudrait une PTE pour écrire une PTE). La fenêtre WP n'ajoute aucune projection (`docs/design/K0.7-P4c-controlled-writes.md`).
- **Limite :** `cli` ne masque ni la NMI ni le #MC. Leur gestionnaire s'exécuterait avec WP = 0. C'est prouvé par une injection sous GDB ; aujourd'hui, cela mène à une panique.

### 39.3 La frontière de confiance : ne pas croire le PMM

**Problème.** Les métadonnées du PMM (`g_meta`) sont des données RW. Si le VMM décide « ce cadre est une table » ou « ce cadre est projetable » sur leur seule foi, une écriture de 16 bits suffit à contourner P4 (défaut D1).

**Correction : critères dérivés de sources protégées ou constantes.**

| Critère | Source |
|---|---|
| structure de pagination | le répertoire actif et les cibles de ses PDE présentes (RO, à croissance seule) |
| image du noyau | `[__kernel_start, __kernel_end)`, symboles de l'éditeur de liens |
| mémoire basse | la constante de 1 MiB |

Ces critères s'appliquent aux projections (D1), à la publication d'une PDE (D3, N2) et à l'allocation **avant** la remise à zéro (N1).

**Raisonnement par induction.** Après `paging_init`, le répertoire est RO. Seul le passage l'écrit, et il refuse toute PDE qui n'est pas « absente vers table neuve, scellée, sans alias, hors de l'image ». L'ensemble des tables publiées ne fait donc que croître, sans doublon. Chaque table y est scellée et sans alias, et le critère de structure reste vrai pour chacune.

### 39.4 Création transactionnelle d'une table

Le cycle de vie d'une table est : FREE → PAGETABLE → remise à zéro → scellement (W retiré, INVLPG) → publication (PDE).
- Les erreurs **ordinaires** (paramètres, droits, références saturées, mémoire épuisée) sont détectées **avant** l'allocation, sans effet.
- Après l'allocation, tout refus révèle une **violation d'invariant**. Le cadre ne peut être ni rendu (sens unique du scellement) ni publié : le noyau s'arrête (D4).

### 39.5 TLB : ce que les tests prouvent ou non

Modifier une PTE n'invalide pas le TLB : INVLPG est nécessaire après un retrait de projection ou de droit (SDM vol. 3A §4.10.4.1). Une entrée P = 0 n'est jamais mise en cache ; l'INVLPG après la **création** d'une projection est donc optionnel (§4.10.4.3).

QEMU vide tout le TLB à chaque changement de CR0.WP, ce qui masque un INVLPG oublié. Il faut donc trois niveaux de preuve distincts (D2) :
- **structurelle :** le code machine, opérande et ordre compris ;
- **comportementale isolée :** une PTE modifiée sans écriture de CR0 ;
- **intégration :** une copie du noyau où le passage n'écrit plus CR0.

Aucune n'est une validation sur processeur physique.

### 39.6 Risque ouvert : N3

Les références d'un cadre de **données** restent tenues dans `g_meta`. En les falsifiant, un cadre encore projeté peut être libéré puis redistribué : l'ancienne projection voit et modifie les données du nouveau propriétaire. En K0, aucun code de production n'utilise `vmm_map`, et toute la RAM est déjà inscriptible par l'identité : N3 n'y ajoute aucune capacité. En K1, avec des propriétaires distincts, c'est une faille d'**isolation**. Sa correction est la première condition de K1 (`docs/design/K1-readiness.md`).

### 39.7 Questions de jury

**Q1. Pourquoi ne pas simplement retirer W des tables et les modifier par un alias ?**
Un alias inscriptible permanent est exactement ce que P4 supprime. Une projection temporaire exige d'écrire une PTE, donc une table inscriptible : c'est circulaire. La fenêtre CR0.WP ne crée aucune projection. Son coût est une fenêtre d'une instruction, pendant laquelle une NMI s'exécuterait sans protection en écriture.

**Q2. En quoi la protection résiste-t-elle à des métadonnées PMM corrompues ?**
Les décisions critiques n'utilisent pas `g_meta`, mais le répertoire RO et des constantes. Elles concernent les alias, les PDE et l'allocation d'un cadre de l'image ou de la mémoire basse. Les tests falsifient `g_meta` et vérifient le refus, sans effet de bord, octet par octet sous GDB pour l'allocation.

**Q3. Les tests passent sous QEMU : le noyau est-il sûr ?**
Non. Les tests prouvent des invariants précis dans des scénarios connus, sous QEMU, sur un seul processeur. Restent notamment : la GDT/TSS inscriptibles, la NMI pendant la fenêtre, N3, le matériel réel et le SMP (`K0.7-P4-closure.md` §7).

## 40. Cohérence entre références physiques et projections virtuelles (K1.0-N3)

> **Statut :** implémenté, contre-vérifié (K1.0-N3-VERIFY, K1.0-N3-TEST-CLOSE) et **intégré dans `main`** le 2026-11-19 (`daaa7a8`), sur décision du développeur. Garanties limitées au modèle actuel : un seul espace d'adressage, monoprocesseur. Rapport : `docs/review/K1.0-N3-implementation.md`.

### 40.1 Le problème : une métadonnée n'est pas une preuve

Un allocateur de cadres doit répondre à deux questions : « puis-je libérer ce cadre ? » et « puis-je le donner à quelqu'un d'autre ? ». Jusqu'en K0.7, la réponse venait du **descripteur** du cadre (`g_meta`) : son compteur `refs`, tenu par `vmm_map`/`vmm_unmap`, et son type (FREE, ALLOCATED…). Ce descripteur est une **donnée inscriptible** du noyau. Une seule écriture parasite (bogue, corruption mémoire) suffit donc à faire libérer un cadre **encore projeté**. Le next-fit finit par le redistribuer, et l'ancienne projection lit et écrit alors les données du nouveau propriétaire (expérience R1 : réutilisation au bout de 32 266 opérations légitimes).

C'est le problème classique de la **cohérence entre comptabilité et réalité**. Un compteur de références n'est sûr que si toutes ses mises à jour le sont. Linux tient par exemple un `_mapcount` par page, distinct du compteur de références, et le *reverse mapping* (rmap) permet de retrouver toutes les PTE d'une page avant de la libérer ou de la déplacer.

### 40.2 Principe retenu : interroger la réalité

La « réalité » d'une projection, c'est la **PTE** que la MMU lira. Après P4, les tables de pages sont la structure la mieux protégée du noyau :
- scellées en lecture seule dans l'identité (P4e-2) ;
- écrites par un seul passage contrôlé, dans une fenêtre CR0.WP (P4e-1) ;
- PDE à écriture unique, donc un ensemble de tables qui ne fait que croître (D3).

L'**oracle** `dyn_mapping_count(f)` compte les PTE présentes à `va ≥ idmap_top` qui désignent `f`. Il ne lit **jamais** `g_meta`. Le PMM l'appelle directement :

```
pmm_free_frame(f) :  si oracle(f) > 0        -> EBUSY, aucune écriture      (N3-I1)
alloc_zeroed(f)   :  si oracle(f) > 0        -> panique AVANT memset         (N3-I2)
vmm_unmap(va)     :  D <- oracle(f) ; efface PTE ; INVLPG ;
                     si type(f) = ALLOCATED et refs(f) >= D : refs <- refs - 1
                     sinon : anomalie comptée, aucune écriture           (N3-I4)
```

**Pourquoi c'est sûr malgré la falsification.** La décision de libérer ou de distribuer ne dépend plus que de l'état des tables, qu'une écriture dans `g_meta` ne modifie pas. On remplace ainsi une hypothèse faible (« `g_meta` est intègre ») par une hypothèse déjà établie et testée par P4 (« les tables publiées sont intègres »).

**Identité et projections dynamiques.** L'identité permanente projette **toute** la RAM gérée (`frame = va`, imposé par le passage) : chaque cadre y figure. La compter rendrait tout cadre « projeté ». L'oracle l'exclut **PTE par PTE** (`va ≥ idmap_top`) et non PDE par PDE. En effet, quand `idmap_top` n'est pas multiple de 4 MiB (128 MiB → `0x07fe0000`), la PDE 31 porte une table **partagée** : 992 PTE d'identité et 32 emplacements dynamiques.

### 40.3 Le retrait sans réparation (U2 sous condition de sûreté)

Avec plusieurs projections d'un même cadre, `refs` devrait valoir `D`, le nombre réel de projections. Si `refs < D` (déficit, falsifié), décrémenter retirerait sa référence à une projection **restante** : c'est l'expérience R3, où un ancien propriétaire consomme la référence du nouveau. La règle `unref ⇔ refs ≥ D` garantit, en notant `refs'` le compteur après le retrait :

$$refs' \ge \min(refs,\; D - 1)$$

- si `refs ≥ D`, alors `refs' = refs - 1 ≥ D - 1`, et chaque projection restante est couverte ;
- sinon `refs' = refs` : le déficit ne s'aggrave pas.

La PTE est **toujours** effacée, puisque retirer un droit d'accès ne peut pas créer de fuite. Aucune « réparation » de `g_meta` n'est tentée : il faudrait connaître le vrai type et les références prises pour d'autres raisons, et aucune source fiable ne les donne. C'est le principe du **refus fermé** (*fail-closed*) : dans le doute, ne rien écrire.

### 40.4 Arrêter avant d'écrire

Dans `alloc_zeroed`, le cadre a déjà quitté l'état FREE (`pmm_core_alloc_as`). Rendre une erreur obligerait à le remettre FREE, et il serait redistribué au prochain appel. Comme pour N1, le seul refus sûr est un **arrêt déterministe avant la remise à zéro**, avec l'empreinte FNV-1a de la page. Le banc vérifie que cette empreinte est égale à celle mesurée avant l'allocation (modes `n3type`, `n3table`), et GDB montre la page intacte au moment de la panique. Avant K1.0, dans le cas d'une table (R6), le contrôle D1 de `table_create` arrêtait bien le noyau, mais **après** que `memset` avait effacé les données de l'ancien propriétaire.

### 40.5 Coût

L'oracle lit 1 024 PTE par table présente au-delà de `idmap_top` : son coût est O(T) par appel. Sous TCG, avec 1 000 couples `alloc` + `free`, on mesure environ ×2,7 sans table dynamique, ×6,7 avec 8 tables et ×22,7 avec 32. Le coût fixe vient principalement du parcours des PDE au-delà de l'identité (768 à 993 lectures par appel, deux appels par couple) ; la table de la frontière, lue en entier quand `idmap_top` n'est pas multiple de 4 MiB, n'en ajoute qu'une part (mesure à 1500 MiB, `docs/review/K1.0-N3-final-verification.md` §6). Une structure scellée de comptes par cadre (approche B) donnerait O(1), au prix d'une nouvelle structure de confiance et d'une refonte du passage (deux écritures par fenêtre WP). C'est une décision d'architecture, renvoyée à K1.4.

### 40.6 Limites

- **Un seul répertoire :** avec plusieurs espaces d'adressage (K1.4), l'oracle devra les parcourir tous.
- **Disponibilité :** un excès falsifié rend un cadre non libérable (fuite signalée par l'audit, `ref_surplus`).
- **Propriété :** `vmm_map` peut toujours projeter n'importe quel cadre ALLOCATED.
- **Retrait sous table falsifiée :** si le type de la table ou du répertoire est falsifié, le passage refuse aussi le retrait (refus fermé de P4).

### 40.7 Questions de jury

**Pourquoi ne pas simplement protéger `g_meta` en lecture seule ?** Il est écrit à chaque allocation et à chaque libération. Le sceller exigerait une fenêtre CR0.WP par opération du PMM, ce qui étendrait à tout le PMM la surface du passage. L'oracle obtient la même garantie pour la question « est-il projeté ? », sans nouvel écrivain privilégié.

**L'oracle n'est-il pas lui-même une dépendance du PMM envers le VMM, donc un cycle ?** C'est une dépendance **en lecture seule**, par appel direct : l'oracle n'alloue rien et n'appelle pas le PMM. Il n'y a donc ni récursion ni cycle à l'exécution. Un pointeur de fonction aurait été plus « découplé », mais un pointeur stocké en mémoire inscriptible est une cible de détournement.

**Que se passe-t-il si les tables elles-mêmes sont corrompues ?** La garantie tombe. C'est l'hypothèse de confiance de P4 : tables scellées, un seul écrivain, audit au démarrage. N3 ne l'affaiblit pas, il s'appuie dessus.

**Pourquoi `refs` existe-t-il encore ?** Il reste la seconde barrière pour les références prises **sans** projection (épinglage), que l'oracle ne voit pas, et il permet un refus rapide dans le cas normal. L'audit signale ses incohérences (`ref_mismatch`, `ref_surplus`).

## 41. Une image i386 sous deux émulateurs ; GDB face à un CPU 64 bits (K1.1-M64-0)

> **Statut :** infrastructure de test seulement ; aucun code du noyau n'est modifié. Rapport et preuves : `docs/review/K1.1-M64-0-report.md`.

### 41.1 Le problème

La migration x86-64 (décision D1) doit garder une **référence i386 gelée** (étiquette `ref/i386-k0.7`) et la tester en continu. Les futurs jalons démarreront sous `qemu-system-x86_64`. Avant d'y écrire la moindre ligne de code 64 bits, il faut donc savoir si les bancs actuels donnent **les mêmes verdicts** quand on change seulement d'émulateur. Sinon, une différence de plateforme serait plus tard confondue avec une régression de la migration.

### 41.2 Deux émulateurs, deux CPU différents

Un processeur x86-64 démarre en mode réel, exactement comme un i386. Le chargeur Multiboot de QEMU le fait passer en mode protégé 32 bits avant de sauter à `_start`. **Le même noyau 32 bits** fonctionne donc sur les deux machines. Elles ne sont pourtant **pas** équivalentes ; voici ce qui a été mesuré par QMP sous QEMU 8.2.2 :

| | `qemu-system-i386` | `qemu-system-x86_64` |
|---|---|---|
| Modèle de CPU par défaut | `qemu32` | `qemu64` (annonce le mode long dans CPUID) |
| Machine | `pc-i440fx-noble-v2` | `pc-i440fx-noble-v2` |
| Carte E820 à 128 MiB | 6 régions | 7 régions (`0xfd00000000-0xffffffffff` réservée en plus, au-delà de 4 GiB) |

La région supplémentaire se trouve au-dessus de 4 GiB. Le noyau 32 bits la publie mais ne l'utilise pas, d'où **130 559 KiB disponibles** dans les deux cas. Les journaux bruts diffèrent donc légitimement ; c'est pourquoi la comparaison porte sur les verdicts et non sur le texte (§41.4).

### 41.3 Le protocole distant de GDB et la description des registres

GDB pilote le « stub » intégré à QEMU par le *GDB Remote Serial Protocol*. Le paquet `g` renvoie **tous** les registres d'un bloc, dans l'ordre et à la taille donnés par une **description de cible** XML (`qXfer:features:read`). Le stub de `qemu-system-x86_64` décrit **toujours** l'architecture 64 bits (`rax`…`r15`, `rip` ; 608 octets), quel que soit le mode du CPU à cet instant. Un GDB qui a déduit « i386 » d'un ELF32 attend 312 octets. Il rejette donc la cible :

```text
Remote 'g' packet reply is too long (expected 312 bytes, got 608 bytes)
warning: Architecture rejected target-supplied description
```

La solution retenue impose `set architecture i386:x86-64` **avant** `target remote`, puis charge les symboles par `symbol-file`. GDB accepte alors la description. En mode protégé, les 32 bits de poids faible de `rsp` **sont** ESP, et `$esp` est un pseudo-registre défini comme cette moitié basse. EFLAGS et la mémoire sont lus sans transformation. Les adresses des symboles ELF32 sont les mêmes. En revanche :

- `$eip` n'existe pas dans cette architecture : on utilise `$pc` ou `$rip` ;
- le **désassembleur** décode le code 32 bits comme du code 64 bits (`push %rbp` au lieu de `push %ebp`) ;
- l'**affichage symbolique** des arguments C est faux (`kmain (magic=0, ...)`) : les emplacements DWARF 32 bits sont interprétés avec la disposition des registres 64 bits.

Les vérifications GDB n'utilisent que des registres (`$esp`, `$eflags`), la mémoire brute et des adresses de symboles. Chacune a été comparée **valeur par valeur** sous les deux émulateurs : ESP = `0x196fec`, `[ESP+4]` = `0x2badb002`, `[ESP+8]` = `0x9500`, EFLAGS = `0x46`, PC = `kmain` = `0x1029b0`. L'inverse échoue aussi, et c'est voulu : imposer `i386:x86-64` face à `qemu-system-i386` est refusé (« not compatible with reported target architecture i386 »).

### 41.4 Ne jamais confondre absence de validation et succès

Trois mécanismes empêchent un faux succès :

1. **Identification de l'émulateur** (`tests/qemu/emu.sh`). Avant tout test, QEMU est interrogé par QMP. Un émulateur absent, une réponse vide (`/bin/true`) ou un CPU d'une autre architecture donnent le statut 2.
2. **Trois exigences par test GDB.** Le statut de GDB doit être nul. Son journal ne doit contenir aucun diagnostic d'échec connu. Les valeurs attendues doivent être présentes. Une valeur correcte affichée par un GDB en échec reste un échec (test E6).
3. **Comparaison stricte** (`tests/qemu/emu_compare.sh`). Chaque côté est d'abord validé **seul** : bilan unique, sans échec et cohérent avec le nombre de verdicts (`QEMU:` et `GDB:`) ; aucune ligne `[FAIL]` ou `[SKIP]` ; SKIP des autotests conformes à une table explicite. Deux bancs qui échouent de la même façon ne sont donc jamais un succès. Les verdicts, les bilans et les SKIP sont ensuite comparés sans aucune tolérance, sauf la parenthèse « (avant 0x…, refus 0x…) » des deux seuls cas `n1stack` et `n1low`, affichée en `[INFO]`. Un résultat absent ou une erreur interne du comparateur est un échec, jamais un accord. **Limite :** une erreur commune aux deux bancs et cohérente avec leurs bilans n'est pas détectable par comparaison ; la parité ne prouve pas la correction du noyau.

### 41.5 Questions de jury

**Pourquoi ne pas simplement remplacer qemu-system-i386 par qemu-system-x86_64 ?** Parce que les deux CPU diffèrent (§41.2). La référence i386 doit rester mesurée sur le CPU sur lequel elle a été validée. Le second émulateur est ajouté **à côté**, et les écarts sont rendus visibles.

**La lecture de `$esp` via une architecture 64 bits est-elle fiable ?** Pour un CPU en mode protégé, oui : l'architecture définit ESP comme les 32 bits de poids faible de RSP. Ce n'est pas qu'une affirmation : la mesure a été répétée sous les deux émulateurs avec des valeurs identiques. Les tests négatifs E5 montrent en outre qu'une architecture inadaptée produit bien des échecs.

## 42. Construction double i386 / x86-64 : un squelette ELF64 contrôlé (K1.1-M64-1)

> **Statut :** chaîne de construction **seulement**. Le squelette ELF64 est compilé, lié et inspecté **statiquement**. Il n'est **pas amorçable** et **aucun code 64 bits n'est exécuté** par SAMOS. Rapport : `docs/review/K1.1-M64-1-report.md`. Conception : `docs/review/K1.1-M64-1-design.md`.

### 42.1 Pourquoi un squelette avant le tremplin

Le passage au mode long (M64-2) mêlera du code 32 bits et 64 bits, un nouveau format d'image et de nouvelles tables. Si la **chaîne de construction** elle-même est fausse (mauvaise cible, drapeau oublié, objet i386 lié dans l'image 64 bits, segment W+X), les erreurs de M64-2 seraient indiscernables d'erreurs de construction. M64-1 isole donc ce risque : on prouve d'abord que l'on sait **produire** un ELF64 conforme, sans rien exécuter.

### 42.2 Séparation des deux architectures

- **Variables propres.** `mk/x86_64.mk` définit `X64_CFLAGS`, `X64_ASFLAGS`, `X64_LDFLAGS`. Les variables de l'i386 (`CFLAGS`, `ASFLAGS`, `LDFLAGS`, `ARCH`) ne sont ni lues ni modifiées.
- **Répertoires propres.** Les objets x86-64 vont dans `build/x86_64/{prod,test}/`. Une règle à motifs GNU Make `build/prod/%.o` ne peut désigner que des chemins commençant par `build/prod/` : les règles des deux architectures ne peuvent pas se capturer mutuellement.
- **`make all` inchangé.** Il ne construit que l'i386. Le moteur de mutations, la reproductibilité et les images de référence sont donc intacts.
- **Preuve.** La sortie de `make -n -B all` est identique avant et après l'ajout. Les images i386 sont identiques octet pour octet à la référence, et chaque objet est contrôlé par sa classe ELF et sa machine.

### 42.3 Les drapeaux propres au x86-64

| Option | Rôle | Conséquence si absente |
|---|---|---|
| `--target=x86_64-unknown-none-elf` | code x86-64 sans système d'exploitation | code i386 ou dépendant de l'hôte |
| `-mno-red-zone` | interdit la **zone rouge** de l'ABI System V AMD64 : 128 octets sous RSP qu'une fonction feuille peut utiliser sans décrémenter RSP | une interruption empile sur la pile courante et **écrase** ces données. Corruption silencieuse, propre aux noyaux |
| `-mgeneral-regs-only` | aucun registre x87, MMX, SSE ou AVX | Clang utiliserait SSE pour des copies ou des calculs flottants, alors qu'aucun état FPU n'est sauvegardé ni activé (CR4.OSFXSR) |
| `-mcmodel=small` | adresses du code et des données sous 2 GiB, adressage relatif à RIP sur 32 bits | **hypothèse locale** : vraie pour un noyau lié à 1 MiB. Un noyau *higher-half* demanderait `-mcmodel=kernel` (adresses dans les 2 GiB **hauts**). C'est la décision A-6, liée à D-LAYOUT, **non tranchée** |
| `-fno-pic -fno-pie` | code à adresse fixe | table GOT et relogements dynamiques inutiles en noyau |

`-mstack-alignment=16`, indispensable pour la cible i686, est **inutile** en x86-64 : l'ABI impose déjà un alignement de 16 octets.

### 42.4 Ce que prouvent les contrôles statiques, et ce qu'ils ne prouvent pas

`tools/check_elf64.sh` (règles X1 à X13) vérifie :
- l'en-tête : ELF64, x86-64, EXEC, entrée `_start` ;
- l'**absence** de magic Multiboot, car l'image ne doit pas se présenter comme amorçable ;
- les sections, alignées sur 4 KiB et ordonnées ;
- les segments : trois PT_LOAD R E / R / RW, aucun W+X, aucune page partagée ;
- l'absence de symbole indéfini et de section dynamique ;
- les bornes de l'image, et la séparation production/test ;
- le **périmètre** : la mnémonique de chaque instruction et ses registres sont comparés à des listes d'interdits. Sont refusés les registres de contrôle et de débogage, les MSR, les tables de descripteurs, LMSW/CLTS, INVLPG, IRET, SYSCALL/SYSRET, ainsi que toute instruction x87, MMX, SSE ou AVX ;
- la correspondance exacte des sections avec les segments (`.text`, `.rodata`, `.data .bss`) et leurs drapeaux (X13) ;
- la compatibilité des adresses avec le modèle `small`.

**Limite constatée.** GNU ld résout un symbole **faible** indéfini, ou un symbole forcé par `--unresolved-symbols=ignore-all`, à l'adresse 0, et **le retire** de la table des symboles de l'exécutable statique. Aucune lecture de l'image ne peut alors le révéler. Ce cas est donc contrôlé **en amont**, sur les objets d'entrée (règles L0 à L2) et sur les options de `ld` (F7).

**Leçon de l'audit (M64-1-AUDIT).** La première version de X11 ne cherchait que quelques mnémoniques et noms de registres. Des tests sur des images volontairement fausses ont montré qu'elle acceptait FNINIT, FLDZ, EMMS, LDMXCSR, FXSAVE, VZEROUPPER, LMSW, CLTS, INVLPG, IRETD et les registres de débogage : c'étaient des faux PASS. Un vérificateur se juge donc par ses **tests négatifs**, et chaque test négatif doit lui-même prouver que le défaut est bien présent dans l'image (sonde indépendante du vérificateur).

**Échec fermé des outils (K1.1-M64-2-L1-1).** Un vérificateur peut produire un faux PASS sans que la **règle** soit fausse : il suffit que l'**outil** qu'il interroge échoue sans que l'erreur se propage. Trois mécanismes du shell en sont responsables, et tous trois étaient présents :

| Mécanisme | Exemple (avant L1-1) | Effet |
|---|---|---|
| Statut d'un pipeline | `und=$(nm -u "$ELF" 2>/dev/null \| grep -c .)` | le statut est celui de **la dernière commande** (`grep`, qui réussit) : un `nm -u` en échec donne « 0 symbole indéfini », donc X8 PASS |
| Erreur jetée | `2>/dev/null` | le message de l'outil disparaît : rien ne signale la panne |
| Erreur arithmétique de bash | `a=$((0x$addr))` sur un champ illisible | bash **abandonne la boucle entière** puis **continue** le script ; les drapeaux `align_ok`, `seg_ok`, `x12` restent à 1, la règle passe, et le statut final est 0 |

Les mêmes motifs rendaient PASS X5 (`od`), X7 (`readelf -l`), X8 (sections dynamiques), X10 en production (`nm`), X12 et X4 (champ de symbole illisible). Chaque cas a été **démontré** par un outil factice, qui écrit un marqueur prouvant que la panne a réellement eu lieu.

**Correction :**
1. **Collecte.** Chaque outil externe s'exécute **une seule fois**, avant toute règle.
2. **Contrat par outil.** Un statut non nul est une erreur d'outil. Une sortie vide l'est aussi quand le contrat en exige une : `od`, `readelf -S` et `readelf -l` produisent toujours du texte sur un ELF. En revanche, la sortie vide est **légitime** pour `nm -u` (aucun symbole indéfini : c'est le cas normal) et pour `nm` (image sans symboles : X4 et X9 échouent alors normalement).
3. **Validation des champs numériques.** Chaque champ est validé avant tout calcul. Un champ illisible est une **erreur interne d'analyse**.
4. **Statut.** Ces erreurs donnent le statut **2**, avant toute règle, donc **sans aucun PASS**. Les critères des règles X1 à X13 ne changent pas : sur les 64 couples (image, mode) du banc, l'ancien et le nouveau vérificateur rendent des statuts et des lignes PASS/FAIL identiques.

**Limite.** `readelf -h` garde sa politique historique : un échec y est un refus X1 (statut 1), car il couvre aussi le fichier non ELF. Un `readelf -h` en panne sur un ELF valide est donc refusé (échec fermé), mais avec un diagnostic « non ELF » imprécis.

**Reproductibilité.** Elle est démontrée sur une même machine, dans un même répertoire source. Entre deux répertoires, les images diffèrent **seulement** par `.debug_info` : NASM y écrit le répertoire de compilation absolu (`DW_AT_comp_dir`). Les images sans débogage sont identiques, et une construction locale dans le chemin de la CI reproduit exactement l'empreinte de la CI. C'est la même limite que pour l'i386.

**Ce que ces contrôles ne prouvent pas :** que du code 64 bits s'exécute correctement, qu'une protection mémoire est appliquée (aucune table de pages n'existe), ou que l'image est chargeable. Ce sont les objets de M64-2 et des jalons suivants.

### 42.5 Questions de jury

**Pourquoi ne pas simplement compiler tout le noyau en 64 bits ?** Parce que ce serait un portage non contrôlé. Les modules spécifiques à l'i386 (GDT, IDT à portes de tâche, pagination à 2 niveaux, cadre d'interruption 32 bits) n'ont pas d'équivalent direct. Les compiler en 64 bits produirait du code qui se lie peut-être, mais dont le comportement serait faux. M64-1 ne revendique que la chaîne de construction.

**À quoi sert un test négatif sur un vérificateur ?** À prouver qu'il **peut échouer**. Un vérificateur qui accepte tout produirait des faux PASS. Pendant M64-1, un test négatif a montré qu'un symbole indéfini passait la règle X8, d'où le contrôle sur les objets.

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
15. Intel SDM vol. 3A : §3.4 (sélecteurs, descripteurs, §3.4.5.1 types et bit Accessed), §6.3-6.5 (sources et classes d'exceptions), §6.10-6.12 (IDT, portes, procédures de traitement), §6.13 (codes d'erreur), §6.14-6.15 (tableau 6-5 de la double faute, référence de chaque exception), chap. 7 (gestion des tâches, TSS, portes de tâche).
16. Intel SDM vol. 2 : `LGDT/LIDT`, `LTR`, `IRET/IRETD`, `PUSHAD/POPAD`, `INT n/INT3`, `UD2`, `DIV`.
17. Intel, *8259A Programmable Interrupt Controller*, fiche technique (ICW1-ICW4, OCW1-OCW3, mode cascade, interruptions parasites).
18. Intel, *82C54 CHMOS Programmable Interval Timer*, fiche technique (modes 0-5, commande de verrouillage).
19. IBM, *Personal Computer AT Technical Reference* (contrôleur clavier 8042, ports 0x60/0x64, codes de balayage).
20. Motorola, *MC146818A Real-Time Clock*, fiche technique (registres CMOS, bit UIP).
21. Intel SDM vol. 2 (`STI` : inhibition des interruptions pour l'instruction suivante ; `HLT` ; `RDTSC`) et vol. 3A §9.2 (ordonnancement mémoire x86).
22. ISO/IEC 9899:2011, §6.2.5 p9 (arithmétique non signée modulo $2^N$), §7.17 (atomiques, pour l'extension multiprocesseur).
23. Intel SDM vol. 3A, chap. 4 : §4.1 (modes de pagination), §4.3 (pagination 32 bits, figures 4-2 et 4-4), §4.6 (droits d'accès, CR0.WP), §4.7 (code d'erreur de #PF), §4.10 (TLB, INVLPG) ; §6.15 (exception 14).
24. ACPI Specification, chapitre « System Address Map Interfaces » (INT 15h E820h, types de plages).
25. D. P. Bovet, M. Cesati, *Understanding the Linux Kernel*, 3e éd., O'Reilly, 2005 : chap. 2 (pagination), chap. 8 (cadres, buddy allocator), chap. 10 (table des exceptions `__ex_table`).
26. A. S. Tanenbaum, H. Bos, *Modern Operating Systems*, 4e éd., chap. 3 (gestion de la mémoire, bitmaps et listes, pagination, TLB).
