# =============================================================================
# SAMOS / AtlasOS — tests/mutation/cases.sh        (lu par tests/mutation/run.sh)
# -----------------------------------------------------------------------------
# Chaque mutation réintroduit un défaut corrigé et nomme les assertions qui
# DOIVENT le signaler. Le texte original doit apparaître exactement une fois.
#
#   mutant NOM FICHIER 'TEXTE ORIGINAL' 'TEXTE MUTÉ' STATUT 'ASSERTION'...
#
# STATUT 35 : les autotests se terminent et signalent des échecs.
# =============================================================================

# M1 — E5 : la libération d'un cadre encore référencé n'est plus refusée
# (pmm_core_free ne vérifie plus refs != 0). Défaut d'origine : un cadre libéré
# restait projeté et pouvait être redistribué (k06-memory-defects.txt, E5).
mutant e5_free_referenced kernel/mm/pmm_core.c \
    'if (frame_desc_refs(p->meta[frame]) != 0) {' \
    'if (0) {' \
    35 \
    'pmm_free_frame(f) == SAMOS_EBUSY'
# K1.0-N3 : la libération d'un cadre PROJETÉ est aussi refusée par le relevé
# des PTE (N3-I1), seconde barrière indépendante de refs ; dans le scénario
# E5 (cadre A projeté), la mutation n'est donc plus visible. Elle reste
# détectée pour une référence prise SANS projection (test_kernel_refs), que
# seul le compteur protège.

# M2 — E3 : vmm_map accepte de nouveau tout cadre non libre (code, constantes,
# tables de pages, structures du noyau), comme avant M2.5b.
# Depuis K0.7-P4e-1, le passage pt_gate_write revalide le type (cadre ALLOCATED
# hors identité) : la projection reste REFUSÉE (défense en profondeur), donc
# e3_target_refused passe. Le défaut reste observable : le contrôle de vmm_map
# n'a plus lieu AVANT get_table, et des tables sont créées pour des refus.
# Les assertions « aucun effet de bord » des scénarios E3 le nomment.
# Depuis K0.7-P4f-B1, le contrôle indépendant du PMM de map_page refuse aussi,
# AVANT toute création de table, les cadres de l'image et les structures de
# pagination : le second scénario (cibles toutes dans l'image ou dans les
# tables) ne crée plus de table. Le premier (cibles hors image : VGA, cadre
# libre, au-delà de la RAM...) en crée encore : c'est lui qui nomme le défaut.
mutant e3_type_check kernel/arch/i386/paging.c \
    'if (counted && t != FRAME_ALLOCATED) {' \
    'if (counted && t == FRAME_FREE) {' \
    35 \
    'paging_table_count() == tables0'

# M3 — M2.5c : vmm_map ne filtre plus les droits (PTE_USER, PTE_PRESENT,
# bits de cache...). Détecté par des tentatives sur un cadre ALLOCATED : les
# cibles critiques, refusées par leur type, ne suffiraient pas.
mutant flags_filter kernel/arch/i386/paging.c \
    'if (counted && (flags & ~PTE_WRITE) != 0) {' \
    'if (0) {' \
    35 \
    'e3_attempt(0xD0060000u, f, PTE_USER, SAMOS_EINVAL, "alloue")' \
    'e3_attempt(0xD0060000u, f, PTE_USER | PTE_WRITE, SAMOS_EINVAL, "alloue")'

# M4 — K0.7-R1c : l'option globale -fstack-protector-strong est retirée.
# Détection 1, autotests : canary_frame_probe n'a plus de copie du canari.
mutant canary_not_instrumented Makefile \
    '-fstack-protector-strong -fcf-protection=none' \
    '-fno-stack-protector -fcf-protection=none' \
    35 \
    'canary_frame_probe(false) == 1'

# M6 — K0.7-R1c : même défaut, détecté dans l'IMAGE DE PRODUCTION par
# l'inspection du code machine (fonctions réellement instrumentées). Le
# symbole __stack_chk_fail existe toujours : sa présence ne prouve rien.
mutant_tool canary_global_flag Makefile \
    '-fstack-protector-strong -fcf-protection=none' \
    '-fno-stack-protector -fcf-protection=none' \
    'tests/tools/canary_check.sh build/samos.elf build/samos-test.elf' \
    'samos.elf : fonctions requises NON instrumentées : kvprintf kvsnprintf out_number memmap_parse' \
    'samos-test.elf : canary_frame_probe NON instrumentée'

# M5 — K0.7-R1b : _start n'initialise plus le canari ; il garde la valeur de
# l'image (prévisible).
mutant canary_not_seeded kernel/arch/i386/boot/entry.asm \
    'mov [__stack_chk_guard], eax' \
    'nop' \
    35 \
    'g_guard_ref != STACK_GUARD_DEFAULT'

# M7 — K0.7-R1d : la vérification des sentinelles de S2 ne signale plus rien
# (df_stack_sentinel_damage renvoie toujours 0). Détecté par le test de
# détection non destructif (mot 1 altéré puis restauré).
mutant df_sentinel_check_removed kernel/arch/i386/gdt.c \
    'mask |= 1u << i;' \
    'mask |= 0u;' \
    35 \
    'mask == (1u << 1)'

# M8 — K0.7-R1d : les sentinelles de S2 ne sont plus posées (la boucle
# d'initialisation n'écrit plus rien ; supprimer l'appel rendrait la fonction
# inutilisée, refusée par -Werror : mutation INVALIDE, constaté).
mutant df_sentinel_not_initialized kernel/arch/i386/gdt.c \
    'w[i] = DF_SENTINEL_VALUE(i);' \
    '(void)w;' \
    35 \
    'values_ok' \
    'df_stack_sentinel_damage() == 0'

# M9 — K0.7-R1f-1 : un symbole est placé dans la page réservée à la garde
# haute de S1. check_elf.sh doit le nommer (les assertions du linker, qui ne
# portent que sur les bornes, ne le voient pas).
mutant_tool stack_layout_intruder kernel/arch/i386/linker.ld \
    '__stack_top_guard_start = .;' \
    '__stack_top_guard_start = .; layout_intruder = . + 16;' \
    'tools/check_elf.sh build/samos.elf --production' \
    'piles: objets dans une page de garde ou reservee : layout_intruder'

