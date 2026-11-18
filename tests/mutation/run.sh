#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/mutation/run.sh            (make test-mutation, K0.7-R0)
# -----------------------------------------------------------------------------
# Vérifie que le banc de tests DÉTECTE des défauts connus : chaque mutation
# réintroduit un défaut corrigé (E3, E5, M2.5c...) dans une COPIE ISOLÉE des
# sources, recompile l'image de test depuis zéro, démarre les autotests sous
# QEMU, et exige un échec signalé par des assertions précises.
#
# Verdict d'une mutation :
#   DÉTECTÉE     compilation réussie, statut QEMU attendu (35 = échecs
#                d'autotests), ligne de bilan « [TEST] suites=... failures=N »
#                avec N > 0, chaque assertion attendue présente en « [FAIL] »,
#                aucun triple fault ni reset inattendu ;
#   NON DÉTECTÉE le noyau muté passe les autotests (statut 33) ;
#   INVALIDE     tout le reste : texte original introuvable ou ambigu, fichier
#                inchangé, compilation en échec, binaire identique à la
#                référence, délai dépassé, panique, triple fault, bilan absent,
#                assertion attendue absente. Une mutation INVALIDE n'est jamais
#                comptée comme détectée.
# La version SAINE est d'abord compilée et exécutée de la même façon : elle
# doit passer (statut 33, 0 échec), sinon rien n'est mesuré.
#
# Isolement : les fichiers suivis par Git sont copiés (état de l'arbre de
# travail) dans un répertoire temporaire ; le dépôt n'est jamais modifié, ce
# qui est vérifié à la fin (HEAD, branche et « git status » inchangés). Le
# répertoire temporaire est supprimé à la sortie, y compris sur interruption.
# Chaque copie part sans répertoire build/ : aucun ancien binaire possible.
#
# Codes de sortie : 0 toutes détectées ; 1 au moins une non détectée ou
# invalide ; 2 prérequis manquant ou version saine en échec ; 130 interruption.
# Variables : MUTATION_CASES (fichier de cas), QEMU, TIMEOUT, QEMU_MEM, JOBS,
#             KEEP=1 (conserver le répertoire temporaire pour diagnostic).
# =============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CASES_FILE="${MUTATION_CASES:-$ROOT/tests/mutation/cases.sh}"
QEMU="${QEMU:-qemu-system-i386}"
TIMEOUT="${TIMEOUT:-30}"
QEMU_MEM="${QEMU_MEM:-128M}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
IMAGE=build/samos-test.elf

die() { echo "[FAIL] $*"; exit 2; }

# --- Prérequis ---------------------------------------------------------------
for tool in git tar awk make timeout cmp sha256sum mktemp clang nasm ld "$QEMU"; do
    command -v "$tool" >/dev/null 2>&1 || die "prérequis absent : $tool"
done
[ -f "$CASES_FILE" ] || die "fichier de cas absent : $CASES_FILE"
git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1 || die "$ROOT n'est pas un dépôt Git"

# --- Répertoire temporaire et nettoyage ---------------------------------------
WORK="$(mktemp -d "${TMPDIR:-/tmp}/samos-mutation.XXXXXX")" || die "mktemp a échoué"
cleanup() {
    if [ "${KEEP:-0}" = 1 ]; then
        echo "  (KEEP=1 : répertoire conservé : $WORK)"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'echo "[FAIL] interrompu"; exit 130' INT TERM HUP

# État du dépôt, pour prouver à la fin qu'il n'a pas été modifié.
repo_state() {
    git -C "$ROOT" rev-parse HEAD
    git -C "$ROOT" symbolic-ref -q HEAD || echo detached
    git -C "$ROOT" status --porcelain=v1 --untracked-files=all
}
STATE_BEFORE="$(repo_state | sha256sum)"

# Copie des fichiers suivis (contenu de l'arbre de travail, sans build/).
SRC="$WORK/src"
mkdir -p "$SRC"
(cd "$ROOT" && git ls-files -z | tar --null -T - -cf -) | tar -xf - -C "$SRC" \
    || die "copie des sources impossible"
[ ! -e "$SRC/build" ] || die "la copie contient build/ (fichier suivi ?)"

# --- Outils --------------------------------------------------------------------
# Nombre d'occurrences exactes (littérales) de $2 dans le fichier $1.
# Les motifs passent par ENVIRON : « awk -v » interpréterait les séquences
# d'échappement (un « \n » littéral du source deviendrait un saut de ligne).
count_literal() {
    PAT="$2" awk 'BEGIN { n = 0; pat = ENVIRON["PAT"] }
        { s = $0; while ((i = index(s, pat)) > 0) { n++; s = substr(s, i + length(pat)) } }
        END { print n }' "$1"
}
# Remplace l'unique occurrence littérale de $2 par $3 dans $1.
replace_literal() {
    PAT="$2" REP="$3" awk 'BEGIN { pat = ENVIRON["PAT"]; rep = ENVIRON["REP"] } {
            i = index($0, pat)
            if (i > 0) { $0 = substr($0, 1, i - 1) rep substr($0, i + length(pat)) }
            print
        }' "$1" > "$1.mut" && mv "$1.mut" "$1"
}

