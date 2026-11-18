#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/tlb_integration.sh          (K0.7-P4f-B4)
# -----------------------------------------------------------------------------
# PREUVE D'INTÉGRATION de la cohérence du TLB par le VMM réel (défaut D2 de
# l'audit P4f-A), SANS le vidage implicite du TLB par QEMU.
#
# Sous QEMU (TCG), tout changement de CR0.WP vide le TLB entier. Chaque
# écriture de PTE du VMM passe par la fenêtre WP du passage : un INVLPG oublié
# y est invisible aux autotests. Ce harnais rejoue le mode isolé test=tlbvmm
# dans une COPIE JETABLE des sources (jamais l'arbre de travail), où :
#   1. paging_init active PG SANS WP (CR0.WP reste 0) ;
#   2. pt_gate_write n'écrit plus CR0 (ni retrait, ni rétablissement de WP)
#      et n'exige plus WP à l'entrée ;
#   3. l'invariant WP de ktest_check est neutralisé (WP est 0 par conception).
# Aucune écriture de CR0 n'a donc lieu après l'activation : seul INVLPG peut
# retirer une traduction du TLB. Les écritures dans les tables scellées
# réussissent parce que WP = 0 (le superviseur ignore R/W) : la copie ne
# teste PAS les protections, seulement la cohérence des traductions.
#
# Phase A : la copie telle quelle doit réussir test=tlbvmm (statut 33).
# Phase B (contrôle de sensibilité du harnais) : la même copie, l'INVLPG de
#   vmm_unmap retiré, doit ÉCHOUER par l'assertion nommée
#   unmap_translation_removed ; sinon le harnais est aveugle.
#
# Usage : tests/tools/tlb_integration.sh [RACINE]   (défaut : racine du dépôt)
# Sortie : 0 si A réussit et B échoue comme prévu ; 1 sinon (diagnostics
# « [FAIL] tlb_integration: ... » et lignes [FAIL] du noyau) ; 2 si un texte
# à remplacer est introuvable (copie non modifiable : rien n'est mesuré).
# =============================================================================
set -u
ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
QEMU="${QEMU:-qemu-system-i386}"
TIMEOUT="${TIMEOUT:-60}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

copy_tree() {   # copy_tree DEST : fichiers suivis par git (ou tout l'arbre hors build/)
    mkdir -p "$1"
    if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        (cd "$ROOT" && git ls-files -z | xargs -0 tar -cf - 2>/dev/null) | tar -xf - -C "$1"
    else
        (cd "$ROOT" && tar --exclude=./build -cf - .) | tar -xf - -C "$1"
    fi
}

# patch FICHIER 'ANCIEN' 'NOUVEAU' : remplacement littéral, exactement une fois.
patch_once() {
    python3 - "$1" "$2" "$3" <<'PY' || exit 2
import sys
p, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
n = s.count(a)
if n != 1:
    print(f"[FAIL] tlb_integration: texte trouvé {n} fois dans {p} (1 requis) : {a!r}")
    sys.exit(2)
open(p, "w").write(s.replace(a, b))
PY
}

no_cr0_flush() {   # no_cr0_flush DIR
    local pg="$1/kernel/arch/i386/paging.c"
    patch_once "$pg" 'cpu_write_cr0(cpu_read_cr0() | CR0_PG | CR0_WP);' 'cpu_write_cr0(cpu_read_cr0() | CR0_PG);'
    patch_once "$pg" '    if (!(cr0 & CR0_WP)) {' '    if (0) {'
    patch_once "$pg" 'cpu_write_cr0(cr0 & ~CR0_WP);' '(void)0;'
    patch_once "$pg" 'cpu_write_cr0(cr0);                         /* WP rétabli AVANT IF */' '(void)cr0;'
    patch_once "$1/kernel/tests/ktest.c" 'if ((cr0 & CR0_PG) && !(cr0 & CR0_WP)) {' 'if (0) {'
}

run_mode() {   # run_mode DIR -> RUN_STATUS, RUN_LOG
    RUN_LOG="$1/tlbvmm.log"; RUN_STATUS=-1
    if ! make -C "$1" -j"${JOBS:-4}" build/samos-test.elf >"$1/build.log" 2>&1; then
        echo "[FAIL] tlb_integration: compilation de la copie en échec ($1/build.log)"
        return
    fi
    timeout "$TIMEOUT" "$QEMU" -m 128M -display none -no-reboot -kernel "$1/build/samos-test.elf" \
        -append test=tlbvmm -serial "file:$RUN_LOG" -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        </dev/null >/dev/null 2>&1
    RUN_STATUS=$?
}

fail=0
# --- Phase A : code tel quel, sans vidage implicite ---------------------------
copy_tree "$WORK/a"
no_cr0_flush "$WORK/a"
run_mode "$WORK/a"
if [ "$RUN_STATUS" -eq 33 ] && tr -d '\r' < "$RUN_LOG" | grep -q '^\[TEST\] suites=1 checks=[0-9]* failures=0$'; then
    echo "[PASS] tlb_integration: phase A, VMM coherent sans vidage implicite du TLB ($(tr -d '\r' < "$RUN_LOG" | grep -o 'checks=[0-9]*'))"
else
    echo "[FAIL] tlb_integration: phase A en echec (statut $RUN_STATUS)"
    tr -d '\r' < "$RUN_LOG" 2>/dev/null | grep -E '^\[FAIL\]|KERNEL PANIC' | head -5
    exit 1
fi
# --- Phase B : contrôle de sensibilité ---------------------------------------
copy_tree "$WORK/b"
no_cr0_flush "$WORK/b"
patch_once "$WORK/b/kernel/arch/i386/paging.c" \
    'cpu_invlpg((const void *)(uintptr_t)va);    /* vmm_unmap : avant pmm_unref */' '(void)0;'
run_mode "$WORK/b"
if [ "$RUN_STATUS" -eq 35 ] && tr -d '\r' < "$RUN_LOG" | grep -q '^\[FAIL\] unmap_translation_removed at '; then
    echo "[PASS] tlb_integration: phase B, INVLPG de vmm_unmap retire -> unmap_translation_removed signale"
else
    echo "[FAIL] tlb_integration: harnais aveugle, phase B non detectee (statut $RUN_STATUS)"
    fail=1
fi
exit $fail