# M10 — K0.7-R1f-2 : la garde haute de S1 est de nouveau projetée (la règle
# est retirée de va_must_stay_unmapped, donc de paging_init ET de vmm_map).
mutant top_guard_mapped kernel/arch/i386/paging.c \
    'return in_page_range(page, __stack_top_guard_start, __stack_top_guard_end);' \
    'return false;' \
    35 \
    'raw_pte(tg) == 0' \
    'cr2 == tg && err == 2u' \
    'vmm_map(tg, f, PTE_WRITE) == SAMOS_EPERM'

# M11 — K0.7-R1f-3 : la garde basse de S2 est de nouveau projetée (clause
# retirée de va_must_stay_unmapped : paging_init ET vmm_map).
mutant df_guard_mapped kernel/arch/i386/paging.c \
    'if (in_page_range(page, __df_guard_start, __df_guard_end)) {' \
    'if (0) {' \
    35 \
    'raw_pte(dg) == 0' \
    'cr2 == dg && err == 2u' \
    'vmm_map(dg, f, PTE_WRITE) == SAMOS_EPERM && vmm_map(dg, f, 0) == SAMOS_EPERM'

# M12 — K0.7-R1h (F1) : gdt_init ne remet plus S2 à zéro (la boucle de remise
# à zéro ne s'exécute plus ; les sentinelles restent posées). Détecté parce
# que S2 a été préremplie d'un motif non nul avant gdt_init (image de test).
# Avant R1h, cette mutation n'était PAS détectée (audit R1g, F1).
mutant s2_zeroing_removed kernel/arch/i386/gdt.c \
    'for (uint32_t i = 0; i < DF_STACK_SIZE / 4u; i++) {' \
    'for (uint32_t i = 0; i < 0u; i++) {' \
    35 \
    's2_rest_nonzero == 0' \
    's2_rest_pattern == 0'

# M13 — K0.7-R1h (F6) : intrus dans la garde haute portant un préfixe
# autrefois exclu (boot_stack_). Avant R1h, check_elf.sh l'acceptait
# (démontré) ; il doit maintenant le nommer.
mutant_tool stack_layout_prefixed_intruder kernel/arch/i386/linker.ld \
    '__stack_top_guard_start = .;' \
    '__stack_top_guard_start = .; boot_stack_intruder = . + 16;' \
    'tools/check_elf.sh build/samos.elf --production' \
    'piles: objets dans une page de garde ou reservee : boot_stack_intruder'

# M14 — K0.7-P4b : un compteur MUTABLE (g_tables, incrémenté à chaque nouvelle
# table) est placé dans la page des racines du VMM, candidate à la lecture
# seule. check_elf.sh doit nommer l'intrus (la compilation et l'édition de
# liens réussissent : la détection est fonctionnelle).
mutant_tool p4b_rw_in_vmm_page kernel/arch/i386/paging.c \
    'static uint32_t g_tables;' \
    'static uint32_t g_tables SAMOS_SECTION(".bss.p4_vmm");' \
    'tools/check_elf.sh build/samos.elf --production' \
    'P4b: page [__kro_vmm_start, __kro_vmm_end) : intrus:g_tables'

# M15 — K0.7-P4b : le compteur d'exceptions (écrit à chaque exception) est placé
# dans la page des gestionnaires, candidate à la lecture seule.
mutant_tool p4b_rw_in_handlers_page kernel/arch/i386/exceptions.c \
    'static volatile uint32_t g_exc_depth;' \
    'static volatile uint32_t g_exc_depth SAMOS_SECTION(".bss.p4_handlers");' \
    'tools/check_elf.sh build/samos.elf --production' \
    'P4b: page [__kro_handlers_start, __kro_handlers_end) : intrus:g_exc_depth'

# M16 — K0.7-P4c : le prototype de passage n'exige plus un cadre de type
# PAGETABLE. Les tests négatifs réécrivent la valeur actuelle (aucune
# destruction) : le refus attendu (EPERM) devient un succès pour un cadre
# ALLOCATED et pour la VGA. (La cible .text reste refusée par la politique de
# valeur : son premier mot a le bit 0x4, lu comme PTE_USER ; elle n'est donc
# pas une assertion ciblée de cette mutation.)
mutant ptgate_type_check_removed kernel/tests/test_ptgate.c \
    'pmm_core_type(p, table_phys >> PAGE_SHIFT) != FRAME_PAGETABLE) {' \
    'false) {' \
    35 \
    'ptgate_write_entry(a, 0, raw_entry(a, 0)) == SAMOS_EPERM' \
    'ptgate_write_entry(0xB8000u, 0, 0) == SAMOS_EPERM'

# M17 — K0.7-P4c : WP n'est plus rétabli à la fermeture de la fenêtre.
# Depuis K0.7-P4e-1, l'invariant global de ktest_check (CR0.WP = 1 hors
# fenêtre) le nomme à l'assertion suivante, puis rétablit WP (image de test
# seulement) : state_intact ne le voit donc plus, l'invariant le signale.
mutant ptgate_wp_not_restored kernel/tests/test_ptgate.c \
    'cpu_write_cr0(cr0);                         /* WP rétabli AVANT les IRQ */' \
    '(void)cr0;' \
    35 \
    'CR0.WP actif hors fenetre (invariant global)'

# M18 — K0.7-P4c : l'imbrication n'est plus refusée.
mutant ptgate_nesting_allowed kernel/tests/test_ptgate.c \
    '    if (g_ptgate_active) {' \
    '    if (0) {' \
    35 \
    'en == SAMOS_EBUSY && g_ptgate_nested_refusals == nested0 + 1u'

