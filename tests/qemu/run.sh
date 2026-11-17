#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/qemu/run.sh
# -----------------------------------------------------------------------------
# Exécute les cas de test « boot complet » sous QEMU et vérifie :
#   - le statut de sortie de QEMU (transmis par isa-debug-exit),
#   - la présence de motifs attendus dans la sortie debugcon,
#   - l'absence de réinitialisation CPU inattendue (triple fault).
#
# Statuts QEMU (voir kernel/include/samos/qemu_dbg.h) :
#   33  = noyau : SUCCESS        35 = noyau : FAILURE      37 = noyau : PANIC
#   124 = délai dépassé (timeout ; attendu pour un boot normal qui s'arrête par HLT)
#   0   = QEMU s'est arrêté seul : avec -no-reboot, signe d'un reset/triple fault
# =============================================================================
set -u
KERNEL="${1:?usage: run.sh build/samos.elf}"
OUT="$(dirname "$KERNEL")/tests"
mkdir -p "$OUT"
QEMU="${QEMU:-qemu-system-i386}"
TIMEOUT="${TIMEOUT:-10}"
fail=0
total=0

# run_case NOM "CMDLINE" STATUT_ATTENDU MOTIF...
run_case() {
    local name="$1" append="$2" expect="$3"; shift 3
    local dlog="$OUT/$name.debugcon.log" slog="$OUT/$name.serial.log" qlog="$OUT/$name.qemu.log"
    total=$((total + 1))
    rm -f "$dlog" "$slog" "$qlog"
    timeout "$TIMEOUT" "$QEMU" -m 128M -display none -no-reboot \
        -kernel "$KERNEL" -append "$append" \
        -debugcon "file:$dlog" -serial "file:$slog" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -d cpu_reset,guest_errors -D "$qlog" </dev/null >/dev/null 2>&1
    local status=$?
    local ok=1
    if [ "$status" -ne "$expect" ]; then
        echo "  statut QEMU $status, attendu $expect"
        ok=0
    fi
    # -d cpu_reset : QEMU 8.2 journalise DEUX resets à la mise sous tension
    # (initialisation de l'objet CPU, puis reset système avec EIP=0xFFF0).
    # Tout reset SUPPLÉMENTAIRE indique un triple fault (ou un reset demandé).
    # Valeur mesurée, ajustable via QEMU_BOOT_RESETS.
    local resets
    resets=$(grep -c 'CPU Reset' "$qlog" 2>/dev/null || true)
    if [ "${resets:-0}" -gt "${QEMU_BOOT_RESETS:-2}" ]; then
        echo "  reset CPU inattendu ($resets resets dans $qlog)"
        ok=0
    fi
    # Motif "serial:TEXTE" : recherché uniquement dans la sortie série
    # (prouve que le pilote COM1 fonctionne de bout en bout). Motif "!TEXTE" :
    # ne doit apparaître nulle part. Sinon : debugcon ou série.
    for pat in "$@"; do
        local files=("$dlog" "$slog") want=1
        case "$pat" in
            serial:*) pat="${pat#serial:}"; files=("$slog") ;;
            '!'*)     pat="${pat#!}"; want=0 ;;
        esac
        if grep -qF -- "$pat" "${files[@]}" 2>/dev/null; then found=1; else found=0; fi
        if [ "$found" -ne "$want" ]; then
            if [ "$want" -eq 1 ]; then echo "  motif absent : $pat"; else echo "  motif interdit present : $pat"; fi
            ok=0
        fi
    done
    if [ "$ok" -eq 1 ]; then
        echo "[PASS] $name"
    else
        echo "[FAIL] $name (journaux : $dlog $slog $qlog)"
        fail=1
    fi
}

# shellcheck source=tests/qemu/cases.sh
source "$(dirname "$0")/cases.sh"

echo "QEMU: $total cas, $([ $fail -eq 0 ] && echo 'tous reussis' || echo 'ECHECS')"
exit $fail