# build_and_run DIR -> compile l'image de test, l'exécute ; positionne
# RUN_BUILD (code make), RUN_STATUS (code QEMU), RUN_LOG, RUN_QLOG.
build_and_run() {
    local dir="$1"
    RUN_LOG="$dir/selftest.log"; RUN_QLOG="$dir/qemu.log"
    rm -rf "$dir/build"
    make -C "$dir" -j"$JOBS" "$IMAGE" >"$dir/build.log" 2>&1
    RUN_BUILD=$?
    RUN_STATUS=-1
    if [ "$RUN_BUILD" -ne 0 ] || [ ! -f "$dir/$IMAGE" ]; then
        return
    fi
    timeout "$TIMEOUT" "$QEMU" -m "$QEMU_MEM" -display none -no-reboot \
        -kernel "$dir/$IMAGE" -append "${MUT_CMDLINE:-test}" \
        -debugcon "file:$dir/debugcon.log" -serial "file:$RUN_LOG" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -d cpu_reset,guest_errors -D "$RUN_QLOG" </dev/null >/dev/null 2>&1
    RUN_STATUS=$?
}

# Triple fault ou reset inattendu (même règle que tests/qemu/run.sh).
abnormal_cpu() {
    local triple resets
    [ -s "$1" ] || return 0                       # pas de journal : verdict impossible
    triple=$(grep -c 'Triple fault' "$1" || true)
    resets=$(grep -c 'CPU Reset' "$1" || true)
    [ "${triple:-0}" -gt 0 ] || [ "${resets:-0}" -gt "${QEMU_BOOT_RESETS:-2}" ]
}

summary_failures() {   # nombre N de « [TEST] suites=... failures=N », vide si absent
    tr -d '\r' < "$1" | sed -n 's/^\[TEST\] suites=[0-9]* checks=[0-9]* failures=\([0-9]*\)$/\1/p' | tail -1
}

# --- Version saine ---------------------------------------------------------------
echo "== version saine"
SANE="$WORK/sane"
cp -a "$SRC" "$SANE"
build_and_run "$SANE"
[ "$RUN_BUILD" -eq 0 ] || die "version saine : compilation en échec (journal : $SANE/build.log)"
sane_fail="$(summary_failures "$RUN_LOG")"
if [ "$RUN_STATUS" -ne 33 ] || [ "$sane_fail" != 0 ] || abnormal_cpu "$RUN_QLOG"; then
    tr -d '\r' < "$RUN_LOG" 2>/dev/null | grep '^\[FAIL\]' | head -5 | sed 's/^/      /'
    die "version saine en échec : statut $RUN_STATUS, failures=${sane_fail:-absent} ($RUN_LOG)"
fi
SANE_SHA="$(sha256sum < "$SANE/$IMAGE")"
echo "[PASS] version saine : statut 33, $(tr -d '\r' < "$RUN_LOG" | grep -o 'checks=[0-9]*' | tail -1), 0 échec"
# L'arbre sain est conservé : les mutations d'outil (mutant_tool) y exécutent
# leur commande de référence.

# sane_mode_ok CMDLINE -> vrai si la version saine réussit ce mode (résultat
# mémorisé : une seule exécution par mode). Le binaire sain est le même.
SANE_MODES=""
sane_mode_ok() {
    case " $SANE_MODES " in *" ok:$1 "*) return 0 ;; *" ko:$1 "*) return 1 ;; esac
    local log="$SANE/mode-$(echo "$1" | tr -c 'a-z0-9' '_').log" st nf
    timeout "$TIMEOUT" "$QEMU" -m "$QEMU_MEM" -display none -no-reboot \
        -kernel "$SANE/$IMAGE" -append "$1" -serial "file:$log" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 </dev/null >/dev/null 2>&1
    st=$?
    nf="$(summary_failures "$log")"
    if [ "$st" -eq 33 ] && [ "$nf" = 0 ]; then SANE_MODES="$SANE_MODES ok:$1"; return 0; fi
    SANE_MODES="$SANE_MODES ko:$1"; return 1
}