# M19-M21 — K0.7-P4d : une des trois pages n'est plus scellée. La sonde
# d'écriture réussit (elle restaure aussitôt la valeur) : l'échec est une
# assertion nommée, pas une corruption.
mutant seal_idt_removed kernel/arch/i386/paging.c \
    'e = paging_seal_identity((uint32_t)(uintptr_t)__kro_idt_start, (uint32_t)(uintptr_t)__kro_idt_end);' \
    'e = SAMOS_OK;' \
    35 \
    'idt_ro' \
    'write_refused(__kro_idt_start)'
mutant seal_handlers_removed kernel/arch/i386/paging.c \
    'e = paging_seal_identity((uint32_t)(uintptr_t)__kro_handlers_start, (uint32_t)(uintptr_t)__kro_handlers_end);' \
    'e = SAMOS_OK;' \
    35 \
    'handlers_ro' \
    'write_refused(__kro_handlers_start)'
mutant seal_vmm_removed kernel/arch/i386/paging.c \
    'e = paging_seal_identity((uint32_t)(uintptr_t)__kro_vmm_start, (uint32_t)(uintptr_t)__kro_vmm_end);' \
    'e = SAMOS_OK;' \
    35 \
    'vmm_ro' \
    'write_refused(__kro_vmm_start)'

# M22-M25 — K0.7-P4e-1 : garanties du passage d'écriture des PDE/PTE. Les
# tests de refus restaurent l'entrée si l'écriture est acceptée à tort.
mutant gate_user_bit_allowed kernel/arch/i386/paging.c \
    '    if (value & PTE_USER) {' \
    '    if (0) {' \
    35 \
    'refused(K_PTE, kpage, kpte | PTE_USER, SAMOS_EPERM)'
mutant gate_ro_identity_writable kernel/arch/i386/paging.c \
    'if (frame != va || (paging_enabled() && (value & PTE_WRITE) && va_read_only_identity(va))) {' \
    'if (frame != va || (false && va_read_only_identity(va))) {' \
    35 \
    'refused(K_PTE, kpage, kpte | PTE_WRITE, SAMOS_EPERM)'
mutant gate_nesting_allowed kernel/arch/i386/paging.c \
    '    if (g_pt_gate_active) {' \
    '    if (0) {' \
    35 \
    'en == SAMOS_EBUSY && entry_of(K_PTE, kpage) == kpte'
# M25 — Depuis K0.7-P4e-2, la PREMIÈRE fenêtre s'ouvre dans paging_init
# (scellement des tables) : WP non rétabli fait refuser l'écriture suivante,
# paging_init échoue et le noyau s'arrête AVANT tout autotest (refus fermé,
# comportement de production voulu). Une détection à l'exécution ne peut donc
# plus être une assertion nommée : elle est faite sur le code machine de
# production (check_elf.sh : une seule écriture de CR0 dans pt_gate_write).
# L'invariant global de ktest reste prouvé par M17 (prototype P4c).
mutant_tool gate_wp_not_restored kernel/arch/i386/paging.c \
    'cpu_write_cr0(cr0);                         /* WP rétabli AVANT IF */' \
    '(void)cr0;' \
    'tools/check_elf.sh build/samos.elf --production' \
    'pt_gate_write:1'

# M26 — K0.7-P4e-1 : deux écritures dans la fenêtre WP au lieu d'une. Détecté
# par la forme du code machine (check_elf.sh --production).
mutant_tool gate_window_not_unitary kernel/arch/i386/paging.c \
    '*entry = value;                             /* UNE écriture de 32 bits */' \
    '*entry = value; *entry = value;' \
    'tools/check_elf.sh build/samos.elf --production' \
    'pt_gate_write:fenetre-non-unitaire'

# M27-M33 — K0.7-P4e-2 : scellement des structures de pagination. Les
# écritures directes de test réécrivent la valeur actuelle : une table restée
# inscriptible produit une assertion, jamais une destruction.
mutant seal_pd_removed kernel/arch/i386/paging.c \
    'e = seal_table_frame(pd);                                 /* 3. répertoire */' \
    'e = SAMOS_OK;' \
    35 \
    'pd_ro' \
    'pd_write_faults'
mutant seal_tables_removed kernel/arch/i386/paging.c \
    'samos_err_t e = seal_table_frame(pde & PTE_FRAME);   /* 1. tables */' \
    'samos_err_t e = SAMOS_OK;' \
    35 \
    'tables_rw == 0'
mutant seal_t0_removed kernel/arch/i386/paging.c \
    'samos_err_t e = seal_table_frame(t0);                     /* 2. T0 */' \
    'samos_err_t e = SAMOS_OK;' \
    35 \
    't0_ro' \
    'pt0_write_faults'
# Depuis K0.7-P4f-B2, le passage refuse aussi de publier une table non
# scellée : sans le scellement de table_create, plus aucune table n'est
# créée après le démarrage (refus fermé). Les autotests normaux s'en
# trouveraient interrompus par une cascade d'échecs ; la mutation est donc
# observée dans le mode isolé test=pdeint, qui le nomme.
# Depuis K0.7-P4f-B3, un refus après l'allocation est FATAL : la première
# création de table du mode pdeint arrête le noyau par le diagnostic de la
# publication refusée, absent de la version saine (mutant_mode).
mutant_mode seal_new_table_removed kernel/arch/i386/paging.c \
    'samos_err_t se = seal_table_frame(pt);' \
    'samos_err_t se = SAMOS_OK;' \
    test=pdeint 37 \
    'KERNEL PANIC: page-table creation: PDE publication refused'
# INVLPG oublié après le scellement : la PTE est correcte (sans W), mais un
# processeur réel garderait la traduction inscriptible chargée dans le TLB
# par la remise à zéro. Sous QEMU, la fenêtre du passage (changement de
# CR0.WP) vide tout le TLB : le défaut est invisible aux autotests (constaté :
# mutation NON DÉTECTÉE à l'exécution). Détection sur le code machine.
mutant_tool seal_invlpg_removed kernel/arch/i386/paging.c \
    'cpu_invlpg((const void *)(uintptr_t)table);' \
    '(void)0;' \
    'tools/check_elf.sh build/samos.elf --production' \
    'seal_table_frame:sans-invlpg'
