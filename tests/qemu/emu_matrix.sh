#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/qemu/emu_matrix.sh                  (K1.1-M64-0)
# -----------------------------------------------------------------------------
# Exécute les mêmes bancs (tests/qemu/run.sh, 86 cas, et
# tests/qemu/gdb_checks.sh) sur la MÊME image i386 sous plusieurs émulateurs,
# puis compare les résultats (tests/qemu/emu_compare.sh). Aucun script n'est
# dupliqué : seul l'émulateur (variable QEMU) et le répertoire des journaux
# (TEST_OUT) changent.
#
# Usage : emu_matrix.sh build/samos.elf build/samos-test.elf EMU_REF EMU...
#   EMU_REF : émulateur de référence (qemu-system-i386 pour make test-x86_64-emu)
# Journaux : build/emu/<nom de l'émulateur>/
# Sortie : 0 si chaque émulateur réussit TOUS les cas ET si aucun ne diverge
# de la référence ; 1 sinon ; 2 si un prérequis manque.
# =============================================================================
set -u
PROD="${1:?usage: emu_matrix.sh PROD TEST EMU_REF EMU...}"
TEST="${2:?}"
shift 2
[ "$#" -ge 2 ] || { echo "[FAIL] emu_matrix : au moins deux emulateurs requis"; exit 2; }
HERE="$(dirname "$0")"
BASE="$(dirname "$TEST")/emu"
REF="$1"
fail=0

for emu in "$@"; do
    dir="$BASE/$(basename "$emu")"
    rm -rf "$dir"; mkdir -p "$dir"
    echo "=== $emu : banc QEMU ($dir/qemu.out)"
    QEMU="$emu" TEST_OUT="$dir" "$HERE/run.sh" "$PROD" "$TEST" > "$dir/qemu.out" 2>&1
    rc=$?
    grep -E '^\[EMU\]|^QEMU: ' "$dir/qemu.out"
    [ "$rc" -eq 0 ] || { echo "[FAIL] $emu : banc QEMU en echec (statut $rc)"; grep '^\[FAIL\]' "$dir/qemu.out" | head; fail=1; }
    echo "=== $emu : verifications GDB ($dir/gdb.out)"
    QEMU="$emu" TEST_OUT="$dir" "$HERE/gdb_checks.sh" "$PROD" > "$dir/gdb.out" 2>&1
    rc=$?
    grep -cE '^\[PASS\] gdb' "$dir/gdb.out" | sed 's/^/    verifications GDB reussies : /'
    [ "$rc" -eq 0 ] || { echo "[FAIL] $emu : verifications GDB en echec (statut $rc)"; grep '^\[FAIL\]' "$dir/gdb.out" | head; fail=1; }
done

for emu in "${@:2}"; do
    echo "=== comparaison $REF / $emu"
    "$HERE/emu_compare.sh" "$BASE/$(basename "$REF")" "$BASE/$(basename "$emu")" || fail=1
done
if [ "$fail" -eq 0 ]; then echo "[PASS] matrice d'emulateurs : $*"; else echo "[FAIL] matrice d'emulateurs : $*"; fi
exit $fail
