#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/emu_selftest.sh               (K1.1-M64-0)
# -----------------------------------------------------------------------------
# Tests NÉGATIFS du choix d'émulateur, des vérifications GDB et de la
# comparaison entre émulateurs. Dans chaque situation, le banc DOIT échouer
# avec le statut indiqué ; sinon, une absence de validation pourrait passer
# pour un succès.
#   E1 émulateur absent                 run.sh et gdb_checks.sh -> statut 2
#   E2 émulateur invalide (/bin/true)   run.sh et gdb_checks.sh -> statut 2
#   E3 émulateur d'une autre architecture (faux QEMU répondant par QMP
#      « cortex-a57-arm-cpu »)          run.sh et gdb_checks.sh -> statut 2
#   E4 résultat de test faux : faux GDB renvoyant ESPMOD16=8, ARG0=0 ...
#                                       gdb_checks.sh -> statut 1, aucun [PASS]
#   E5 erreur GDB réelle : architecture GDB inadaptée à l'émulateur choisi
#      (i386:x86-64 sous qemu-system-i386 ; i386 sous qemu-system-x86_64,
#      « Remote 'g' packet reply is too long »)
#                                       gdb_checks.sh -> statut 1, aucun [PASS]
#   E6 GDB en échec (statut 1) mais valeurs correctes affichées
#                                       gdb_checks.sh -> statut 1
#   E7 comparaison : résultats identiques -> 0 (contre-épreuve) ; verdict
#      différent, SKIP différent, cas manquant -> 1 ; résultats absents -> 2 ;
#      mesure seule différente « (avant 0x1) » / « (avant 0x2) » -> 0 AVEC une
#      ligne [INFO] qui la montre ; statut différent sous une même mesure -> 1
# Usage : emu_selftest.sh build/samos.elf build/samos-test.elf
#         (émulateur testé : variable QEMU, défaut qemu-system-i386)
# =============================================================================
set -u
PROD="${1:?usage: emu_selftest.sh build/samos.elf build/samos-test.elf}"
TEST="${2:?}"
QEMU="${QEMU:-qemu-system-i386}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(dirname "$TEST")/tests/emu_selftest"
rm -rf "$TMP"; mkdir -p "$TMP/bin"
fail=0

# expect_status NOM STATUT_ATTENDU [CONTRAINTE] COMMANDE... ; CONTRAINTE « nopass »
# exige en plus qu'aucune ligne [PASS] ne soit produite.
expect_status() {
    local name="$1" want="$2" rule="$3"; shift 3
    local rc
    "$@" > "$TMP/$name.log" 2>&1
    rc=$?
    if [ "$rc" -ne "$want" ]; then
        echo "[FAIL] emu : $name -> statut $rc, $want attendu (voir $TMP/$name.log)"; fail=1
    elif [ "$rule" = nopass ] && grep -q '^\[PASS\]' "$TMP/$name.log"; then
        echo "[FAIL] emu : $name -> statut $rc mais des [PASS] ont ete produits (voir $TMP/$name.log)"; fail=1
    else
        echo "[PASS] emu : $name -> statut $rc comme attendu"
    fi
}
CASES1="$TMP/one.cases.sh"
echo 'IMG=prod TIMEOUT=2 run_case x "" 124 "boot"' > "$CASES1"

# ---- E1-E3 : émulateur refusé ----------------------------------------------
cat > "$TMP/bin/fake-arm-qemu" <<'EOF'
#!/usr/bin/env bash
# Faux émulateur : se présente comme une machine ARM par QMP.
[ "${1:-}" = --version ] && { echo "QEMU emulator version 0.0 (faux)"; exit 0; }
cat > /dev/null
echo '{"QMP": {}}'
echo '{"return": {}}'
echo '{"return": "cortex-a57-arm-cpu"}'
echo '{"return": "faux"}'
echo '{"return": "virt-machine"}'
EOF
chmod +x "$TMP/bin/fake-arm-qemu"
for e in "E1_absent:/nonexistent/qemu-system-i386" "E2_invalid:/bin/true" "E3_other_arch:$TMP/bin/fake-arm-qemu"; do
    n="${e%%:*}" q="${e#*:}"
    expect_status "${n}_run" 2 nopass env QEMU="$q" TEST_OUT="$TMP/${n}_run" CASES="$CASES1" "$ROOT/tests/qemu/run.sh" "$PROD" "$TEST"
    expect_status "${n}_gdb" 2 nopass env QEMU="$q" TEST_OUT="$TMP/${n}_gdb" "$ROOT/tests/qemu/gdb_checks.sh" "$PROD"
done