mutant gate_pagetable_w_allowed kernel/arch/i386/paging.c \
    '(frame_is_pagetable(frame) || frame_is_paging_structure(frame))) {' \
    '(false && frame_is_pagetable(frame) && frame_is_paging_structure(frame))) {' \
    35 \
    'gate_keeps_pd_ro'
# Écriture directe d'une PTE réintroduite (sans passer par pt_gate_write) :
# signalée statiquement par pt_write_lint.sh.
mutant_tool pt_direct_write kernel/arch/i386/paging.c \
    'samos_err_t ge = pt_gate_write(PT_KIND_PTE, va, 0);' \
    'pt[PT_INDEX(va)] = 0; samos_err_t ge = SAMOS_OK;' \
    'tests/tools/pt_write_lint.sh' \
    'pt_write_lint: ecriture directe d'"'"'entree'

# M34-M40 — K0.7-P4f-B1 : frontière de confiance du PMM (D1). Exécutées dans
# le mode ISOLÉ test=pmmtrust (MUT_CMDLINE), qui falsifie des descripteurs ;
# la version saine doit d'abord réussir ce mode.
# Depuis K0.7-P4f-B3 : sans le contrôle de map_page, la requête refusée par
# le passage dans une région SANS table atteint la création d'une table ; le
# passage refuse alors d'écrire dans la table dont le type PMM est falsifié
# (première table), et ce refus après allocation est FATAL. La version saine
# n'atteint jamais ce point (aucune table créée pour une requête interdite).
mutant_mode pmmtrust_map_check_removed kernel/arch/i386/paging.c \
    'if (counted && frame_alias_forbidden(pa)) {' \
    'if (false && frame_alias_forbidden(pa)) {' \
    test=pmmtrust 37 \
    'KERNEL PANIC: page-table creation: sealing refused'
MUT_CMDLINE=test=pmmtrust mutant pmmtrust_gate_check_removed kernel/arch/i386/paging.c \
    '} else if (frame_alias_forbidden(frame)) {' \
    '} else if (false && frame_alias_forbidden(frame)) {' \
    35 \
    'gate_refused'
# Depuis K0.7-P4f-B3 : l'alias est accepté dans la région à table existante
# (assertion nommée), puis la sonde sans table atteint une création de table
# fatale (diagnostic). Les deux sont exigés.
mutant_mode pmmtrust_pde_targets_ignored kernel/arch/i386/paging.c \
    'if ((pde & PTE_PRESENT) && (pde & PTE_FRAME) == frame) {' \
    'if (false && (pde & PTE_PRESENT) && (pde & PTE_FRAME) == frame) {' \
    test=pmmtrust 37 \
    '[FAIL] refused_existing_region at' \
    'KERNEL PANIC: page-table creation: sealing refused'
# Répertoire falsifié : le contrôle de TYPE du passage sur le répertoire
# (fondé sur le PMM) refuse alors toute écriture, si bien que la région à
# table existante reste refusée ; la requête sans table atteint en revanche
# la création d'une table, dont le scellement refusé est fatal.
mutant_mode pmmtrust_directory_ignored kernel/arch/i386/paging.c \
    'if (frame == (uint32_t)(uintptr_t)g_pd) {' \
    'if (false) {' \
    test=pmmtrust 37 \
    'KERNEL PANIC: page-table creation: sealing refused'
MUT_CMDLINE=test=pmmtrust mutant pmmtrust_image_ignored kernel/arch/i386/paging.c \
    'return frame_in_kernel_image(frame) || frame_is_paging_structure(frame);' \
    'return (false && frame_in_kernel_image(frame)) || frame_is_paging_structure(frame);' \
    35 \
    'text_refused' \
    'idt_refused' \
    'gdt_refused'
# Depuis K0.7-P4f-B3, V2 est un mode destructif (test=tablealias). Sans le
# contrôle de table_create, le cadre volé est SCELLÉ, puis le passage refuse
# sa publication (alias préexistant) : l'arrêt survient plus tard et par un
# autre diagnostic que celui de la version saine.
# K1.0-N3 : la première couche (PMM, N3-I2) arrête désormais tablealias avant
# table_create ; la seconde couche est éprouvée par test=tablealias_l2, qui
# neutralise la première (image de test seulement).
mutant_mode pmmtrust_prior_alias_ignored kernel/arch/i386/paging.c \
    'if (paging_enabled() && frame_has_foreign_alias(pt)) {' \
    'if (false && frame_has_foreign_alias(pt)) {' \
    test=tablealias_l2 37 \
    'KERNEL PANIC: page-table creation: PDE publication refused'
MUT_CMDLINE=test=pmmtrust mutant pmmtrust_audit_blind kernel/arch/i386/paging.c \
    'if (frame != va && frame_alias_forbidden(frame)) {' \
    'if (false && frame_alias_forbidden(frame)) {' \
    35 \
    'audit_sees_foreign_alias'

# M41-M46 — K0.7-P4f-B2 : intégrité du répertoire (D3). Mode ISOLÉ
# test=pdeint ; toute PDE acceptée à tort est annulée par le prototype P4c.
MUT_CMDLINE=test=pdeint mutant pde_substitution_allowed kernel/arch/i386/paging.c \
    'if (g_pd[PD_INDEX(va)] & PTE_PRESENT) {' \
    'if (false) {' \
    35 \
    'substitution_refused' \
    'identical_write_refused'
# (Une PDE vers le répertoire reste refusée par le contrôle « t == pd »
# antérieur : directory_target_refused n'est pas une détection de M42.)
MUT_CMDLINE=test=pdeint mutant pde_duplicate_allowed kernel/arch/i386/paging.c \
    'if (frame_is_paging_structure(t)) {' \
    'if (false && frame_is_paging_structure(t)) {' \
    35 \
    'duplicate_refused'
MUT_CMDLINE=test=pdeint mutant pde_prior_alias_ignored kernel/arch/i386/paging.c \
    'if (paging_enabled() && (frame_has_foreign_alias(t) || !table_identity_sealed(t))) {' \
    'if (paging_enabled() && ((false && frame_has_foreign_alias(t)) || !table_identity_sealed(t))) {' \
    35 \
    'aliased_table_refused'
