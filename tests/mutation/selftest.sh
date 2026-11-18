#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/mutation/selftest.sh                         (K0.7-R0)
# -----------------------------------------------------------------------------
# Vérifie que tests/mutation/run.sh NE peut PAS annoncer un faux succès.
# Chaque situation doit produire le code de sortie et le diagnostic attendus :
#   1. QEMU absent                       -> 2, « prérequis absent »
#   2. fichier de cas absent             -> 2, « fichier de cas absent »
#   3. fichier de cas vide               -> 2, « aucune mutation exécutée »
#   4. version saine en échec (16 MiB)   -> 2, « version saine en échec »
#   5. mutation sans effet observable    -> 1, « NON DÉTECTÉE »
#   6. texte original introuvable        -> 1, « trouvé 0 fois »
#   7. mutation qui casse la compilation -> 1, « compilation en échec »
#   8. mutation qui provoque une panique -> 1, « statut QEMU 37 »
#   9. assertion attendue non signalée   -> 1, « assertion absente »
#  10. interruption (SIGTERM)            -> 130, répertoire temporaire supprimé
#      (SIGINT ne peut pas servir ici : un shell non interactif lance ses
#      commandes en arrière-plan avec SIGINT ignoré, et un signal ignoré à
#      l'entrée ne peut pas être intercepté ; run.sh traite INT, TERM et HUP
#      de la même façon.)
# Contre-épreuve : un fichier contenant une mutation réelle donne 0.
# Durée : environ une minute.
# =============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="$HERE/run.sh"
T="$(mktemp -d "${TMPDIR:-/tmp}/samos-mutself.XXXXXX")" || exit 2
trap 'rm -rf "$T"' EXIT
trap 'exit 130' INT TERM HUP
fail=0; n=0

# expect NOM CODE MOTIF [VAR=VAL...] -- (exécute run.sh avec MUTATION_CASES=$T/$NOM.sh)
expect() {
    local name="$1" want="$2" pat="$3"; shift 3
    n=$((n + 1))
    env MUTATION_CASES="$T/$name.sh" "$@" "$RUN" >"$T/$name.out" 2>&1
    local got=$?
    if [ "$got" -eq "$want" ] && grep -qF -- "$pat" "$T/$name.out"; then
        echo "[PASS] $name : code $got, « $pat »"
    else
        echo "[FAIL] $name : code $got (attendu $want), motif « $pat » $(grep -qF -- "$pat" "$T/$name.out" && echo présent || echo absent)"
        sed 's/^/      /' "$T/$name.out" | tail -5
        fail=1
    fi
}

GOOD="mutant flags_filter kernel/arch/i386/paging.c 'if (counted && (flags & ~PTE_WRITE) != 0) {' 'if (0) {' 35 'e3_attempt(0xD0060000u, f, PTE_USER, SAMOS_EINVAL, \"alloue\")'"

echo "$GOOD" > "$T/no_qemu.sh"
expect no_qemu 2 "prérequis absent" QEMU=/nonexistent/qemu
expect missing_cases 2 "fichier de cas absent"
: > "$T/empty.sh"
expect empty 2 "aucune mutation exécutée"
echo "$GOOD" > "$T/sane_fails.sh"
expect sane_fails 2 "version saine en échec" QEMU_MEM=16M

cat > "$T/undetected.sh" <<'EOF'
mutant log_text kernel/mm/mm.c '"pmm: descriptor table consistent\n"' '"pmm: descriptor table CONSISTENT\n"' 35 'x'
EOF
expect undetected 1 "NON DÉTECTÉE"
cat > "$T/not_found.sh" <<'EOF'
mutant absent kernel/mm/pmm_core.c 'texte qui n existe pas' 'x' 35 'x'
EOF
expect not_found 1 "trouvé 0 fois"
cat > "$T/no_build.sh" <<'EOF'
mutant syntax kernel/mm/pmm_core.c 'if (frame_desc_refs(p->meta[frame]) != 0) {' 'if (frame_desc_refs( {' 35 'x'
EOF
expect no_build 1 "compilation en échec"
cat > "$T/panic.sh" <<'EOF'
mutant panic kernel/arch/i386/paging.c 'KASSERT(e == SAMOS_OK);' 'KASSERT(e != SAMOS_OK);' 35 'x'
EOF
expect panic 1 "statut QEMU 37"
cat > "$T/wrong_assert.sh" <<'EOF'
mutant wrong kernel/arch/i386/paging.c 'if (counted && (flags & ~PTE_WRITE) != 0) {' 'if (0) {' 35 'assertion qui n existe pas'
EOF
expect wrong_assert 1 "assertion absente"
echo "$GOOD" > "$T/good.sh"
expect good 0 "1 detectees"

# 10. Interruption : le répertoire temporaire de run.sh doit disparaître.
n=$((n + 1))
echo "$GOOD" > "$T/int.sh"
mkdir -p "$T/tmp"
MUTATION_CASES="$T/int.sh" TMPDIR="$T/tmp" setsid "$RUN" >"$T/int.out" 2>&1 &
pid=$!
for _ in $(seq 1 100); do ls "$T/tmp"/samos-mutation.* >/dev/null 2>&1 && break; sleep 0.1; done
sleep 2
kill -TERM -- "-$pid" 2>/dev/null
wait "$pid"; got=$?
left="$(ls -A "$T/tmp")"
if [ "$got" -eq 130 ] && [ -z "$left" ]; then
    echo "[PASS] interrupt : code 130, répertoire temporaire supprimé"
else
    echo "[FAIL] interrupt : code $got (attendu 130), restes : ${left:-aucun}"
    fail=1
fi

echo "MUTATION-SELFTEST: $n situations, $([ $fail -eq 0 ] && echo 'toutes conformes' || echo 'ECHECS')"
exit $fail
