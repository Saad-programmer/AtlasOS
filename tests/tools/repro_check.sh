#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/repro_check.sh          (make test-repro, K0.7-R0)
# -----------------------------------------------------------------------------
# Reproductibilité : deux compilations PROPRES et INDÉPENDANTES, dans deux
# répertoires de longueurs différentes (pour révéler tout chemin incorporé),
# puis comparaison OCTET PAR OCTET des images produites :
#     build/samos.elf (production)   build/samos-test.elf (autotests)
#
# Trois niveaux, par image :
#   1. ELF complet identique (SHA-256)                  -> IDENTIQUE
#   2. sinon, ELF sans débogage (objcopy --strip-debug) identique ET toutes les
#      sections différentes sont des sections .debug_*   -> DÉBOGAGE SEULEMENT
#      (code, données, en-têtes, symboles identiques : ce qui est chargé et
#      exécuté ne dépend pas du répertoire de compilation)
#   3. sinon                                             -> DIFFÉRENT (échec)
# Chaque section différente est nommée, et les chemins absolus des
# répertoires de compilation retrouvés dans les images sont signalés.
#
# Limite connue (K0.7) : NASM 2.16 écrit le répertoire absolu de compilation
# dans DW_AT_comp_dir (.debug_info) et n'a pas d'équivalent de
# -ffile-prefix-map (utilisé pour Clang). Le niveau 2 est donc le résultat
# attendu aujourd'hui. REPRO_STRICT=1 exige le niveau 1 (échoue actuellement).
#
# Le dépôt n'est pas modifié (vérifié) ; les copies temporaires sont
# supprimées à la sortie, y compris sur interruption.
# Codes de sortie : 0 reproductible (niveau 1, ou 2 hors mode strict) ;
# 1 différence ; 2 prérequis manquant ou compilation en échec ; 130 interruption.
# =============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
STRICT="${REPRO_STRICT:-0}"
ARTIFACTS="build/samos.elf build/samos-test.elf"
MAKE_CMD="${REPRO_MAKE:-make}"   # auto-test uniquement : make instrumenté

die() { echo "[FAIL] $*"; exit 2; }

for tool in git tar make clang nasm ld objcopy readelf cmp sha256sum mktemp grep; do
    command -v "$tool" >/dev/null 2>&1 || die "prérequis absent : $tool"
done
git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1 || die "$ROOT n'est pas un dépôt Git"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/samos-repro.XXXXXX")" || die "mktemp a échoué"
trap 'rm -rf "$WORK"' EXIT
trap 'echo "[FAIL] interrompu"; exit 130' INT TERM HUP

repo_state() {
    git -C "$ROOT" rev-parse HEAD
    git -C "$ROOT" symbolic-ref -q HEAD || echo detached
    git -C "$ROOT" status --porcelain=v1 --untracked-files=all
}
STATE_BEFORE="$(repo_state | sha256sum)"

# Deux arbres indépendants, chemins de longueurs différentes, sans build/.
A="$WORK/a"
B="$WORK/second-independent-tree"
for d in "$A" "$B"; do
    mkdir -p "$d"
    (cd "$ROOT" && git ls-files -z | tar --null -T - -cf -) | tar -xf - -C "$d" \
        || die "copie des sources impossible"
    [ ! -e "$d/build" ] || die "la copie contient build/"
    echo "== compilation propre : $d"
    # shellcheck disable=SC2086
    "$MAKE_CMD" -C "$d" -j"$JOBS" $ARTIFACTS >"$d.build.log" 2>&1 \
        || { tail -5 "$d.build.log" | sed 's/^/      /'; die "compilation en échec dans $d ($d.build.log)"; }
    for f in $ARTIFACTS; do
        [ -s "$d/$f" ] || die "artefact absent après compilation : $d/$f"
    done
done

# Sections d'un ELF (noms), dans l'ordre de la table des sections.
sections() { readelf -SW "$1" | sed -n 's/^ *\[ *[0-9]*\] \([^ ]\{1,\}\) .*/\1/p'; }
# Contenu d'une section (vide pour NOBITS ou sections non extractibles).
dump() { objcopy --dump-section "$2=$3" "$1" "$WORK/null.o" >/dev/null 2>&1 || : > "$3"; }

fail=0; total=0
for f in $ARTIFACTS; do
    total=$((total + 1))
    fa="$A/$f"; fb="$B/$f"
    sa="$(sha256sum < "$fa" | cut -c1-16)"; sb="$(sha256sum < "$fb" | cut -c1-16)"
    if cmp -s "$fa" "$fb"; then
        echo "[PASS] $f : IDENTIQUE octet pour octet (sha256 $sa...)"
        continue
    fi
    # Sections différentes (contenu extractible) ; NOBITS et tables de symboles
    # sont couvertes par la comparaison de l'ELF sans débogage ci-dessous.
    diffs=""
    if [ "$(sections "$fa")" != "$(sections "$fb")" ]; then
        diffs=" <liste des sections>"
    fi
    for s in $(sections "$fa"); do
        dump "$fa" "$s" "$WORK/sa.bin"; dump "$fb" "$s" "$WORK/sb.bin"
        cmp -s "$WORK/sa.bin" "$WORK/sb.bin" || diffs="$diffs $s"
    done
    objcopy --strip-debug "$fa" "$WORK/a.nodebug" && objcopy --strip-debug "$fb" "$WORK/b.nodebug" \
        || die "objcopy --strip-debug a échoué sur $f"
    nondebug=""
    for s in $diffs; do
        case "$s" in .debug_*) ;; *) nondebug="$nondebug $s" ;; esac
    done
    paths=""
    for d in "$A" "$B"; do
        n="$(grep -a -o -F -- "$d" "$d/$f" | wc -l)"
        [ "${n:-0}" -eq 0 ] || paths="$paths $(basename "$d"):$n"
    done
    echo "  $f : ELF complets différents ($(stat -c %s "$fa") / $(stat -c %s "$fb") octets, sha256 $sa... / $sb...)"
    echo "    sections différentes :${diffs:- aucune section extractible}"
    echo "    occurrences du chemin de compilation dans l'image :${paths:- aucune}"
    if cmp -s "$WORK/a.nodebug" "$WORK/b.nodebug" && [ -z "$nondebug" ]; then
        if [ "$STRICT" = 1 ]; then
            echo "[FAIL] $f : DÉBOGAGE SEULEMENT, refusé en mode strict (REPRO_STRICT=1)"
            fail=1
        else
            echo "[PASS] $f : DÉBOGAGE SEULEMENT — image sans débogage identique octet pour octet ; seules diffèrent :$diffs (chemins absolus, limite connue de NASM)"
        fi
    else
        echo "[FAIL] $f : DIFFÉRENT hors débogage :${nondebug:- (ELF sans débogage différents)}"
        cmp "$WORK/a.nodebug" "$WORK/b.nodebug" 2>&1 | head -1 | sed 's/^/      /'
        fail=1
    fi
done

if [ "$(repo_state | sha256sum)" != "$STATE_BEFORE" ]; then
    echo "[FAIL] l'état du dépôt a changé pendant la vérification"
    exit 2
fi
[ "$total" -gt 0 ] || die "aucun artefact comparé"
echo "REPRO: $total images comparees, $([ $fail -eq 0 ] && echo reproductibles || echo 'NON reproductibles')$([ "$STRICT" = 1 ] && echo ' (mode strict)')"
exit $fail