MUT_CMDLINE=test=pdeint mutant pde_unsealed_allowed kernel/arch/i386/paging.c \
    'if (paging_enabled() && (frame_has_foreign_alias(t) || !table_identity_sealed(t))) {' \
    'if (paging_enabled() && (frame_has_foreign_alias(t) || (false && !table_identity_sealed(t)))) {' \
    35 \
    'unsealed_table_refused' \
    'falsified_target_refused'
# Validation du passage contournée pour les PDE : pt_gate_write n'appelle
# plus pt_gate_check pour une PDE (l'entrée est écrite sans aucun contrôle).
MUT_CMDLINE=test=pdeint mutant pde_gate_validation_bypassed kernel/arch/i386/paging.c \
    'samos_err_t e = pt_gate_check(kind, va, value, &entry);' \
    'samos_err_t e = (kind == PT_KIND_PDE) ? (entry = &g_pd[PD_INDEX(va)], SAMOS_OK) : pt_gate_check(kind, va, value, &entry);' \
    35 \
    'substitution_refused' \
    'duplicate_refused' \
    'pde_clear_refused'
MUT_CMDLINE=test=pdeint mutant pde_audit_blind kernel/arch/i386/paging.c \
    'dup = (g_pd[k] & PTE_PRESENT) && (g_pd[k] & PTE_FRAME) == t;' \
    'dup = false;' \
    35 \
    'audit_sees_duplicate_pde'

# M47-M52 — K0.7-P4f-B3 : intégrité transactionnelle de la création des
# tables (D4).
# Effet de bord avant un refus : sans le pré-contrôle des références, une
# table neuve est publiée avant que pmm_ref ne rende ERANGE.
MUT_CMDLINE=test=tablelife mutant tl_erange_precheck_removed kernel/arch/i386/paging.c \
    'if (counted && t == FRAME_ALLOCATED && pmm_refs(pa) >= FRAME_REFS_MAX) {' \
    'if (false && t == FRAME_ALLOCATED && pmm_refs(pa) >= FRAME_REFS_MAX) {' \
    35 \
    'erange_no_table_created'
# Violation interne transformée en retour ordinaire : le mode destructif
# constate que vmm_map est revenu (« NOT fatal ») et échoue (statut 35).
mutant_mode tc_pde_fatal_to_return kernel/arch/i386/paging.c \
    'table_create_fatal("PDE publication refused", e, va, pt);' \
    'return e;' \
    test=tcpde 35 \
    '[TEST] internal violation NOT fatal'
mutant_mode tc_seal_fatal_to_return kernel/arch/i386/paging.c \
    'table_create_fatal("sealing refused", se, va, pt);' \
    'return se;' \
    test=tcseal 35 \
    '[TEST] internal violation NOT fatal'
mutant_mode tc_alias_fatal_to_return kernel/arch/i386/paging.c \
    'table_create_fatal("frame already mapped elsewhere", SAMOS_EPERM, va, pt);' \
    'return SAMOS_EPERM;' \
    test=tablealias_l2 35 \
    '[TEST] internal violation NOT fatal'
# Publication d'une table non scellée : le scellement ne retire plus W. Le
# passage refuse d'écrire W sur l'identité d'un cadre de pagination (règle à
# sens unique de P4e-2) : le scellement en bloc de paging_init échoue et le
# noyau s'arrête dès le démarrage (refus fermé), avec ce diagnostic.
mutant_mode seal_keeps_write kernel/arch/i386/paging.c \
    'samos_err_t e = pt_gate_write(PT_KIND_PTE, table, pte & ~PTE_WRITE);' \
    'samos_err_t e = pt_gate_write(PT_KIND_PTE, table, pte);' \
    test=tablelife 37 \
    'KERNEL PANIC: memory management initialisation failed (-10)'
# Contrôle d'erreur supprimé : l'épuisement du PMM n'est plus reconnu ; le
# cadre 0 est traité comme une table. Depuis K0.7-P4f-D, la seconde couche de
# table_create le refuse (mémoire basse) avant le scellement.
mutant_mode tl_enomem_check_removed kernel/arch/i386/paging.c \
    '        return SAMOS_ENOMEM;                    /* seule erreur ordinaire : aucun effet */' \
    '        (void)0;' \
    test=tablelife 37 \
    'KERNEL PANIC: page-table creation: frame belongs to the kernel image or low memory (error -10)'

# M53-M58 — K0.7-P4f-B4 : invalidations du TLB (D2). Sous QEMU, la fenêtre
# CR0.WP du passage vide le TLB : les autotests normaux ne voient aucun INVLPG
# oublié. Détections : harnais d'intégration sans vidage implicite
# (tlb_integration.sh), contrôle du code machine (check_elf.sh), et
# expérience isolée (test=tlbexp).
mutant_tool unmap_invlpg_removed kernel/arch/i386/paging.c \
    'cpu_invlpg((const void *)(uintptr_t)va);    /* vmm_unmap : avant pmm_unref */' \
    '(void)0;' \
    'tests/tools/tlb_integration.sh' \
    '[FAIL] unmap_translation_removed at' \
    '[FAIL] tlb_integration: phase A en echec'
mutant_tool unmap_invlpg_removed_machine_code kernel/arch/i386/paging.c \
    'cpu_invlpg((const void *)(uintptr_t)va);    /* vmm_unmap : avant pmm_unref */' \
    '(void)0;' \
    'tools/check_elf.sh build/samos.elf --production' \
    'vmm_unmap:sans-invlpg'
# INVLPG présent mais sur une AUTRE adresse : une instruction sans rapport ne
# doit pas satisfaire le contrôle.
mutant_tool unmap_invlpg_wrong_address kernel/arch/i386/paging.c \
    'cpu_invlpg((const void *)(uintptr_t)va);    /* vmm_unmap : avant pmm_unref */' \
    'cpu_invlpg((const void *)(uintptr_t)(va + 0x1000u));' \
    'tools/check_elf.sh build/samos.elf --production' \
    'vmm_unmap:operande'