# --- Mutations ---------------------------------------------------------------------
total=0; detected=0; bad=0

# mutant NOM FICHIER 'TEXTE ORIGINAL' 'TEXTE MUTÉ' STATUT_ATTENDU 'ASSERTION'...
#   ASSERTION : texte exact d'une ligne « [FAIL] <ASSERTION> at ... » attendue.
mutant() {
    local name="$1" file="$2" orig="$3" repl="$4" want="$5"; shift 5
    local dir="$WORK/m$total" verdict="" why=""
    total=$((total + 1))
    RUN_LOG=/dev/null
    echo "== mutation $name ($file)"
    cp -a "$SRC" "$dir"
    local n; n="$(count_literal "$dir/$file" "$orig" 2>/dev/null || echo 0)"
    if [ ! -f "$dir/$file" ]; then
        verdict=INVALIDE; why="fichier absent"
    elif [ "$n" -ne 1 ]; then
        verdict=INVALIDE; why="texte original trouvé $n fois (exactement 1 requis)"
    else
        replace_literal "$dir/$file" "$orig" "$repl"
        if cmp -s "$SRC/$file" "$dir/$file"; then
            verdict=INVALIDE; why="fichier inchangé après substitution"
        elif [ "$(count_literal "$dir/$file" "$repl")" -lt 1 ]; then
            verdict=INVALIDE; why="texte muté absent après substitution"
        fi
    fi
    # K0.7-P4f-B1 — Mode isolé (MUT_CMDLINE, par ex. « test=pmmtrust ») : la
    # version saine doit d'abord réussir CE mode (statut 33, 0 échec).
    if [ -z "$verdict" ] && [ -n "${MUT_CMDLINE:-}" ] && ! sane_mode_ok "$MUT_CMDLINE"; then
        verdict=INVALIDE; why="version saine en échec pour « $MUT_CMDLINE »"
    fi
    if [ -z "$verdict" ]; then
        build_and_run "$dir"
        local nf; nf="$(summary_failures "$RUN_LOG" 2>/dev/null)"
        if [ "$RUN_BUILD" -ne 0 ]; then
            verdict=INVALIDE; why="compilation en échec ($dir/build.log)"
        elif [ "$(sha256sum < "$dir/$IMAGE")" = "$SANE_SHA" ]; then
            verdict=INVALIDE; why="binaire identique à la version saine"
        elif abnormal_cpu "$RUN_QLOG"; then
            verdict=INVALIDE; why="triple fault, reset inattendu ou journal QEMU absent"
        elif [ "$RUN_STATUS" -eq 33 ]; then
            verdict="NON DÉTECTÉE"; why="les autotests passent avec le défaut"
        elif [ "$RUN_STATUS" -ne "$want" ]; then
            verdict=INVALIDE; why="statut QEMU $RUN_STATUS, attendu $want (124 = délai, 37 = panique, 0 = arrêt CPU)"
        elif [ -z "$nf" ] || [ "$nf" -eq 0 ]; then
            verdict=INVALIDE; why="bilan d'autotests absent ou sans échec"
        else
            local a missing=""
            for a in "$@"; do
                tr -d '\r' < "$RUN_LOG" | grep -qF -- "[FAIL] $a at " || missing="$missing
      assertion absente : [FAIL] $a"
            done
            if [ -n "$missing" ]; then
                verdict=INVALIDE; why="échec non ciblé :$missing"
            else
                verdict=DÉTECTÉE; why="statut $RUN_STATUS, $nf échec(s), $# assertion(s) ciblée(s) présente(s)"
            fi
        fi
    fi
    if [ "$verdict" = DÉTECTÉE ]; then
        echo "[PASS] $name : DÉTECTÉE ($why)"
        detected=$((detected + 1))
    else
        echo "[FAIL] $name : $verdict ($why)"
        tr -d '\r' < "$RUN_LOG" | grep '^\[FAIL\]' | head -5 | sed 's/^/      /'
        bad=1
    fi
    [ "${KEEP:-0}" = 1 ] || rm -rf "$dir"
}

