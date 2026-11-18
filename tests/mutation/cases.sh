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
mutant e3_type_check kernel/arch/i386/paging.c \
    'if (counted && t != FRAME_ALLOCATED) {' \
    'if (counted && t == FRAME_FREE) {' \
    35 \
    'e3_target_refused(kpage, "kmain")' \
    'e3_target_refused(paging_directory(), "CR3")' \
    'e3_target_refused(pt_kmain, "table active")'

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
mutant ptgate_wp_not_restored kernel/tests/test_ptgate.c \
    'cpu_write_cr0(cr0);                         /* WP rétabli AVANT les IRQ */' \
    '(void)cr0;' \
    35 \
    'state_intact(ifl)'

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
