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
#   E7 comparaison (22 cas, données synthétiques falsifiées) : identiques
#      -> 0 ; échec d'un côté OU des deux, cas renommé/manquant, verdict hors
#      bilan, SKIP différent, inattendu ou de nom inconnu (même des deux
#      côtés), échec GDB commun, mesure stable ou parenthèse quelconque
#      modifiée, statut différent sous une même mesure -> 1 ; bilan absent,
#      résultats absents, ligne skipped= absente, GDB tronqué, erreur
#      interne du filtre -> 2 ; mesure
#      AUTORISÉE différente -> 0 avec [INFO] ; LIMITE : faux PASS commun et
#      cohérent -> 0 (non détectable par comparaison)
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
# Résultats synthétiques complets et cohérents (« ref »), puis une variante
# falsifiée par cas. Attentes du comparateur fixées pour ces données :
# SKIP attendu « selftest_16M:1 » (nom k), autre journal « other:0 », mesure
# variable autorisée pour le seul cas d_page_intact.
export SKIP_EXPECT="selftest_16M:1 other:0" SKIP_NAMES="selftest_16M:k" VARIABLE_MEASURES="d_page_intact"
mkres() {   # mkres DIR : résultats sains
    mkdir -p "$1"
    printf '%s\n' "[EMU] faux" "[PASS] a" "[PASS] b" "[PASS] c" \
        "[PASS] d_page_intact (avant 0x1, refus 0x1)" "[PASS] s_page_intact (avant 0x5, refus 0x5)" \
        "QEMU: 5 cas, 5 reussis, 0 echecs" > "$1/qemu.out"
    printf '%s\n' "[PASS] gdb: x" "[PASS] gdb: y" "GDB: 2 verifications, 2 reussies, 0 echecs" > "$1/gdb.out"
    printf '%s\n' "[SKIP] k: precondition" "[TEST] skipped=1" > "$1/selftest_16M.serial.log"
    printf '%s\n' "[TEST] skipped=0" > "$1/other.serial.log"
}
# variant NOM FICHIER 'EXPR_SED' [FICHIER 'EXPR_SED'...] : copie de ref falsifiée
variant() {
    local d="$TMP/cmp/$1"; shift
    mkres "$d"
    while [ "$#" -ge 2 ]; do sed -i "$2" "$d/$1"; shift 2; done
}
mkres "$TMP/cmp/ref"; mkres "$TMP/cmp/ref2"
C="$ROOT/tests/qemu/emu_compare.sh"
# cmp_case NOM STATUT A B : compare deux répertoires de $TMP/cmp
cmp_case() { expect_status "E7_$1" "$2" any "$C" "$TMP/cmp/$3" "$TMP/cmp/$4"; }

cmp_case identical 0 ref ref2
variant fail_one qemu.out 's/^\[PASS\] b$/[FAIL] b/; s/5 reussis, 0 echecs/4 reussis, 1 echecs/'
cmp_case verdict_fails_one_side 1 ref fail_one
cmp_case same_fail_both_sides 1 fail_one fail_one
variant renamed qemu.out 's/^\[PASS\] b$/[PASS] b2/'
cmp_case case_renamed 1 ref renamed
variant missing qemu.out '/^\[PASS\] c$/d; s/5 cas, 5 reussis/4 cas, 4 reussis/'
cmp_case case_missing_coherent_bilan 1 ref missing
variant incoherent qemu.out '$a [PASS] z'
cmp_case extra_verdict_not_in_bilan 1 ref incoherent
variant nobilan qemu.out '/^QEMU: /d'
cmp_case bilan_absent 2 ref nobilan
mkdir -p "$TMP/cmp/absent"
cmp_case results_absent 2 ref absent
variant skip0 selftest_16M.serial.log 's/skipped=1/skipped=0/; /^\[SKIP\]/d'
cmp_case skip_count_differs 1 ref skip0
variant skipboth other.serial.log 's/skipped=0/skipped=1/'
cmp_case unexpected_skip_both_sides 1 skipboth skipboth
variant noskipline selftest_16M.serial.log '/skipped=/d'
cmp_case skip_line_absent_both_sides 2 noskipline noskipline
variant skipname selftest_16M.serial.log 's/^\[SKIP\] k:/[SKIP] autre:/'
cmp_case unexpected_skip_name_both_sides 1 skipname skipname
variant gdbtrunc gdb.out '/^\[PASS\] gdb: y$/d; /^GDB: /d'
cmp_case gdb_truncated_both_sides 2 gdbtrunc gdbtrunc
variant gdbfail gdb.out 's/^\[PASS\] gdb: y$/[FAIL] gdb: y/; s/2 reussies, 0 echecs/1 reussies, 1 echecs/'
cmp_case gdb_fail_both_sides 1 gdbfail gdbfail
variant gdbstatus gdb.out '1i [FAIL] gdb: statut 1 (voir x)'
cmp_case gdb_error_line_both_sides 1 gdbstatus gdbstatus
variant meas2 qemu.out 's/d_page_intact (avant 0x1, refus 0x1)/d_page_intact (avant 0x2, refus 0x3)/'
cmp_case allowed_measure_differs 0 ref meas2
grep -q '^\[INFO\] mesure differente (autorisee), B : \[PASS\] d_page_intact (avant 0x2, refus 0x3)$' "$TMP/E7_allowed_measure_differs.log" ||
    { echo "[FAIL] emu : ecart de mesure autorise non affiche (voir $TMP/E7_allowed_measure_differs.log)"; fail=1; }
variant measbad qemu.out 's/d_page_intact (avant 0x1, refus 0x1)/d_page_intact (zero)/'
cmp_case allowed_case_malformed_measure 1 ref measbad
variant stable qemu.out 's/s_page_intact (avant 0x5, refus 0x5)/s_page_intact (avant 0x6, refus 0x6)/'
cmp_case stable_measure_differs 1 ref stable
variant paren qemu.out 's/^\[PASS\] a$/[PASS] a (note)/'
cmp_case arbitrary_parenthesis_added 1 ref paren
variant measfail qemu.out 's/^\[PASS\] d_page_intact/[FAIL] d_page_intact/; s/5 reussis, 0 echecs/4 reussis, 1 echecs/'
cmp_case status_differs_same_measure 1 ref measfail
# Erreur interne du filtre de mesures (expression sed invalide) : ne doit
# JAMAIS produire deux résumés vides « identiques » (défaut trouvé à l'audit).
expect_status E7_internal_filter_error 2 any env VARIABLE_MEASURES='bad(' "$C" "$TMP/cmp/ref" "$TMP/cmp/ref2"
grep -q 'DIVERGENCE' "$TMP/E7_case_renamed.log" ||
    { echo "[FAIL] emu : divergence non nommee (voir $TMP/E7_case_renamed.log)"; fail=1; }
# LIMITE documentée (et vérifiée pour ne pas être oubliée) : un faux PASS
# commun aux deux côtés ET cohérent avec les bilans n'est pas détectable par
# comparaison. Ce test attend 0 : il prouve la limite, pas une correction.
variant fake1 qemu.out '$a [PASS] z' qemu.out 's/5 cas, 5 reussis/6 cas, 6 reussis/'
variant fake2 qemu.out '$a [PASS] z' qemu.out 's/5 cas, 5 reussis/6 cas, 6 reussis/'
cmp_case LIMIT_common_coherent_fake_pass 0 fake1 fake2
unset SKIP_EXPECT SKIP_NAMES VARIABLE_MEASURES
exit $fail