# apply_mutation DIR FICHIER ORIG REPL -> vide si appliquée, sinon la raison.
apply_mutation() {
    local dir="$1" file="$2" orig="$3" repl="$4" n
    [ -f "$dir/$file" ] || { echo "fichier absent"; return; }
    n="$(count_literal "$dir/$file" "$orig")"
    [ "$n" -eq 1 ] || { echo "texte original trouvé $n fois (exactement 1 requis)"; return; }
    replace_literal "$dir/$file" "$orig" "$repl"
    cmp -s "$SRC/$file" "$dir/$file" && { echo "fichier inchangé après substitution"; return; }
    [ "$(count_literal "$dir/$file" "$repl")" -ge 1 ] || echo "texte muté absent après substitution"
}

# K0.7-R1c — mutant_tool NOM FICHIER 'ORIG' 'MUTÉ' 'COMMANDE' 'DIAGNOSTIC'...
# Mutation détectée par un OUTIL d'inspection des artefacts (et non par les
# autotests) : les deux images sont compilées (« make all »), puis COMMANDE est
# exécutée dans l'arbre. Exigé : COMMANDE réussit (code 0) dans l'arbre sain ;
# dans l'arbre muté, elle échoue avec le code 1 (code 2 = prérequis : INVALIDE)
# et sa sortie contient chaque DIAGNOSTIC (texte exact). L'image de production
# mutée doit différer de l'image saine.
SANE_ALL=""
mutant_tool() {
    local name="$1" file="$2" orig="$3" repl="$4" cmd="$5"; shift 5
    local dir="$WORK/m$total" verdict="" why="" out rc
    total=$((total + 1))
    echo "== mutation $name ($file, vérifiée par : $cmd)"
    if [ -z "$SANE_ALL" ]; then
        if make -C "$SANE" -j"$JOBS" all >"$SANE/build-all.log" 2>&1; then SANE_ALL=ok; else SANE_ALL=ko; fi
    fi
    if [ "$SANE_ALL" != ok ]; then
        verdict=INVALIDE; why="compilation saine (make all) en échec ($SANE/build-all.log)"
    elif ! (cd "$SANE" && eval "$cmd") >"$WORK/sane-tool.out" 2>&1; then
        verdict=INVALIDE; why="la commande échoue déjà sur l'arbre sain"
        sed 's/^/      /' "$WORK/sane-tool.out" | grep FAIL | head -3
    fi
    if [ -z "$verdict" ]; then
        cp -a "$SRC" "$dir"
        why="$(apply_mutation "$dir" "$file" "$orig" "$repl")"
        [ -z "$why" ] || verdict=INVALIDE
    fi
    if [ -z "$verdict" ]; then
        if ! make -C "$dir" -j"$JOBS" all >"$dir/build.log" 2>&1; then
            verdict=INVALIDE; why="compilation en échec ($dir/build.log)"
        elif cmp -s "$SANE/build/samos.elf" "$dir/build/samos.elf"; then
            verdict=INVALIDE; why="image de production identique à la version saine"
        else
            out="$( (cd "$dir" && eval "$cmd") 2>&1 )"; rc=$?
            if [ "$rc" -eq 0 ]; then
                verdict="NON DÉTECTÉE"; why="la commande réussit avec le défaut"
            elif [ "$rc" -ne 1 ]; then
                verdict=INVALIDE; why="code $rc (1 attendu ; 2 = prérequis)"
            else
                local a missing=""
                for a in "$@"; do
                    printf '%s\n' "$out" | grep -qF -- "$a" || missing="$missing
      diagnostic absent : $a"
                done
                if [ -n "$missing" ]; then
                    verdict=INVALIDE; why="échec non ciblé :$missing"
                else
                    verdict=DÉTECTÉE; why="code 1, $# diagnostic(s) ciblé(s) présent(s)"
                fi
            fi
        fi
    fi
    if [ "$verdict" = DÉTECTÉE ]; then
        echo "[PASS] $name : DÉTECTÉE ($why)"
        detected=$((detected + 1))
    else
        echo "[FAIL] $name : $verdict ($why)"
        printf '%s\n' "${out:-}" | grep FAIL | head -5 | sed 's/^/      /'
        bad=1
    fi
    [ "${KEEP:-0}" = 1 ] || rm -rf "$dir"
}

# shellcheck source=tests/mutation/cases.sh
source "$CASES_FILE"

# --- Bilan ---------------------------------------------------------------------------
if [ "$(repo_state | sha256sum)" != "$STATE_BEFORE" ]; then
    echo "[FAIL] l'état du dépôt a changé pendant les mutations"
    exit 2
fi
if [ "$total" -eq 0 ]; then
    echo "[FAIL] aucune mutation exécutée"
    exit 2
fi
echo "MUTATION: $total mutations, $detected detectees, $((total - detected)) non detectees ou invalides"
exit $bad
