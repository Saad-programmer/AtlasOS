#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/harness_selftest.sh
# -----------------------------------------------------------------------------
# Tests NÉGATIFS du banc QEMU lui-même (K0.7) : dans chacune de ces
# situations, tests/qemu/run.sh DOIT échouer. Un banc incapable d'échouer
# produirait des faux positifs.
#   H1 QEMU introuvable
#   H2 image absente
#   H3 motif attendu inexistant
#   H4 statut attendu incorrect
#   H5 motif interdit présent
#   H6 aucun cas exécuté
#   H7 triple fault non attendu
#   H8 image inconnue (IMG=xyz)
# =============================================================================
set -u
PROD="${1:?usage: harness_selftest.sh build/samos.elf build/samos-test.elf}"
TEST="${2:?}"
TMP="$(dirname "$TEST")/tests/harness"; mkdir -p "$TMP"
fail=0
must_fail() {   # NOM COMMANDE...
    local name="$1"; shift
    if "$@" >"$TMP/$name.log" 2>&1; then
        echo "[FAIL] banc : $name n'a PAS échoué (faux positif possible, voir $TMP/$name.log)"; fail=1
    else
        echo "[PASS] banc : $name échoue comme attendu"
    fi
}
mkcases() { printf '%s\n' "$2" > "$TMP/$1.cases.sh"; echo "$TMP/$1.cases.sh"; }

must_fail H1_no_qemu env QEMU=/nonexistent/qemu CASES="$(mkcases h1 'TIMEOUT=2 run_case x "" 124 "boot"')" \
    tests/qemu/run.sh "$PROD" "$TEST"
must_fail H2_no_image tests/qemu/run.sh /nonexistent.elf "$TEST"
must_fail H3_missing_pattern env CASES="$(mkcases h3 'IMG=prod TIMEOUT=2 run_case x "" 124 "THIS TEXT NEVER APPEARS"')" \
    tests/qemu/run.sh "$PROD" "$TEST"
must_fail H4_wrong_status env CASES="$(mkcases h4 'run_case x "test=panic" 33 "KERNEL PANIC"')" \
    tests/qemu/run.sh "$PROD" "$TEST"
must_fail H5_forbidden_present env CASES="$(mkcases h5 'run_case x "test=panic" 37 "!KERNEL PANIC"')" \
    tests/qemu/run.sh "$PROD" "$TEST"
must_fail H6_no_case env CASES="$(mkcases h6 '# vide')" tests/qemu/run.sh "$PROD" "$TEST"
must_fail H7_unexpected_triple env CASES="$(mkcases h7 'run_case x "test=triple" 0')" \
    tests/qemu/run.sh "$PROD" "$TEST"
must_fail H8_unknown_image env CASES="$(mkcases h8 'IMG=xyz TIMEOUT=2 run_case x "" 124')" \
    tests/qemu/run.sh "$PROD" "$TEST"
# Contre-épreuve : un cas correct DOIT réussir (le banc n'échoue pas tout le temps).
if env CASES="$(mkcases ok 'run_case x "test=panic" 37 "KERNEL PANIC"')" tests/qemu/run.sh "$PROD" "$TEST" >"$TMP/ok.log" 2>&1; then
    echo "[PASS] banc : contre-épreuve réussie"
else
    echo "[FAIL] banc : un cas correct échoue (voir $TMP/ok.log)"; fail=1
fi
exit $fail
