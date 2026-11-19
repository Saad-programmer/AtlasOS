# K1 — Préparation (état des lieux et progression proposée)

**Nature :** document de préparation. **Aucune fonctionnalité nouvelle n'est implémentée.** K1 n'est pas commencé et attend l'autorisation du développeur.
**Base :** clôture de P4 (`docs/review/K0.7-P4-closure.md`), branche `claude/k0.7-p4-close`.

## 1. Capacités réellement présentes (i386, QEMU)

Relevé dans le code (`kernel/`, environ 8 900 lignes de C et d'assembleur), pas dans les intentions.

| Domaine | Présent | Absent |
|---|---|---|
| Démarrage | Multiboot 1, validation des structures du chargeur, pile de démarrage avec pages de garde, image chargée à 1 MiB (identité) | UEFI, Multiboot 2, noyau *higher-half*, x86-64 |
| Processeur | GDT plate, TSS principal, TSS de double faute (porte de tâche, pile dédiée, sentinelles), IDT à 32 vecteurs d'exception, rapport et panique | IST, SYSCALL/SYSENTER, ring 3, FPU/SSE |
| Interruptions | PIC 8259 remappé, couche IRQ, PIT 100 Hz, clavier PS/2 (8042, jeu 1), file SPSC | APIC/IOAPIC, MSI, NMI traitée (la NMI aboutit à une panique) |
| Mémoire physique | carte E820 défensive, PMM à descripteurs typés (FREE, RESERVED, KERNEL, ALLOCATED, PAGETABLE) avec compteur de références, remise à zéro, refus des cadres de l'image et de la mémoire basse indépendamment des métadonnées | `kmalloc`, allocateur de pages contiguës, zones (DMA, haute) |
| Pagination | 32 bits, 4 KiB, identité sur ≤ 1 GiB de RAM, `.text`/`.rodata` RO, structures critiques et tables scellées, passage contrôlé `pt_gate_write`, `vmm_map`/`vmm_unmap`/`vmm_translate` pour **un seul** espace d'adressage, audit au démarrage | plusieurs espaces d'adressage, NX/PAE, pages utilisateur, défaut de page récupérable, partage, copie sur écriture |
| Consoles | VGA texte, COM1 en émission, debugcon QEMU | réception série, framebuffer |
| Robustesse | canaris de pile, pages de garde, panique avec trace de pile et garde de récursion, autotests en noyau, 77 cas QEMU, 66 mutations, reproductibilité, contrôles du code machine | journalisation réentrante, reprise après faute |
| Processus | **aucun** | tâches, ordonnanceur, appels système, IPC, VFS, pilotes de stockage, audio, IA, voix |

**Utilisateurs de `vmm_map` :** aucun code de production n'appelle `vmm_map` ou `vmm_unmap`. Seuls les autotests créent des projections dynamiques.

## 2. Conditions de passage à K1

1. **Décision du développeur sur les risques acceptés** pour P4 (`K0.7-P4-closure.md` §7) et sur la clôture de K0.7 dans son ensemble.
2. **N3 corrigé avant tout usage des projections dynamiques en production**, avant tout espace d'adressage séparé et avant tout mécanisme d'isolation mémoire. C'est **bloquant** (§4).
3. **Cible architecturale confirmée** (x86-64, *higher-half*, mode de démarrage). Ce changement d'architecture exige l'approbation du développeur (CLAUDE.md, §1).
4. Décisions sur la GDT/TSS et sur la NMI pendant la fenêtre WP : la migration x86-64 (IST, TSS 64 bits) les modifie profondément, il faut donc décider s'il vaut mieux les traiter avant ou pendant celle-ci.
5. Bancs verts à l'entrée de chaque jalon : `make test`, `make test-mutation`, `make test-repro`.

## 3. Progression proposée (sous réserve d'autorisation, jalon par jalon)

| Jalon | Contenu | Dépend de |
|---|---|---|
| **K1.0 — Correction de N3** | contrôle indépendant des projections avant libération et réattribution (§4, approche 1) ; mutations, mode destructif, rapport | décision du développeur |
| K1.1 — Décisions d'architecture | document seul : cible x86-64, schéma *higher-half*, démarrage (Multiboot 2 ou autre), devenir de la fenêtre CR0.WP (alternatives en x86-64 : projections temporaires par processeur, NX) | K1.0 |
| K1.2 — `kmalloc` | allocateur d'objets du noyau sur le PMM, sans projections dynamiques (identité seulement) | K1.0 |
| K1.3 — Migration x86-64 | mode long, tables à quatre niveaux, IST pour #DF/NMI/#MC, NX ; reprise des protections de P4 en x86-64, par un **audit dédié** | K1.1 |
| K1.4 — Espaces d'adressage | répertoires multiples, projections utilisateur, libération des tables (chemin dédié, P4f-B3 §9) ; **N3 doit alors couvrir tous les espaces** (§4, approche 2) | K1.0, K1.3 |
| K1.5 — Ring 3, appels système, tâches | premier processus utilisateur, appels système validés, ordonnanceur simple | K1.4 |

Chaque jalon suit la même règle : implémentation, explication, validation complète, documentation, commit, puis arrêt jusqu'à l'autorisation suivante.

## 4. Correction de N3 : comparaison des approches

**N3 :** une écriture dans `g_meta` (champ `refs`, ou type) permet de libérer puis de redistribuer un cadre de données encore projeté (`K0.7-P4f-E-closure-readiness.md` §4).

| Critère | **1. Vérification indépendante des projections avant libération** | **2. Références VMM distinctes des métadonnées PMM** | **3. Interface interne contrôlée entre PMM et VMM** |
|---|---|---|---|
| Principe | avant `pmm_free_frame` et avant la remise à zéro d'une réattribution, parcourir les PTE hors identité des tables publiées (RO) ; refus (EBUSY) ou arrêt si le cadre y figure | compteur de projections par cadre, tenu par le VMM dans une page **scellée**, écrite seulement par le passage (fenêtre WP) ; le PMM refuse de libérer un cadre dont ce compteur n'est pas nul | les cadres « projetables » ne sont libérables que par une fonction du VMM (par exemple `vmm_release`), qui retire d'abord les projections ; `pmm_free_frame` refuse ces cadres |
| Sécurité face à une `g_meta` falsifiée | **oui** : la source est constituée des tables RO | **oui**, si la page de comptes est scellée et validée par le passage | **partielle** : l'interface décide toujours sur la foi du type PMM ; cela supprime les **mauvais usages**, pas la falsification |
| Complexité | faible (environ 10 lignes ; fonction déjà existante `frame_has_foreign_alias`) | élevée : nouvelle structure de confiance, nouvelle sorte d'écriture par le passage, cohérence avec `pmm_ref`/`pmm_unref` | moyenne : refonte des appelants, règle de propriété |
| Mémoire | 0 | 2 octets par cadre (512 KiB pour 1 GiB), sur des pages scellées | 0 |
| Temps | parcours linéaire des tables hors identité à chaque libération et allocation : environ ×2 à ×3 mesuré sous TCG (prototype P4f-E), croissant avec le nombre de tables | O(1) par opération, plus une fenêtre WP par projection et par retrait | négligeable |
| Avec plusieurs espaces d'adressage | **doit parcourir tous les répertoires** ; coût proportionnel à la somme des tables | naturel : le compteur est global | naturel, mais même faiblesse vis-à-vis de la falsification |
| Nouveaux risques | dépendance du PMM envers le VMM (inversion de couche, à placer dans une enveloppe du VMM) | erreur de comptage, donc fuite ou refus à tort ; surface de la fenêtre WP accrue | règle de propriété contournable par un appel direct au PMM |
| Maintenance | simple ; le critère est le même que celui de D1 | lourde ; il faut des mutations et un audit des comptes | moyenne |

**Recommandation :**
- **K1.0 :** approche **1**. Elle suffit tant qu'il n'existe qu'un seul espace d'adressage et elle n'ajoute aucune structure de confiance. Elle peut être complétée par l'approche **3**, qui clarifie la propriété des cadres et supprime les mauvais usages, sans la présenter comme une défense contre la falsification.
- **K1.4** (plusieurs espaces d'adressage) : réexaminer le passage à l'approche **2** si le coût de l'approche 1 devient prohibitif. Ce choix est une décision d'architecture du développeur.

**Aucune nouvelle dépendance architecturale n'est introduite par ce document.**

## 5. Premier jalon proposé : K1.0 — Correction de N3

**Objectif :** aucun cadre encore projeté hors identité ne peut être libéré ni redistribué, même avec une `g_meta` falsifiée (champ `refs` ou type).

**Périmètre :**
1. vérification indépendante (approche 1) dans le chemin de libération et avant la remise à zéro d'une réattribution, sans modifier le passage ni CR0.WP ;
2. un appel « VMM → PMM » documenté, ou une enveloppe dans le VMM, pour éviter l'inversion de couche ;
3. un mode isolé reproduisant N3 (falsification de `refs`, puis du type), avec des assertions nommées : refus, absence d'effet, isolation des données ;
4. des mutations : contrôle de la libération supprimé, contrôle de la réattribution supprimé ;
5. la mesure du coût ; l'intégration éventuelle du préfiltre d'identité (R-PERF), avec sa mutation, si le développeur l'autorise ;
6. un rapport et une mise à jour de la clôture de P4 : N3 passe à « corrigé dans le périmètre d'un seul espace d'adressage ».

**Hors périmètre :** x86-64, `kmalloc`, plusieurs espaces d'adressage, ring 3.