# map_page : invalidation OPTIONNELLE (P 0 -> 1, SDM §4.10.4.3) ; aucun test
# de comportement ne peut la détecter, même sans vidage implicite (constaté) :
# seul le code machine la vérifie.
mutant_tool map_invlpg_removed kernel/arch/i386/paging.c \
    'cpu_invlpg((const void *)(uintptr_t)va);    /* map_page : défensive (P 0 -> 1) */' \
    '(void)0;' \
    'tools/check_elf.sh build/samos.elf --production' \
    'map_page:sans-invlpg'
mutant_tool seal_identity_invlpg_removed kernel/arch/i386/paging.c \
    'cpu_invlpg((const void *)(uintptr_t)va);    /* seal_identity : W retiré, invalidation requise */' \
    '(void)0;' \
    'tools/check_elf.sh build/samos.elf --production' \
    'paging_seal_identity:sans-invlpg'
# L'expérience isolée elle-même détecte l'absence d'invalidation.
MUT_CMDLINE=test=tlbexp mutant tlbexp_invlpg_removed kernel/tests/test_tlb.c \
    'cpu_invlpg((const void *)(uintptr_t)va);                       /* étape 1 : invalidation */' \
    '(void)0;' \
    35 \
    'new_frame_after_invlpg'

# M59-M66 — K0.7-P4f-D : intégrité de l'image du noyau face à un PMM
# falsifié (N1, N2, N5).
# Contrôle de l'allocateur supprimé : en création de table, la page de la GDT
# est remise à zéro, puis la seconde couche (table_create) arrête le noyau par
# SON diagnostic, absent de la version saine (qui s'arrête dans le PMM).
mutant_mode n1_alloc_check_removed kernel/mm/pmm.c \
    '    if (why != 0) {' \
    '    if (false && why != 0) {' \
    test=n1gdt 37 \
    'KERNEL PANIC: page-table creation: frame belongs to the kernel image or low memory'
# Même suppression, allocation de DONNÉES : aucune seconde couche, la page de
# test de l'image (motif connu) est distribuée (« NOT trapped »).
mutant_mode n1_alloc_check_removed_data kernel/mm/pmm.c \
    '    if (why != 0) {' \
    '    if (false && why != 0) {' \
    test=n1alloc 35 \
    '[TEST] protected frame handed out NOT trapped'
# Remise à zéro prématurée (avant le contrôle) : le refus survient encore,
# mais l'empreinte affichée est celle d'une page nulle. (Mode n1alloc : sur
# une vraie page de .data, le noyau s'arrêterait en silence, ce qui ne serait
# pas une détection valide.)
mutant_mode n1_premature_zeroing kernel/mm/pmm.c \
    '    uint32_t phys = f << PAGE_SHIFT;' \
    '    uint32_t phys = f << PAGE_SHIFT; memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);' \
    test=n1alloc 37 \
    'refused before zeroing, page checksum 0x76efddc5'
# Contrôle de publication supprimé dans le passage.
MUT_CMDLINE=test=imageint mutant n2_gate_check_removed kernel/arch/i386/paging.c \
    '        if (frame_never_table(t)) {' \
    '        if (false && frame_never_table(t)) {' \
    35 \
    'text_pde_refused' \
    'idt_pde_refused'
# Passage contourné pour les PDE (aucune validation) : aussi détecté par N2.
MUT_CMDLINE=test=imageint mutant n2_gate_bypassed kernel/arch/i386/paging.c \
    'samos_err_t e = pt_gate_check(kind, va, value, &entry);' \
    'samos_err_t e = (kind == PT_KIND_PDE) ? (entry = &g_pd[PD_INDEX(va)], SAMOS_OK) : pt_gate_check(kind, va, value, &entry);' \
    35 \
    'text_pde_refused' \
    'lowmem_pde_refused'
# Nouveaux contrôles d'audit supprimés (N5).
MUT_CMDLINE=test=imageint mutant n5_audit_forbidden_blind kernel/arch/i386/paging.c \
    'if ((g_pd[i] & PTE_PRESENT) && frame_never_table(g_pd[i] & PTE_FRAME)) {' \
    'if (false && frame_never_table(g_pd[i] & PTE_FRAME)) {' \
    35 \
    'audit_sees_forbidden_table'
MUT_CMDLINE=test=imageint mutant n5_audit_image_type_blind kernel/arch/i386/paging.c \
    'if (pmm_core_type(p, f) != FRAME_KERNEL) {' \
    'if (false) {' \
    35 \
    'audit_sees_image_type_mismatch'
MUT_CMDLINE=test=imageint mutant n5_audit_lowmem_blind kernel/arch/i386/paging.c \
    'if (t == FRAME_FREE || t == FRAME_ALLOCATED || t == FRAME_PAGETABLE) {' \
    'if (false && t == FRAME_FREE) {' \
    35 \
    'audit_sees_lowmem_allocatable'

# M67-M79 — K1.0-N3 : cadres encore projetés face à des métadonnées PMM
# falsifiées (docs/review/K1.0-N3-implementation.md). Chaque mutation est
# détectée par une assertion nommée du mode isolé n3 ou par un diagnostic
# exact d'un mode destructif ; jamais par un délai ni par un triple fault.
# N3-I1 : contrôle avant libération supprimé. La libération falsifiée réussit
# (assertion nommée), puis la boucle de réutilisation rend le cadre projeté :
# la seconde barrière (N3-I2) arrête le noyau. D'où le statut 37.
mutant_mode n3_free_check_removed kernel/mm/pmm.c \
    '    if (vmm_frame_dynamically_mapped(phys)) {' \
    '    if (false && vmm_frame_dynamically_mapped(phys)) {' \
    test=n3 37 \
    '[FAIL] n3_i1_forged_refs_free_refused at'
