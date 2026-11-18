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
    'pmm_free_frame(f) == SAMOS_EBUSY' \
    'pmm_free_frame(A) == SAMOS_EBUSY'

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
MUT_CMDLINE=test=pdeint mutant seal_new_table_removed kernel/arch/i386/paging.c \
    'samos_err_t se = seal_table_frame(pt);' \
    'samos_err_t se = SAMOS_OK;' \
    35 \
    'vmm_tables_created'
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
MUT_CMDLINE=test=pmmtrust mutant pmmtrust_map_check_removed kernel/arch/i386/paging.c \
    'if (counted && frame_alias_forbidden(pa)) {' \
    'if (false && frame_alias_forbidden(pa)) {' \
    35 \
    'no_table_created'
MUT_CMDLINE=test=pmmtrust mutant pmmtrust_gate_check_removed kernel/arch/i386/paging.c \
    '} else if (frame_alias_forbidden(frame)) {' \
    '} else if (false && frame_alias_forbidden(frame)) {' \
    35 \
    'gate_refused'
MUT_CMDLINE=test=pmmtrust mutant pmmtrust_pde_targets_ignored kernel/arch/i386/paging.c \
    'if ((pde & PTE_PRESENT) && (pde & PTE_FRAME) == frame) {' \
    'if (false && (pde & PTE_PRESENT) && (pde & PTE_FRAME) == frame) {' \
    35 \
    'first_table_refused' \
    'other_table_refused' \
    'new_table_refused'
MUT_CMDLINE=test=pmmtrust mutant pmmtrust_directory_ignored kernel/arch/i386/paging.c \
    'if (frame == (uint32_t)(uintptr_t)g_pd) {' \
    'if (false) {' \
    35 \
    'directory_refused'
MUT_CMDLINE=test=pmmtrust mutant pmmtrust_image_ignored kernel/arch/i386/paging.c \
    'return frame_in_kernel_image(frame) || frame_is_paging_structure(frame);' \
    'return (false && frame_in_kernel_image(frame)) || frame_is_paging_structure(frame);' \
    35 \
    'text_refused' \
    'idt_refused' \
    'gdt_refused'
MUT_CMDLINE=test=pmmtrust mutant pmmtrust_prior_alias_ignored kernel/arch/i386/paging.c \
    'if (paging_enabled() && frame_has_foreign_alias(pt)) {' \
    'if (false && frame_has_foreign_alias(pt)) {' \
    35 \
    'v2_refused_before_any_window'
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
