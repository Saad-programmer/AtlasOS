#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/repro_selftest.sh                       (K0.7-R0)
# -----------------------------------------------------------------------------
# Vérifie que tests/tools/repro_check.sh ne peut pas annoncer un faux succès :
#   1. prérequis absent (nasm retiré du PATH)      -> 2, « prérequis absent »
#   2. compilation en échec                         -> 2, « compilation en échec »
#   3. un octet de .rodata modifié dans UN arbre    -> 1, « DIFFÉRENT hors débogage »
#   4. mode strict (REPRO_STRICT=1)                 -> 1 tant que NASM incorpore
#      le chemin absolu (limite connue)
# Contre-épreuve : l'exécution normale donne 0.
# Les cas 2 et 3 utilisent un « make » instrumenté (REPRO_MAKE) : make réel,
# puis échec forcé ou altération d'un octet de l'image dans le second arbre.
# =============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="$HERE/repro_check.sh"
T="$(mktemp -d "${TMPDIR:-/tmp}/samos-reproself.XXXXXX")" || exit 2
trap 'rm -rf "$T"' EXIT
trap 'exit 130' INT TERM HUP
fail=0; n=0

expect() {
    local name="$1" want="$2" pat="$3"; shift 3
    n=$((n + 1))
    env "$@" "$RUN" >"$T/$name.out" 2>&1
    local got=$?
    if [ "$got" -eq "$want" ] && grep -qF -- "$pat" "$T/$name.out"; then
        echo "[PASS] $name : code $got, « $pat »"
    else
        echo "[FAIL] $name : code $got (attendu $want), motif « $pat »"
        tail -5 "$T/$name.out" | sed 's/^/      /'
        fail=1
    fi
}

# 1. PATH réduit : tous les outils nécessaires sauf nasm.
mkdir -p "$T/bin"
for t in bash env dirname basename sed cut stat rm mkdir mktemp wc cat head tail tr nproc \
         git tar make clang ld objcopy readelf cmp sha256sum grep; do
    p="$(command -v "$t")" && ln -s "$p" "$T/bin/$t"
done
expect no_nasm 2 "prérequis absent : nasm" PATH="$T/bin"

# 2 et 3. make instrumenté.
cat > "$T/make-fail" <<'SH'
#!/usr/bin/env bash
case "$*" in *second-independent-tree*) echo "erreur simulée"; exit 1 ;; esac
exec make "$@"
SH
cat > "$T/make-tamper" <<'SH'
#!/usr/bin/env bash
make "$@" || exit $?
case "$*" in
  *second-independent-tree*)
    dir="$2"; f="$dir/build/samos.elf"
    hex=$(readelf -SW "$f" | sed -n 's/^ *\[ *[0-9]*\] \.rodata  *[A-Z]*  *[0-9a-f]*  *\([0-9a-f]*\) .*/\1/p')
    [ -n "$hex" ] || { echo "offset de .rodata introuvable"; exit 1; }
    off=$((16#$hex))
    printf '\x5a' | dd of="$f" bs=1 seek="$off" conv=notrunc status=none ;;
esac
SH
chmod +x "$T/make-fail" "$T/make-tamper"
expect build_fails 2 "compilation en échec" REPRO_MAKE="$T/make-fail"
expect tampered 1 "DIFFÉRENT hors débogage" REPRO_MAKE="$T/make-tamper"
expect strict 1 "refusé en mode strict" REPRO_STRICT=1
expect normal 0 "REPRO: 2 images comparees, reproductibles"

echo "REPRO-SELFTEST: $n situations, $([ $fail -eq 0 ] && echo 'toutes conformes' || echo 'ECHECS')"
exit $fail
