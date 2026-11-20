#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/qemu/emu_compare.sh                 (K1.1-M64-0)
# -----------------------------------------------------------------------------
# Compare les résultats de deux exécutions des bancs (une par émulateur)
# produites par tests/qemu/emu_matrix.sh. Usage :
#   emu_compare.sh DIR_A DIR_B
# Chaque répertoire contient qemu.out (sortie de run.sh), gdb.out (sortie de
# gdb_checks.sh) et les journaux série des cas (*.serial.log).
#
# Le comparateur NE SE CONTENTE PAS de la parité : deux exécutions qui
# échouent de la même manière ne sont jamais un succès. Étapes :
#
# 1. Chaque répertoire est validé SEUL (sortie 2 si incomplet, 1 si en échec) :
#    - qemu.out : une seule ligne « QEMU: N cas, R reussis, E echecs », avec
#      R = N et E = 0, et exactement N lignes de verdict [PASS]/[FAIL] ;
#    - gdb.out : une seule ligne « GDB: N verifications, R reussies, E echecs »,
#      avec R = N, E = 0, et exactement N lignes « [PASS] gdb: » ;
#    - aucune ligne [FAIL] ni [SKIP] dans qemu.out et gdb.out ;
#    - SKIP des autotests : chaque journal listé dans SKIP_EXPECT doit porter
#      « [TEST] skipped=N » avec N attendu ; tout autre journal doit porter 0 ;
#      chaque ligne « [SKIP] nom: » doit figurer dans SKIP_NAMES.
#    Ces attentes recopient celles de tests/qemu/cases.sh (contrôle croisé).
#
# 2. Résumé normalisé, comparé ligne à ligne, sans tolérance : verdicts
#    (statut + nom + texte), bilans, « skip CAS N », lignes [SKIP] des
#    journaux. Toute différence est une DIVERGENCE (sortie 1).
#
# Mesures autorisées à différer : UNIQUEMENT la parenthèse
# « (avant 0xH, refus 0xH) » des cas listés dans VARIABLE_MEASURES. Mesuré
# en K1.1-M64-0 : l'empreinte de la pile active (n1stack) change d'une
# exécution à l'autre sous le même émulateur ; celle de la mémoire basse
# (n1low, écrite par le BIOS) diffère entre les deux machines. Leur règle
# (empreinte différente d'une page remise à zéro) est jugée par run.sh. Toute
# autre parenthèse (empreintes des pages stables n1gdt, n1data, n1alloc,
# n3type, n3table, notes, chemins) est comparée telle quelle. Les écarts
# autorisés sont LISTÉS ([INFO] mesure), jamais cachés.
#
# Non comparé : le texte brut des journaux (CPU qemu32/qemu64, région E820
# supplémentaire au-delà de 4 GiB sous x86_64) ; le nombre de régions est
# affiché ([INFO]).
#
# LIMITE : une erreur commune aux deux bancs et COHÉRENTE avec leurs bilans
# (faux PASS identique des deux côtés) n'est pas détectable par comparaison.
# La parité n'est pas une preuve de correction du noyau ; celle-ci repose sur
# les assertions, les tests négatifs et les mutations.
# =============================================================================
set -u
set -o pipefail   # une étape de filtrage en erreur ne doit jamais vider un résumé en silence
A="${1:?usage: emu_compare.sh DIR_A DIR_B}"
B="${2:?usage: emu_compare.sh DIR_A DIR_B}"
SKIP_EXPECT="${SKIP_EXPECT:-selftest:0 selftest_16M:1 selftest_512M:0 selftest_4G:0 imageint:0 n3:0 pdeint:0 pmmtrust:0 tablelife:0 tlbexp:0 tlbvmm:0}"
SKIP_NAMES="${SKIP_NAMES:-selftest_16M:memmap_bench_total_over_100mib}"
VARIABLE_MEASURES="${VARIABLE_MEASURES:-n1stack_page_intact n1low_page_intact}"
MEAS_RE='\(avant 0x[0-9a-f]{1,8}, refus 0x[0-9a-f]{1,8}\)'

