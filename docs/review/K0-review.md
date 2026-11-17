# SAMOS / AtlasOS — Revue de fin de phase K0

**Phase :** SAMOS-K0, reconstruction du noyau et fondations (K0.1 à K0.6)
**Date :** 2026-11-17
**Base revue :** branche `claude/trusting-lovelace-x7fiip`, de `83e0fb4` (K0.1) au commit K0.6
**Statut :** développement fonctionnel **arrêté**, conformément au cahier des charges (§5, « point d'arrêt obligatoire »). Aucun travail de K1 n'a commencé.

> Ce document est une revue technique, pas une certification. Un test réussi prouve que la propriété vérifiée tient dans les conditions du test, rien de plus. Aucune garantie de sécurité n'est revendiquée.

---

## 1. Périmètre et méthode

Le cahier des charges demande d'examiner l'architecture, la correction, la sécurité, la gestion mémoire, les interruptions, les interfaces, la robustesse, la préparation à x86-64, les tests de régression et la documentation.

Méthode :

1. relecture complète de chaque fichier du noyau, à la recherche de défauts, d'hypothèses implicites et de couplages ;
2. vérification que chaque invariant documenté dans `Details.md` est couvert par au moins un test ;
3. exécution intégrale du banc de tests depuis un dépôt propre (`make clean && make test`) ;
4. comparaison du noyau obtenu avec les exigences de x86-64.

## 2. Inventaire

| Élément | Valeur mesurée |
|---|---|
| Code noyau (C, NASM, en-têtes, script de liaison), hors tests | 4 960 lignes |
| — dont `arch/i386` (spécifique au CPU) | 1 756 lignes |
| — `core` / `drivers` / `lib` / `mm` / `include` | 544 / 761 / 496 / 445 / 958 lignes |
| Autotests dans le noyau (`kernel/tests`) | 1 121 lignes |
| Bancs de test hôtes et QEMU (`tests/`, `tools/`) | 1 098 lignes |
| Image `build/samos.elf` | 198 600 octets (avec DWARF) ; `.text` 33 156 o, `.rodata` 13 796 o, `.bss` 142 236 o, `.bootstack` 20 480 o |
| Empreinte mémoire du noyau | `0x00100000`-`0x00136000` (216 KiB, bitmap PMM de 128 KiB comprise) |
| Profondeur maximale de pile au démarrage | 836 octets sur 16 384 |
| Dépendances d'exécution | **aucune** (`nm -u` vide : ni libc, ni libgcc) |
| Documentation | README 270 lignes, Walkthrough 900, Details 1 760 |
| Commits K0 | 6 (un par jalon), tous poussés |

## 3. Architecture réalisée

### 3.1 Ordre d'initialisation (`kmain`)

```text
_start (NASM)  pile 16 KiB alignée 16, EFLAGS=0, .bss=0, pile peinte, EBP=0
   │
kmain
   ├─ log_init        VGA 80x25 + UART 16550 (test de boucle locale) + debugcon
   ├─ gdt_init        code/données plats, TSS principal (TR), TSS de #DF
   ├─ idt_init        32 portes d'exception, #DF = porte de tâche
   ├─ bootinfo_init   magic, bornes, copies bornées (cmdline, chargeur)
   ├─ layout          régions de l'image vérifiées
   ├─ mm_init         carte E820 → PMM (bitmap) → pagination (CR0.PG|WP)
   ├─ pic_init        8259A remappé 0x20-0x2F, tout masqué
   ├─ pit_init        100 Hz, IRQ 0
   ├─ keyboard_init   8042 + clavier, IRQ 1
   └─ ktest_run_all (si « test ») | idle_loop (STI ; écho clavier ; sti;hlt)
```

Chaque étape ne dépend que des précédentes. Le journal est initialisé en premier, pour que toute défaillance ultérieure soit **visible**. Les exceptions sont capturées avant la lecture des données du chargeur. La pagination est active avant la première IRQ.

### 3.2 Graphe de dépendances des modules

```text
            ┌──────────── kernel/tests (autotests, modes de faute) ───────────┐
            ▼                                                                 │
 core/kmain ──► core/{bootinfo, layout, log, panic} ──► lib/{string, format, math64, ringbuf}
     │                         │                                  ▲
     ├──► mm/{memmap, pmm_core, pmm, mm} ──────────────────────────┤  (portables)
     │                         │                                  │
     ├──► drivers/{vga, serial, pit, ps2kbd, kbd_decode, qemu_dbg}─┤
     │                         │
     └──► arch/i386/{entry, gdt, idt, isr_stubs, exceptions, pic, irq, paging, probe}
                 (seul code dépendant du CPU et du format des tables matérielles)
```

**Constats d'architecture :**

- **Conforme à l'ADR 0001.** Noyau monolithique modulaire, i386, Multiboot 1, Clang, Make, chargé à 1 MiB en identité.
- **Frontière arch/portable respectée pour l'essentiel.** Les modules purs (`string`, `format`, `math64`, `ringbuf`, `kbd_decode`, `memmap`, `pmm_core`) sont compilés **et testés sur l'hôte** sans modification, ce qui prouve leur indépendance vis-à-vis du matériel.
- **Couplages notables :**
  - `core/layout.c` inclut `arch/paging.h` et `arch/cpu.h` (vérification de la garde par le MMU) ;
  - `drivers/pit.c` et `drivers/ps2kbd.c` dépendent de `arch/irq.h` ;
  - `arch/exceptions.c` appelle `paging_classify`.

  Ces couplages sont acceptables en K0, mais une couche HAL minimale (`hal_irq_register`, `hal_mmu_is_mapped`) les isolerait avant le portage x86-64. Voir la recommandation R4.
- **Aucune IA ni logique de haut niveau dans le noyau**, conformément à l'objectif D.

## 4. Matrice de vérification (exécution du 2026-11-17)

Commande : `make clean && make test`. Résultat : **61 lignes `[PASS]`, 0 `[FAIL]`, statut 0.**

| Catégorie | Résultat | Preuve |
|---|---|---|
| Compilation | **PASS** | Clang 18.1.3, `-Werror -Wformat=2 …`, 0 avertissement ; NASM `-w+error` |
| Édition de liens | **PASS** | ld 2.42 `--fatal-warnings`, 8 `ASSERT` de liaison, `nm -u` vide |
| Boot | **PASS** | QEMU 8.2.2, Multiboot 1 ; RAM de 16 MiB, 128 MiB, 512 MiB, 1,5 GiB et 4 GiB |
| Correction fonctionnelle | **PASS** | 223 vérifications dans le noyau ; ABI vérifiée instruction par instruction par GDB |
| Sécurité (tests ciblés) | **PASS** | 13 démarrages volontairement fatals diagnostiqués ; tests négatifs (magic, garde, double libération, sondes) ; ASan/UBSan sur 7 modules |
| Intégration | **PASS** | clavier de bout en bout (frappes réelles via QEMU), IRQ sous pagination, #DF avec CR3 paginé |
| Régression | **PASS** | toutes les suites K0.1 à K0.5 rejouées à chaque jalon ; 5 régressions détectées et corrigées |
| Documentation | **PASS** (relecture) | README, Walkthrough et Details cohérents avec le code au commit K0.6 |
| Conformité architecturale | **PASS** | voir §3 ; aucune dépendance non approuvée |
| IRQ parasites 7/15 | **NOT TESTED** | chemin non reproductible de façon déterministe sous QEMU |
| Matériel réel | **NOT TESTED** | aucune exécution hors QEMU |

## 5. Défauts trouvés et corrigés pendant K0

Ces défauts sont **résolus**. Ils sont listés ici parce qu'ils illustrent la valeur des tests négatifs et des régressions.

| Jalon | Gravité (à la découverte) | Défaut | Détection | Correction |
|---|---|---|---|---|
| K0.4 | **HIGH** (banc de test) | Un triple fault n'était **pas** détecté : avec `-no-reboot`, QEMU s'arrête sans journaliser de nouveau `CPU Reset` | test volontaire `exc_triple` | détection par la ligne `Triple fault` |
| K0.5 | **HIGH** | Le clavier ne recevait rien : la réécriture d'une copie périmée de la configuration du 8042 recoupait l'horloge du port 1 (bit 4) | test de bout en bout `keyboard_e2e` + compteurs | bit 4 effacé explicitement |
| K0.5 | **HIGH** | Course de réveil perdu dans la boucle d'inactivité (`sti` … `cli; sti; hlt`) | relecture, avant toute exécution | test avec IF = 0 puis `sti; hlt` |
| K0.5 | MEDIUM | `INT 0x20` (tests K0.4) exécutait le gestionnaire du timer et envoyait un EOI parasite au PIC | régression `exc_np`/`exc_df` | tests déplacés sur 0x2E ; risque documenté |
| K0.3 | MEDIUM (banc de test) | Course sur un journal périmé dans le test GDB VGA (échec intermittent) | 2e exécution de `make test` | suppression des journaux avant lancement ; 10/10 exécutions |
| K0.4 | LOW | Le CPU écrivait le bit *Accessed* dans la GDT | autotest (`0x92` lu `0x93`) | bit pré-positionné |
| K0.4 | LOW | Hypothèse fausse : `INT` vers une entrée nulle donne #GP (type invalide), pas #NP | test `exc_np` | deux cas distincts testés |
| K0.6 | LOW | Mesure de temps invalide (0 ms) avec IF = 0 | valeur physiquement impossible | mesure avec IF = 1 |
| K0.6 | LOW | Plafond de 1 GiB appliqué silencieusement | essai à 1,5 GiB / 4 GiB | avertissement + cas de test |

## 6. Constats ouverts

Gravités selon `CLAUDE.md` §5. **Aucun constat CRITICAL ou HIGH ouvert n'a été identifié.** Cela ne prouve pas qu'il n'en existe pas.

### 6.1 MEDIUM

**M1. Code de test et modes de faute volontaire inclus dans l'image du noyau.**

- **Emplacement :** `kernel/core/kmain.c:121` (`cmdline_has("test")`), `kernel/tests/*.c`, `Makefile` (`C_SRCS`).
- **Explication :** l'image unique contient les autotests et des modes qui provoquent délibérément des fautes (`test=wtext`, `test=stack`, `test=triple`…), sélectionnés par la ligne de commande du chargeur.
- **Déclencheur :** toute personne qui contrôle la ligne de commande de démarrage.
- **Conséquence :** en K0, cette personne contrôle déjà toute la machine, donc le risque est faible. Plus tard, c'est une surface d'attaque inutile et du code non destiné à la production.
- **Correction proposée :** option de compilation `CONFIG_KTEST` (par défaut 0), deux cibles (`build/samos.elf` et `build/samos-test.elf`), et `make test` sur l'image de test.
- **Test :** `check_elf.sh` vérifie l'absence du symbole `ktest_run_all` dans l'image de production.

**M2. L'allocateur physique n'a pas de notion de propriété ni de compteur de références.**

- **Emplacement :** `kernel/mm/pmm.c:61` (`pmm_free_frame`).
- **Explication :** libérer un cadre encore projeté, ou utilisé comme table de pages, n'est pas détecté. Seules la double libération et la libération d'un cadre réservé le sont.
- **Déclencheur :** un bogue du noyau (pas d'appelant non fiable en K0).
- **Conséquence :** *use-after-free* physique. Le cadre réalloué serait modifiable via l'ancienne projection.
- **Correction proposée (K1) :** tableau de descripteurs de cadres (propriétaire, compteur de projections), et `vmm_unmap` qui décrémente ce compteur.
- **Test :** projeter un cadre, le libérer, et exiger un refus.

**M3. `vmm_map` accepte n'importe quelle adresse physique.**

- **Emplacement :** `kernel/arch/i386/paging.c:57`.
- **Explication :** c'est une API interne de confiance, mais aucun contrôle n'empêche de projeter l'image du noyau ou une région du firmware dans une nouvelle adresse virtuelle **inscriptible**, ce qui contournerait la lecture seule de `.text` par un alias.
- **Conséquence :** sans objet en K0. Critique dès que le noyau projettera de la mémoire pour le compte d'un programme (K1).
- **Correction proposée :** séparer `vmm_map_kernel` (confiance) et `vmm_map_user` (vérification : le cadre appartient à l'appelant et n'est pas réservé).

**M4. Données du noyau exécutables (pas de NX).**

- **Emplacement :** `kernel/arch/i386/paging.c:189` (seul le bit R/W est disponible).
- **Explication :** la pagination 32 bits sans PAE n'a pas de bit d'interdiction d'exécution (SDM vol. 3A §4.6). W^X n'est réalisé que pour le code.
- **Correction :** matérielle, par PAE (entrées de 64 bits, bit 63) ou mode long. C'est un argument direct pour la migration x86-64.
- **Test (futur) :** sonde qui saute dans une page de données et exige #PF avec le bit I/D.

**M5. Pas de canari de pile (`-fno-stack-protector`).**

- **Emplacement :** `Makefile:39`.
- **Explication :** un débordement de tampon local qui écrase l'adresse de retour n'est pas détecté. La page de garde ne protège que contre l'épuisement de la pile, pas contre l'écrasement d'un cadre.
- **Correction proposée :** implémenter `__stack_chk_guard` (initialisé depuis RDTSC, ou RDRAND si présent) et `__stack_chk_fail` (qui appelle `panic`), puis passer à `-fstack-protector-strong`. C'est petit, indépendant et explicable.

### 6.2 LOW

| ID | Emplacement | Constat | Recommandation |
|---|---|---|---|
| L1 | `kernel/drivers/qemu_dbg.c:10` | Le port `0xE9` est écrit à chaque caractère, y compris sur du matériel réel (normalement inutilisé, mais non garanti) | `CONFIG_QEMU_DEBUGCON=0` pour une image destinée au matériel |
| L2 | `kernel/arch/i386/idt.c:30` | L'IDT est dans une page inscriptible ; le CPU n'y écrit jamais | la placer dans une page dédiée en lecture seule après `idt_init` (la GDT doit rester inscriptible : bit *busy* du TSS) |
| L3 | `kernel/arch/i386/paging.c:189` | Toute la mémoire basse (< 1 MiB, hors page 0) est projetée en RW | ne projeter que la VGA en RW, le reste en R ou non projeté |
| L4 | `kernel/drivers/ps2kbd.c:43` | Sans contrôleur 8042 (machines récentes), chaque attente bornée peut durer environ 1 s sur du matériel réel (10⁶ × `io_wait`) ; l'initialisation pourrait prendre plusieurs secondes | consulter l'indicateur « 8042 présent » de la table ACPI FADT ; réduire la borne |
| L5 | `kernel/core/bootinfo.c:39` | `copy_untrusted_string` lit 1 octet au-delà de la limite pour détecter la troncature | sans danger sans pagination (moment de l'appel) ; à revoir si l'appel est déplacé |
| L6 | `kernel/mm/pmm_core.c:85`, `kernel/mm/mm.c:56` | `base + length` vaut 0 (débordement) pour une région se terminant exactement à $2^{64}$ | **vérifié sans effet** (région ignorée, au-delà de 4 GiB) ; utiliser `base + (length - 1)` par cohérence |
| L7 | `kernel/core/log.c:30` | Journal non réentrant (règle « pas de journal en IRQ », respectée mais non imposée par le code) | verrou d'IRQ (`irq_save`) dans `console_write` ou tampon par contexte |
| L8 | `kernel/arch/i386/gdt.c:28` | Profondeur de pile de la tâche #DF (4 KiB) non mesurée | la peindre comme la pile principale et mesurer après `test=stack` |
| L9 | `kernel/arch/i386/irq.c:10` | Compteurs d'IRQ sur 32 bits (débordement après 497 jours à 100 Hz) | sans conséquence fonctionnelle ; 64 bits si utilisés pour des mesures |
| L10 | — | NMI et #MC mènent à une panique générique | gestionnaire NMI dédié (lecture des ports 0x61/0x92) ; MCA en K1 |

## 7. Analyse de sécurité globale

### 7.1 Frontières de confiance actuelles

```text
 Firmware / chargeur ──(magic, mbi, cmdline, carte E820)──► bootinfo + memmap   [validées, copiées, bornées]
 Périphériques ───────(IRQ, scancodes, UART, 8042)─────────► ISR minimaux       [bornés, sans attente]
 Code du noyau ───────(tout)───────────────────────────────► noyau              [confiance totale : ring 0 unique]
```

### 7.2 Invariants établis et leur preuve

| Invariant | Établi en | Preuve |
|---|---|---|
| Aucune donnée du chargeur utilisée sans validation | K0.1, K0.6 | test négatif du magic, cartes hostiles et aléatoires sous ASan |
| Pile valide, alignée, DF = 0 dans tout code C, interruptions comprises | K0.1, K0.4 | GDB, autotests dans un gestionnaire |
| Toute exception est diagnostiquée, aucune n'est ignorée | K0.4 | 13 démarrages fatals |
| Une pile épuisée ne mène pas à un triple fault | K0.4 + K0.6 | `stack_overflow` |
| Le code et les constantes du noyau ne sont pas inscriptibles | K0.6 | sondes, `pf_write_text`, `pf_write_rodata` |
| Déréférencer NULL est détecté | K0.6 | sonde, `pf_null` |
| Cadres réservés jamais distribués ; pas de double distribution ; double libération refusée | K0.6 | test hébergé (200 000 opérations), épuisement dans le noyau |
| Mémoire recyclée remise à zéro | K0.6 | cadre sali puis réalloué |
| Aucun débordement de tampon dans le formatage et le journal | K0.3 | test différentiel sous ASan, troncature signalée |
| Pas de blocage sur un matériel absent ou défaillant | K0.3, K0.5 | attentes bornées (UART, 8042) ; refus de `sleep` avec IF = 0 |

### 7.3 Ce que K0 ne protège pas (par construction)

- **Aucune isolation interne** : tout s'exécute en ring 0 dans un seul espace d'adressage. Un bogue de pilote peut corrompre n'importe quelle donnée inscriptible du noyau. C'est le compromis assumé du noyau monolithique (cahier des charges §4).
- **Pas de ring 3** : la frontière principale de SAMOS (utilisateur et IA d'un côté, noyau de l'autre) **n'existe pas encore**. Les mécanismes de K0 en sont les fondations : TSS avec `ss0/esp0`, portes DPL 0, sondes mémoire, remise à zéro des cadres.
- **Données exécutables** (M4) ; pas de canari de pile (M5) ; pas d'ASLR (sans objet).

## 8. Robustesse et limites fonctionnelles

| Domaine | Limite |
|---|---|
| Plateforme | testé uniquement sous QEMU (TCG) ; sur l'EliteBook du développeur (UEFI, probablement sans CSM), Multiboot 1 en mode BIOS, VGA texte et port série ne sont vraisemblablement pas disponibles |
| Mémoire | ≤ 1 GiB gérée ; ≤ 4 GiB adressable ; pas de `kmalloc` |
| Temps | PIT 100 Hz uniquement ; pas d'APIC timer, de HPET ni de TSC étalonné |
| Concurrence | monoprocesseur ; file clavier et ticks corrects seulement sur un seul cœur x86 |
| Entrées | clavier US QWERTY ; pas de réception série ; pas de souris |
| Diagnostic | traces de pile limitées à la pile de démarrage ; adresses non symbolisées dans le noyau (`addr2line` sur l'hôte) |

## 9. Préparation à la migration x86-64

### 9.1 État par composant

| Composant | Portabilité | Travail nécessaire en x86-64 |
|---|---|---|
| `lib/string`, `lib/format`, `lib/ringbuf`, `drivers/kbd_decode` | ✅ portables | aucun ; vérifier `%p` (16 chiffres) |
| `lib/math64` | ✅ portable | devient trivial (`DIV` 64 bits natif), à conserver pour l'i386 |
| `mm/memmap`, `mm/pmm_core` | ✅ portables (testés sur hôte 64 bits) | bitmap dimensionnée dynamiquement au-delà de 4 GiB ; numéros de cadres déjà sur 32 bits (16 TiB) |
| `core/log`, `core/panic` | ⚠️ quasi portables | `stack_collect` : EBP vers RBP ; bornes de pile 64 bits |
| `core/bootinfo` | ⚠️ propre à Multiboot 1 | dépend du protocole de démarrage choisi (D2) |
| `drivers/vga`, `serial`, `pit`, `ps2kbd` | ✅ (ports d'E/S identiques) | aucun ; APIC/IOAPIC à terme |
| `arch/i386/entry.asm` | ❌ | trampoline 32 → 64 bits : CPUID (LM), tables de pages PAE/4 niveaux minimales, `EFER.LME` (+ `NXE`), `CR4.PAE`, `CR0.PG`, saut lointain vers un segment de code 64 bits (bit L) |
| `arch/i386/gdt.c` | ❌ | descripteur de code 64 bits ; TSS 64 bits (16 octets dans la GDT) avec `RSP0` et **IST1-7** ; **plus de commutation de tâche matérielle** |
| `arch/i386/idt.c`, `isr_stubs.asm` | ❌ | portes de 16 octets ; **`PUSHAD` n'existe plus** (sauvegarde explicite des 15 registres) ; `IRETQ` ; alignement de RSP sur 16 fait par le CPU |
| `arch/i386/exceptions.c` (#DF) | ❌ | la porte de tâche est **remplacée par l'IST** : #DF (et NMI, #MC) sur une pile IST dédiée, ce qui conserve la protection contre l'épuisement de la pile |
| `arch/i386/paging.c` | ❌ | 4 niveaux (PML4, PDPT, PD, PT), entrées de 64 bits, **bit NX** (corrige M4), noyau *higher-half* (`0xFFFFFFFF80000000`), projection directe de la RAM physique (*direct map*) |
| `arch/i386/probe.asm` | ⚠️ | même principe ; ajouter la correction des #GP (adresses non canoniques) |
| `arch/i386/pic.c`, `irq.c` | ✅ | inchangés, mais APIC recommandé pour le SMP |
| Chaîne de compilation | ⚠️ | `--target=x86_64-unknown-none-elf`, **`-mno-red-zone`** (obligatoire : les interruptions écrasent la zone rouge de 128 octets sous RSP), `-mcmodel=kernel`, `-mgeneral-regs-only`, `elf_x86_64` |

### 9.2 Points de vigilance spécifiques

1. **Zone rouge (*red zone*).** L'ABI System V AMD64 autorise une fonction feuille à utiliser 128 octets **sous** RSP sans les réserver. Une interruption en ring 0 empile son cadre précisément là. Sans `-mno-red-zone`, le noyau se corromprait silencieusement. Il faut un test dédié.
2. **Convention d'appel.** Les arguments passent dans RDI, RSI, RDX, RCX, R8 et R9 au lieu de la pile. `_start`, les stubs et les sondes doivent être réécrits.
3. **Démarrage.** Le chargeur `-kernel` de QEMU ne charge **pas** un ELF64 Multiboot. Les options sont évaluées en D2.
4. **Démonstrations à rejouer.** Toutes les démonstrations de K0 ont leur équivalent 64 bits et devront être rejouées : faute/trappe, #DF sur pile dédiée (IST), page nulle, lecture seule, garde, triple fault.

### 9.3 Évaluation

Les fondations **portables** (bibliothèque, mémoire physique, pilotes, journal, tests) représentent environ 64 % du code (3 204 lignes sur 4 960) et sont réutilisables telles quelles. La couche `arch/` (environ 1 750 lignes) doit être **réécrite**, et non adaptée. C'était prévu dès l'ADR 0001, et le découpage `arch/` le permet sans toucher au reste.

## 10. Recommandations

### 10.1 Corrections recommandées avant K1

Aucune n'est bloquante pour K0.

| # | Origine | Action | Effort estimé |
|---|---|---|---|
| R1 | M1 | séparer l'image de test (`CONFIG_KTEST`) et l'image de production | faible |
| R2 | M5 | canari de pile maison (`__stack_chk_guard/fail`) + `-fstack-protector-strong` | faible |
| R3 | L2, L3 | IDT et mémoire basse en lecture seule | faible |
| R4 | §3.2 | petite couche HAL (`hal_irq_*`, `hal_mmu_*`) pour isoler `core`/`drivers` de `arch` avant x86-64 | moyen |
| R5 | L8 | peindre et mesurer la pile #DF | très faible |

### 10.2 À intégrer à la conception de K1

M2 (propriété des cadres) et M3 (projections utilisateur vérifiées) sont des prérequis du ring 3. M4 (NX) est résolu par x86-64.

## 11. Décisions qui reviennent au développeur

| # | Question | Options | Recommandation |
|---|---|---|---|
| **D1** | Autoriser K1 ? Dans quel ordre ? | (a) migration x86-64 d'abord, puis processus ; (b) `kmalloc` et processus en i386, migration ensuite | **(a)**. Écrire processus, ordonnanceur et appels système en i386 obligerait à les réécrire pour x86-64 (cadres d'interruption, changement de contexte, convention d'appel). Les fondations portables sont prêtes. |
| **D2** | Protocole de démarrage x86-64 | (a) garder Multiboot 1 + trampoline 32→64 bits écrit par nous (testable avec `qemu -kernel`) ; (b) Multiboot 2 + GRUB + ISO ; (c) application UEFI directe | **(a)** pour la continuité des tests automatiques, puis **(c)** pour le matériel réel (EliteBook UEFI) dans une phase dédiée |
| **D3** | Corrections R1 à R5 | les appliquer maintenant ; ou au début de K1 | les appliquer au début de K1 (R1 et R2 en premier) |
| **D4** | Stratégie matériel réel | conserver QEMU seul ; ou ajouter un banc UEFI (OVMF sous QEMU, puis clé USB sur l'EliteBook) | OVMF sous QEMU avant tout essai sur l'EliteBook ; ne jamais écrire sur ses disques |
| **D5** | Licence | à choisir | aucune recommandation sans votre décision (MIT, BSD-2, GPL-2.0, Apache-2.0 ont des conséquences différentes sur les contributions et la réutilisation) |
| **D6** | Fusion vers `main` | relire et fusionner la Pull Request de la branche `claude/trusting-lovelace-x7fiip` | — |

## 12. Conclusion

Le noyau SAMOS K0 :

- démarre ;
- valide ses entrées externes ;
- diagnostique toute exception ;
- survit à l'épuisement de sa pile ;
- gère la mémoire physique avec des invariants vérifiés ;
- protège son code et sa page nulle par la pagination ;
- reçoit des interruptions d'horloge et de clavier sans perte.

Chaque propriété affirmée dans la documentation est adossée à un test exécuté, et les limites sont nommées. Le principal manque est **la séparation des privilèges** : ring 3, espaces d'adressage, appels système. C'est exactement l'objet de K1, qui ne commencera qu'avec votre autorisation.