# N3-I2 : contrôle avant remise à zéro supprimé. Données : le cadre projeté
# est distribué (« NOT trapped »). Table : la seconde couche D1 l'arrête, par
# SON diagnostic, absent de la version saine.
mutant_mode n3_alloc_check_removed_data kernel/mm/pmm.c \
    'if (N3_ALLOC_CHECK_ENABLED() && vmm_frame_dynamically_mapped(phys)) {' \
    'if (false && vmm_frame_dynamically_mapped(phys)) {' \
    test=n3type 35 \
    '[TEST] mapped frame handed out NOT trapped'
mutant_mode n3_alloc_check_removed_table kernel/mm/pmm.c \
    'if (N3_ALLOC_CHECK_ENABLED() && vmm_frame_dynamically_mapped(phys)) {' \
    'if (false && vmm_frame_dynamically_mapped(phys)) {' \
    test=n3table 37 \
    'KERNEL PANIC: page-table creation: frame already mapped elsewhere'
# Remise à zéro prématurée (avant le contrôle) : le refus survient encore,
# mais l'empreinte du diagnostic est celle d'une page nulle.
mutant_mode n3_premature_zeroing kernel/mm/pmm.c \
    '    if (N3_ALLOC_CHECK_ENABLED() && vmm_frame_dynamically_mapped(phys)) {' \
    '    memset((void *)(uintptr_t)phys, 0, PAGE_SIZE); if (N3_ALLOC_CHECK_ENABLED() && vmm_frame_dynamically_mapped(phys)) {' \
    test=n3type 37 \
    'refused before zeroing, page checksum 0x76efddc5'
# N3-I3 : parcours des PTE contourné (aucune occurrence comptée).
MUT_CMDLINE=test=n3 mutant n3_scan_bypassed kernel/arch/i386/paging.c \
    '                n++;' \
    '                (void)0;' \
    35 \
    'n3_i3_boundary_mapping_counted' \
    'n3_i3_independent_of_pmm'
# N3-I3 : la table de la frontière (identité et projections dynamiques dans
# la même PDE) est sautée.
MUT_CMDLINE=test=n3 mutant n3_scan_skips_boundary_table kernel/arch/i386/paging.c \
    'for (uint32_t i = PD_INDEX(g_idmap_top); i < 1024u; i++) {    /* N3 : depuis la PDE de la frontière */' \
    'for (uint32_t i = PD_INDEX(g_idmap_top) + 1u; i < 1024u; i++) {' \
    35 \
    'n3_i3_boundary_mapping_counted'
# N3-I3 : identité de la table de la frontière comptée comme projection
# dynamique (test PTE par PTE supprimé). L'assertion nommée échoue ; la suite
# du mode rencontre ensuite un cadre d'identité « projeté » et la seconde
# barrière arrête le noyau (statut 37).
# (Un parcours commençant à la PDE 0 serait une mutation ÉQUIVALENTE : le test
# PTE par PTE exclut déjà l'identité ; elle n'est donc pas retenue.)
mutant_mode n3_identity_counted_in_boundary kernel/arch/i386/paging.c \
    'if (va >= g_idmap_top && (pte & PTE_PRESENT) && (pte & PTE_FRAME) == frame) {' \
    'if ((va | 1u) != 0 && (pte & PTE_PRESENT) && (pte & PTE_FRAME) == frame) {' \
    test=n3 37 \
    '[FAIL] n3_i3_boundary_identity_not_counted at'
# N3-I3 : frontière décalée d'une page (> au lieu de >=) : la première page
# dynamique (va == idmap_top, même PDE que l'identité) n'est plus vue.
MUT_CMDLINE=test=n3 mutant n3_identity_boundary_off_by_one kernel/arch/i386/paging.c \
    'if (va >= g_idmap_top && (pte & PTE_PRESENT) && (pte & PTE_FRAME) == frame) {' \
    'if (va > g_idmap_top && (pte & PTE_PRESENT) && (pte & PTE_FRAME) == frame) {' \
    35 \
    'n3_i3_boundary_mapping_counted'
# N3-I4 : le retrait rend une référence malgré un déficit : la référence de
# la projection restante est consommée.
MUT_CMDLINE=test=n3 mutant n3_unmap_consumes_other_ref kernel/arch/i386/paging.c \
    'pmm_refs(frame) >= real && real != 0;' \
    'pmm_refs(frame) + 0u * real != 0;' \
    35 \
    'n3_i4_other_ref_kept'
# N3-I4 : retour à l'ancien comportement (refus si incohérent) : la
# projection incohérente ne peut plus être retirée.
MUT_CMDLINE=test=n3 mutant n3_unmap_refuses_inconsistent kernel/arch/i386/paging.c \
    '    samos_err_t ge = pt_gate_write(PT_KIND_PTE, va, 0);' \
    '    if (!release_ref) { return SAMOS_EINVAL; } samos_err_t ge = pt_gate_write(PT_KIND_PTE, va, 0);' \
    35 \
    'n3_i4_inconsistent_mapping_removed'
# Anomalie de retrait non comptée.
MUT_CMDLINE=test=n3 mutant n3_unmap_anomaly_uncounted kernel/arch/i386/paging.c \
    '        g_unmap_ref_anomalies++;               /* N3-I4 : projection retirée, g_meta intacte */' \
    '        (void)0;' \
    35 \
    'n3_i4_no_meta_write'
# N3-I5 : compteurs d'audit supprimés.
MUT_CMDLINE=test=n3 mutant n3_audit_ref_mismatch_blind kernel/arch/i386/paging.c \
    '                a->ref_mismatch++;' \
    '                (void)0;' \
    35 \
    'n3_i5_ref_deficit_seen'
MUT_CMDLINE=test=n3 mutant n3_audit_type_mismatch_blind kernel/arch/i386/paging.c \
    '                a->mapped_type_mismatch++;' \
    '                (void)0;' \
    35 \
    'n3_i5_type_free_seen' \
    'n3_i5_r4_type_seen'

# M80-M85 — K1.0-N3-TEST-CLOSE : lacunes relevées par la contre-vérification
# (docs/review/K1.0-N3-final-verification.md, sondes X1 à X4, X9, X10).
# X1 : U2 trop strict (> au lieu de >=) : le retrait COHÉRENT ne rend plus sa
# référence. Défaut distinct de n3_unmap_consumes_other_ref (sens opposé).
MUT_CMDLINE=test=n3 mutant n3_unmap_strict_gt kernel/arch/i386/paging.c \
    'pmm_refs(frame) >= real && real != 0;' \
    'pmm_refs(frame) > real && real != 0;' \
    35 \
    'n3_i4_last_unmap_releases'
