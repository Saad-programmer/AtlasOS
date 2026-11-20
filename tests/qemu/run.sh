#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/qemu/run.sh
# -----------------------------------------------------------------------------
# Exécute les cas de test « démarrage complet » sous QEMU (tests/qemu/cases.sh)
# et rend un verdict par cas à partir de TROIS sources indépendantes :
#   1. le statut de sortie de QEMU (transmis par isa-debug-exit, ou timeout) ;
#   2. des motifs attendus / interdits dans les journaux série et debugcon ;
#   3. le journal interne de QEMU (-d cpu_reset) : triple fault, resets.
#
# Statuts QEMU (voir kernel/include/samos/qemu_dbg.h) :
#   33  = noyau : SUCCESS        35 = noyau : FAILURE      37 = noyau : PANIC
#   124 = délai dépassé (attendu pour un démarrage normal qui reste inactif)
#   0   = QEMU s'est arrêté seul : avec -no-reboot, signe d'un triple fault
#   127 = QEMU introuvable (toujours un échec)
#
# Variables globales (environnement) :
#   QEMU=qemu-system-i386  émulateur (K1.1-M64-0 : qemu-system-x86_64 accepté,
#                          même image i386 ; identifié par tests/qemu/emu.sh)
#   TEST_OUT=DIR           répertoire des journaux (défaut : build/tests)
#
# Variables par cas (préfixe d'appel) :
#   IMG=prod|test   image à démarrer (défaut : test)
#   QEMU_MEM=128M   taille de la RAM
#   TIMEOUT=10      délai maximal en secondes
#   EXPECT_TRIPLE=1 le cas DOIT provoquer un triple fault
#
# Motifs :
#   "texte"         présent dans debugcon OU série
#   "serial:texte"  présent dans la sortie série
#   "re:regex"      expression régulière étendue (grep -E), debugcon ou série
#   "!texte"        absent de debugcon ET de série
# =============================================================================
set -u
PROD_KERNEL="${1:?usage: run.sh build/samos.elf build/samos-test.elf}"
TEST_KERNEL="${2:?usage: run.sh build/samos.elf build/samos-test.elf}"
# TEST_OUT : répertoire des journaux (K1.1-M64-0 : un répertoire par émulateur
# quand tests/qemu/emu_matrix.sh compare deux émulateurs).
OUT="${TEST_OUT:-$(dirname "$TEST_KERNEL")/tests}"
mkdir -p "$OUT"
QEMU="${QEMU:-qemu-system-i386}"
TIMEOUT="${TIMEOUT:-10}"
fail=0
total=0
passed=0

# Garde-fou contre un faux positif global : sans QEMU, tous les cas « timeout »
# ne doivent pas pouvoir réussir. K1.1-M64-0 : l'émulateur est identifié par
# QMP (tests/qemu/emu.sh) ; absent, invalide ou d'une autre architecture, il
# est refusé (statut 2). Son identité figure en tête du rapport.
# shellcheck source=tests/qemu/emu.sh
source "$(dirname "$0")/emu.sh"
emu_probe "$QEMU" || exit 2
emu_print "$QEMU"
for k in "$PROD_KERNEL" "$TEST_KERNEL"; do
    [ -f "$k" ] || { echo "[FAIL] image absente : $k"; exit 2; }
done

image_path() {
    case "${IMG:-test}" in
        prod) echo "$PROD_KERNEL" ;;
        test) echo "$TEST_KERNEL" ;;
        *) echo "/nonexistent-image-${IMG}" ;;
    esac
}