# ---- E4, E6 : faux GDB -------------------------------------------------------
cat > "$TMP/bin/gdb-wrong" <<'EOF'
#!/usr/bin/env bash
# Faux GDB : connexion « réussie » mais valeurs fausses.
[ "${1:-}" = --version ] && { echo "GNU gdb (faux) 0"; exit 0; }
printf 'ESPMOD16=8\nARG0=0x0\nARG1=0x0\nDF=1\nIF=1\nINSTACK=0\n'
exit 0
EOF
cat > "$TMP/bin/gdb-error" <<'EOF'
#!/usr/bin/env bash
# Faux GDB : valeurs correctes affichées, mais statut d'échec.
[ "${1:-}" = --version ] && { echo "GNU gdb (faux) 0"; exit 0; }
printf 'ESPMOD16=12\nARG0=0x2badb002\nARG1=0x9500\nDF=0\nIF=0\nINSTACK=1\n'
exit 1
EOF
chmod +x "$TMP/bin/gdb-wrong" "$TMP/bin/gdb-error"
expect_status E4_wrong_values 1 nopass env QEMU="$QEMU" GDB="$TMP/bin/gdb-wrong" TEST_OUT="$TMP/E4" \
    GDB_PORT=1241 "$ROOT/tests/qemu/gdb_checks.sh" "$PROD"
expect_status E6_gdb_status 1 any env QEMU="$QEMU" GDB="$TMP/bin/gdb-error" TEST_OUT="$TMP/E6" \
    GDB_PORT=1242 "$ROOT/tests/qemu/gdb_checks.sh" "$PROD"
grep -q '^\[FAIL\] gdb: statut 1' "$TMP/E6_gdb_status.log" ||
    { echo "[FAIL] emu : E6 sans diagnostic « statut 1 » (voir $TMP/E6_gdb_status.log)"; fail=1; }

# ---- E5 : vraie erreur GDB (architecture inadaptée) -------------------------
# shellcheck source=tests/qemu/emu.sh
source "$ROOT/tests/qemu/emu.sh"
if emu_probe "$QEMU" > "$TMP/probe.log"; then
    if [ "$EMU_TARGET" = i386 ]; then wrong="i386:x86-64"; else wrong=""; fi
    expect_status E5_gdb_arch_mismatch 1 nopass env QEMU="$QEMU" GDB_ARCH="$wrong" TEST_OUT="$TMP/E5" \
        GDB_PORT=1243 "$ROOT/tests/qemu/gdb_checks.sh" "$PROD"
    grep -q '^\[FAIL\] gdb: erreur GDB\|^\[FAIL\] gdb: statut' "$TMP/E5_gdb_arch_mismatch.log" ||
        { echo "[FAIL] emu : E5 sans diagnostic d'erreur GDB (voir $TMP/E5_gdb_arch_mismatch.log)"; fail=1; }
else
    echo "[FAIL] emu : E5 impossible, emulateur $QEMU refuse ($(cat "$TMP/probe.log"))"; fail=1
fi

# ---- E7 : comparaison entre émulateurs -------------------------------------
mkres() {   # mkres DIR VERDICT_CAS_B SKIP_16M [omettre_cas]
    mkdir -p "$1"
    { echo "[EMU] faux"; echo "[PASS] a"; echo "[$2] b"; [ "${4:-}" = omit ] || echo "[PASS] c"
      echo "QEMU: 3 cas, 3 reussis, 0 echecs"; } > "$1/qemu.out"
    printf '[PASS] gdb: x\n' > "$1/gdb.out"
    echo "[TEST] skipped=$3" > "$1/selftest_16M.serial.log"
}
mkres "$TMP/cmp/ref" PASS 1
mkres "$TMP/cmp/same" PASS 1
mkres "$TMP/cmp/verdict" FAIL 1
mkres "$TMP/cmp/skip" PASS 0
mkres "$TMP/cmp/missing" PASS 1 omit
mkdir -p "$TMP/cmp/absent"
for v in "meas1:PASS:0x1" "meas2:PASS:0x2" "meas3:FAIL:0x1"; do
    IFS=: read -r n st val <<<"$v"
    mkres "$TMP/cmp/$n" PASS 1
    echo "[$st] d_page_intact (avant $val, refus $val)" >> "$TMP/cmp/$n/qemu.out"
done
C="$ROOT/tests/qemu/emu_compare.sh"
expect_status E7_identical 0 any "$C" "$TMP/cmp/ref" "$TMP/cmp/same"
expect_status E7_verdict_diverges 1 any "$C" "$TMP/cmp/ref" "$TMP/cmp/verdict"
expect_status E7_skip_diverges 1 any "$C" "$TMP/cmp/ref" "$TMP/cmp/skip"
expect_status E7_case_missing 1 any "$C" "$TMP/cmp/ref" "$TMP/cmp/missing"
expect_status E7_results_absent 2 any "$C" "$TMP/cmp/ref" "$TMP/cmp/absent"
expect_status E7_measure_only_differs 0 any "$C" "$TMP/cmp/meas1" "$TMP/cmp/meas2"
grep -q '^\[INFO\] mesure differente, B : \[PASS\] d_page_intact (avant 0x2' "$TMP/E7_measure_only_differs.log" ||
    { echo "[FAIL] emu : ecart de mesure non affiche (voir $TMP/E7_measure_only_differs.log)"; fail=1; }
expect_status E7_status_diverges_same_measure 1 any "$C" "$TMP/cmp/meas1" "$TMP/cmp/meas3"
grep -q 'DIVERGENCE' "$TMP/E7_verdict_diverges.log" ||
    { echo "[FAIL] emu : divergence non nommee (voir $TMP/E7_verdict_diverges.log)"; fail=1; }

exit $fail