# X2 : D relevé APRÈS l'effacement (la projection retirée n'est plus comptée) :
# en déficit, la référence de la projection restante est consommée.
MUT_CMDLINE=test=n3 mutant n3_unmap_count_after_removal kernel/arch/i386/paging.c \
    'uint32_t real = dyn_mapping_count(frame, 0u);' \
    'uint32_t real = dyn_mapping_count(frame, 0u) - 1u;' \
    35 \
    'n3_i4_other_ref_kept'
# X3 : pmm_unref AVANT l'INVLPG (une seule instruction invlpg, appel au PMM
# placé avant elle). Invisible aux autotests sous QEMU (vidage du TLB par la
# fenêtre CR0.WP) : seul le contrôle du code machine le détecte, par la règle
# d'ordre de vmm_unmap.
mutant_tool n3_unmap_unref_before_invlpg kernel/arch/i386/paging.c \
    '    cpu_invlpg((const void *)(uintptr_t)va);    /* vmm_unmap : avant pmm_unref */' \
    '    if (release_ref) { samos_err_t e0 = pmm_unref(frame); KASSERT(e0 == SAMOS_OK); } release_ref = false; cpu_invlpg((const void *)(uintptr_t)va);' \
    'tools/check_elf.sh build/samos.elf --production' \
    'vmm_unmap:invlpg-apres-pmm_unref'
# X4 : déduplication de l'audit supprimée : ref_mismatch compte une anomalie
# par PTE (3 pour un cadre à 3 projections) au lieu d'une par cadre.
MUT_CMDLINE=test=n3 mutant n3_audit_counts_per_pte kernel/arch/i386/paging.c \
    'if (frame_alias_forbidden(frame) || dyn_first_slot(frame) != ((i << 10) | j)) {' \
    'if (frame_alias_forbidden(frame) || (dyn_first_slot(frame) & 0u) != 0) {' \
    35 \
    'n3_i5_ref_mismatch_per_frame'
# X9 : l'oracle ignore la dernière PTE (index 1023) de chaque table.
MUT_CMDLINE=test=n3 mutant n3_scan_skips_last_pte kernel/arch/i386/paging.c \
    "for (uint32_t j = 0; j < 1024u; j++) {    /* N3 : jusqu'à la PTE 1023 incluse */" \
    'for (uint32_t j = 0; j < 1023u; j++) {' \
    35 \
    'n3_x9_last_pte_counted' \
    'n3_x9_last_pte_free_refused' \
    'n3_x9_last_pte_not_reallocated'
# X10 : l'oracle ignore la dernière PDE (1023).
MUT_CMDLINE=test=n3 mutant n3_scan_skips_last_pde kernel/arch/i386/paging.c \
    'for (uint32_t i = PD_INDEX(g_idmap_top); i < 1024u; i++) {    /* N3 : depuis la PDE de la frontière */' \
    'for (uint32_t i = PD_INDEX(g_idmap_top); i < 1023u; i++) {' \
    35 \
    'n3_x10_last_pde_counted' \
    'n3_x10_last_pde_free_refused' \
    'n3_x10_last_pde_not_zeroed'

# M86-M88 — K0.7-SCOPE-RESOLVE : cohérence de la carte mémoire avec Multiboot
# (test_memmap, N2 de l'audit M2). Défauts de memmap.c (production).
# K0.7-E820-FIX : cibles recalées sur les vérifications PAR PLAGE
# (memcover.c) ; la première n'est plus vue que par la vérification du banc
# (128 MiB), car les plages ne dépendent plus du total.
mutant memmap_total_first_region_only kernel/mm/memmap.c \
    '            t += m->r[i].length;' \
    '            t += m->r[i].length; return t;' \
    35 \
    'memmap_bench_total_over_100mib'
mutant memmap_parse_halves_length kernel/mm/memmap.c \
    '        add_region(m, base, length, type);' \
    '        add_region(m, base, length / 2u, type);' \
    35 \
    'memmap_upper_range_usable'
mutant memmap_parse_loses_one_page kernel/mm/memmap.c \
    '        add_region(m, base, length, type);' \
    '        add_region(m, base, length - (type == MULTIBOOT_MEMORY_AVAILABLE ? 0x1000u : 0u), type);' \
    35 \
    'memmap_lower_range_usable' \
    'memmap_upper_range_usable'

# M89-M91 — K0.7-E820-FIX (F1, F2).
# F2 : région basse supprimée (le total diminue : l'ancien test la voyait).
mutant memmap_drops_low_region kernel/mm/memmap.c \
    '        add_region(m, base, length, type);' \
    '        if (base != 0) { add_region(m, base, length, type); }' \
    35 \
    'memmap_lower_range_usable'
# F2 : région basse remplacée par une COPIE recouvrant la région haute : total
# inchangé, aucune couverture supplémentaire. L'ancien test (58feb25, totaux)
# la déclarait réussie ; non équivalente à la précédente (cartes différentes).
mutant memmap_low_region_duplicated_high kernel/mm/memmap.c \
    '        add_region(m, base, length, type);' \
    '        add_region(m, base == 0 ? 0x100000u : base, length, type);' \
    35 \
    'memmap_lower_range_usable'
# F1 : fin de région calculée par base + length (débordement pour une région
# finissant à 2^64). Invisible sous QEMU (aucune telle région) : détectée par
# le test hébergé, par son assertion nommée.
mutant_tool memcover_naive_region_end kernel/tests/memcover.c \
    'if (m->r[i].base <= pos && pos <= rl && (!found || rl > reach)) {' \
    'if (m->r[i].base <= pos && pos < m->r[i].base + m->r[i].length && (!found || rl > reach)) {' \
    'tests/host/run.sh' \
    'f1_region_ending_at_2_64_covers'