worst() { if [ "$1" -ge "$2" ]; then echo "$1"; else echo "$2"; fi; }

# check_bilan FICHIER BILAN_RE VERDICT_RE : bilan unique, sans échec, cohérent
# avec le nombre de lignes de verdict ; aucune ligne [FAIL] ni [SKIP].
check_bilan() {
    local f="$1" re="$2" vre="$3" st=0 cnt line n r e got
    cnt=$(grep -cE "^$re\$" "$f")
    if [ "$cnt" -ne 1 ]; then
        echo "[FAIL] comparaison : $f : $cnt ligne(s) de bilan (1 requise)"; return 2
    fi
    line=$(grep -E "^$re\$" "$f")
    n=$(sed -E "s/^$re\$/\\1/" <<<"$line"); r=$(sed -E "s/^$re\$/\\2/" <<<"$line"); e=$(sed -E "s/^$re\$/\\3/" <<<"$line")
    got=$(grep -cE "$vre" "$f")
    if [ "$e" -ne 0 ] || [ "$r" -ne "$n" ]; then
        echo "[FAIL] comparaison : $f : bilan en echec ($line)"; st=1
    fi
    if [ "$got" -ne "$n" ]; then
        echo "[FAIL] comparaison : $f : $got verdict(s) pour un bilan de $n (incoherent)"; st=1
    fi
    if grep -qE '^\[(FAIL|SKIP)\]' "$f"; then
        echo "[FAIL] comparaison : $f : ligne(s) [FAIL] ou [SKIP] presente(s)"; st=1
    fi
    return "$st"
}