# verdict NOM STATUT ATTENDU DLOG SLOG QLOG MOTIF...
verdict() {
    local name="$1" status="$2" expect="$3" dlog="$4" slog="$5" qlog="$6"; shift 6
    local ok=1
    if [ "$status" -ne "$expect" ]; then
        echo "  statut QEMU $status, attendu $expect"
        ok=0
    fi
    # Triple fault : QEMU écrit « Triple fault » ; AVEC -no-reboot il s'arrête
    # au lieu de réinitialiser le CPU, donc aucun « CPU Reset » supplémentaire
    # (constaté en K0.4). QEMU 8.2 journalise 2 resets à la mise sous tension.
    local triple resets
    triple=$(grep -c 'Triple fault' "$qlog" 2>/dev/null || true)
    resets=$(grep -c 'CPU Reset' "$qlog" 2>/dev/null || true)
    if [ ! -s "$qlog" ]; then
        echo "  journal QEMU vide ou absent ($qlog) : verdict impossible"
        ok=0
    elif [ "${EXPECT_TRIPLE:-0}" -eq 1 ]; then
        [ "${triple:-0}" -gt 0 ] || { echo "  triple fault attendu mais absent"; ok=0; }
    elif [ "${triple:-0}" -gt 0 ] || [ "${resets:-0}" -gt "${QEMU_BOOT_RESETS:-2}" ]; then
        echo "  triple fault ou reset inattendu (triple=$triple, resets=$resets)"
        ok=0
    fi
    for pat in "$@"; do
        local files=("$dlog" "$slog") want=1 mode=F
        case "$pat" in
            serial:*) pat="${pat#serial:}"; files=("$slog") ;;
            re:*)     pat="${pat#re:}"; mode=E ;;
            '!'*)     pat="${pat#!}"; want=0 ;;
        esac
        local found=0
        # tr -d '\r' : le pilote série émet CR LF
        if cat "${files[@]}" 2>/dev/null | tr -d '\r' | grep -q"$mode" -- "$pat"; then found=1; fi
        if [ "$found" -ne "$want" ]; then
            if [ "$want" -eq 1 ]; then echo "  motif absent : $pat"; else echo "  motif interdit present : $pat"; fi
            ok=0
        fi
    done
    if [ "$ok" -eq 1 ]; then
        echo "[PASS] $name"
        passed=$((passed + 1))
    else
        echo "[FAIL] $name (journaux : $slog $dlog $qlog)"
        fail=1
    fi
}

# run_case NOM "CMDLINE" STATUT_ATTENDU MOTIF...
run_case() {
    local name="$1" append="$2" expect="$3"; shift 3
    local dlog="$OUT/$name.debugcon.log" slog="$OUT/$name.serial.log" qlog="$OUT/$name.qemu.log"
    local img; img=$(image_path)
    total=$((total + 1))
    rm -f "$dlog" "$slog" "$qlog"      # jamais de journal périmé (défaut K0.3)
    timeout "$TIMEOUT" "$QEMU" -m "${QEMU_MEM:-128M}" -display none -no-reboot \
        -kernel "$img" -append "$append" \
        -debugcon "file:$dlog" -serial "file:$slog" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -d cpu_reset,guest_errors -D "$qlog" </dev/null >/dev/null 2>&1
    verdict "$name" $? "$expect" "$dlog" "$slog" "$qlog" "$@"
}

# run_case_keys NOM "CMDLINE" STATUT "touche1 touche2 ..." MOTIF...
# Injecte de vraies frappes clavier par le moniteur QEMU (« sendkey ») dès que
# le noyau a écrit « [TEST] kbd ready ».
run_case_keys() {
    local name="$1" append="$2" expect="$3" keys="$4"; shift 4
    local dlog="$OUT/$name.debugcon.log" slog="$OUT/$name.serial.log" qlog="$OUT/$name.qemu.log"
    local img; img=$(image_path)
    total=$((total + 1))
    rm -f "$dlog" "$slog" "$qlog"
    feeder() {
        for _ in $(seq 1 100); do grep -q 'kbd ready' "$dlog" 2>/dev/null && break; sleep 0.1; done
        for k in $keys; do echo "sendkey $k"; sleep 0.05; done
        sleep "$TIMEOUT"
    }
    timeout "$TIMEOUT" "$QEMU" -m "${QEMU_MEM:-128M}" -display none -no-reboot \
        -kernel "$img" -append "$append" \
        -debugcon "file:$dlog" -serial "file:$slog" -monitor stdio \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -d cpu_reset,guest_errors -D "$qlog" < <(feeder) >/dev/null 2>&1
    verdict "$name" $? "$expect" "$dlog" "$slog" "$qlog" "$@"
}

# shellcheck source=tests/qemu/cases.sh
source "${CASES:-$(dirname "$0")/cases.sh}"

if [ "$total" -eq 0 ]; then
    echo "[FAIL] aucun cas exécuté"
    exit 2
fi
echo "QEMU: $total cas, $passed reussis, $((total - passed)) echecs"
exit $fail