# validate DIR : statut 0 si complet, cohérent et sans échec ; 1 ou 2 sinon.
validate() {
    local d="$1" st=0 line f base want got name
    if [ ! -d "$d" ] || [ ! -s "$d/qemu.out" ] || [ ! -s "$d/gdb.out" ]; then
        echo "[FAIL] comparaison : resultats absents dans $d"; return 2
    fi
    check_bilan "$d/qemu.out" 'QEMU: ([0-9]+) cas, ([0-9]+) reussis, ([0-9]+) echecs' '^\[(PASS|FAIL)\] ' || st=$(worst "$st" $?)
    check_bilan "$d/gdb.out" 'GDB: ([0-9]+) verifications, ([0-9]+) reussies, ([0-9]+) echecs' '^\[PASS\] gdb: ' || st=$(worst "$st" $?)
    for spec in $SKIP_EXPECT; do
        base="${spec%%:*}" want="${spec#*:}"
        got=$(grep -hoE '^\[TEST\] skipped=[0-9]+' "$d/$base.serial.log" 2>/dev/null | sed 's/.*=//' | head -1)
        if [ -z "$got" ]; then
            echo "[FAIL] comparaison : $d/$base.serial.log : ligne « [TEST] skipped= » absente"; st=2
        elif [ "$got" != "$want" ]; then
            echo "[FAIL] comparaison : $d/$base : skipped=$got, $want attendu"; st=$(worst "$st" 1)
        fi
    done
    for f in "$d"/*.serial.log; do
        [ -f "$f" ] || continue
        base="$(basename "$f" .serial.log)"
        case " $SKIP_EXPECT " in *" $base:"*) ;; *)
            got=$(grep -hoE '^\[TEST\] skipped=[0-9]+' "$f" | sed 's/.*=//' | head -1)
            if [ -n "$got" ] && [ "$got" != 0 ]; then
                echo "[FAIL] comparaison : $d/$base : skipped=$got non attendu"; st=$(worst "$st" 1)
            fi ;;
        esac
        while IFS= read -r line; do
            name="${line#\[SKIP\] }"; name="${name%%:*}"
            case " $SKIP_NAMES " in *" $base:$name "*) ;; *)
                echo "[FAIL] comparaison : $d/$base : SKIP non attendu ($line)"; st=$(worst "$st" 1) ;;
            esac
        done < <(grep -E '^\[SKIP\] ' "$f")
    done
    return "$st"
}

# Retire la parenthèse de mesure des SEULS cas autorisés, au format exact.
strip_allowed() {
    local c args=()
    for c in $VARIABLE_MEASURES; do args+=(-e "s/^(\\[PASS\\] $c) $MEAS_RE\$/\\1/"); done
    sed -E "${args[@]}"
}

# g : grep dont « aucune ligne » (statut 1) n'est pas une erreur ; statut 2
# (fichier illisible, expression invalide) en reste une.
g() { grep "$@"; local r=$?; [ "$r" -le 1 ]; }

summary() {   # summary DIR : résumé normalisé ; statut non nul à la moindre erreur
    local d="$1" f base err=0
    { g -E '^\[(PASS|FAIL|SKIP)\] |^QEMU: [0-9]+ cas' "$d/qemu.out" | strip_allowed; } || err=1
    g -E '^\[(PASS|FAIL|SKIP)\] |^GDB: [0-9]+ verifications' "$d/gdb.out" || err=1
    for f in "$d"/*.serial.log; do
        [ -f "$f" ] || continue
        base="$(basename "$f" .serial.log)"
        { g -hE '^\[TEST\] skipped=[0-9]+' "$f" | sed "s/^\[TEST\] skipped=/skip $base /"; } || err=1
        { g -hE '^\[SKIP\] ' "$f" | sed "s/^/$base /"; } || err=1
    done
    return "$err"
}

status=0
for d in "$A" "$B"; do
    validate "$d"; rc=$?
    [ "$rc" -le "$status" ] || status=$rc
done
if [ "$status" -ne 0 ]; then
    echo "[FAIL] comparaison : au moins un banc est absent, incomplet ou en echec ; la parite ne vaut pas succes"
    exit "$status"
fi

# Résumé : toute erreur de construction (sed, grep en erreur = statut 2) ou un
# résumé vide est un échec, jamais un accord entre deux résumés vides.
build() {   # build DIR : résumé trié ; statut non nul si une étape a échoué
    local out
    out="$(summary "$1" 2> >(sed 's/^/[FAIL] comparaison : erreur interne : /' >&2))" || return 1
    [ -n "$out" ] || return 1
    printf '%s\n' "$out" | LC_ALL=C sort
}
SA="$(build "$A")" || { echo "[FAIL] comparaison : resume impossible pour $A"; exit 2; }
SB="$(build "$B")" || { echo "[FAIL] comparaison : resume impossible pour $B"; exit 2; }
echo "[INFO] $A : $(printf '%s\n' "$SA" | wc -l) lignes de resume ; $B : $(printf '%s\n' "$SB" | wc -l)"
for d in "$A" "$B"; do
    echo "[INFO] $d : $(grep -m1 -hoE 'mm: [0-9]+ regions, [0-9]+ KiB available' "$d/gdb_vga.serial.log" 2>/dev/null || echo 'carte memoire non relevee')"
done
# Écarts autorisés : affichés pour rester visibles.
allowed() {
    local c
    for c in $VARIABLE_MEASURES; do grep -hE "^\[PASS\] $c $MEAS_RE\$" "$1/qemu.out"; done | LC_ALL=C sort
}
diff <(allowed "$A") <(allowed "$B") | sed -n 's/^< /[INFO] mesure differente (autorisee), A : /p; s/^> /[INFO] mesure differente (autorisee), B : /p'
if [ "$SA" = "$SB" ]; then
    echo "[PASS] comparaison : bancs complets et sans echec ; verdicts, bilans et SKIP identiques sous les deux emulateurs"
    exit 0
fi
echo "[FAIL] comparaison : DIVERGENCE entre $A et $B"
diff <(printf '%s\n' "$SA") <(printf '%s\n' "$SB") | sed 's/^/        /'
exit 1
