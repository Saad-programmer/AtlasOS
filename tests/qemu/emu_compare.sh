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
# Résumé normalisé, comparé ligne à ligne (aucune tolérance) :
#   - chaque verdict « [PASS] / [FAIL] / [SKIP] nom » des deux bancs, SANS la
#     parenthèse finale de mesures ;
#   - la ligne de bilan « QEMU: N cas, R reussis, E echecs » ;
#   - pour chaque autotest, « skip CAS N » tiré de « [TEST] skipped=N ».
# Toute différence est une DIVERGENCE : sortie 1 avec le diff. Un résumé
# absent ou vide est aussi un échec (sortie 2) : une exécution manquante ne
# peut pas passer pour un accord.
#
# Parenthèse finale des verdicts (« (avant 0x..., refus 0x...) ») : ce sont
# des MESURES, pas des verdicts. Mesuré en K1.1-M64-0 : l'empreinte de la pile
# active (n1stack) change d'une exécution à l'autre sous le MÊME émulateur, et
# celle de la mémoire basse (n1low, contenu écrit par le BIOS) diffère entre
# les deux machines ; leur règle (« différente d'une page remise à zéro ») est
# satisfaite dans les deux cas. Ces écarts sont LISTÉS ([INFO] mesure), jamais
# cachés ; un statut ou un nom différent reste une divergence.
#
# Ce qui n'est PAS comparé, et pourquoi : le texte brut des journaux. La
# machine x86_64 publie une région E820 réservée supplémentaire
# (0xfd00000000-0xffffffffff, au-dessus de 4 GiB, ignorée par le noyau 32 bits)
# et son CPU (qemu64) diffère de qemu32 ; les journaux diffèrent donc
# légitimement. La différence est AFFICHÉE ([INFO]) pour rester visible.
# =============================================================================
set -u
A="${1:?usage: emu_compare.sh DIR_A DIR_B}"
B="${2:?usage: emu_compare.sh DIR_A DIR_B}"

summary() {   # summary DIR : résumé normalisé trié sur la sortie standard
    local d="$1" f
    for f in "$d/qemu.out" "$d/gdb.out"; do
        [ -s "$f" ] || { echo "MISSING $(basename "$f")"; continue; }
        grep -E '^\[(PASS|FAIL|SKIP)\] ' "$f" | sed -E 's/ \([^()]*\)$//'
        grep -E '^QEMU: [0-9]+ cas' "$f"
    done
    for f in "$d"/*.serial.log; do
        [ -f "$f" ] || continue
        grep -hE '^\[TEST\] skipped=[0-9]+' "$f" | sed "s/^\[TEST\] skipped=/skip $(basename "$f" .serial.log) /"
    done
}

status=0
for d in "$A" "$B"; do
    if [ ! -d "$d" ] || ! grep -qE '^QEMU: [0-9]+ cas' "$d/qemu.out" 2>/dev/null || ! grep -q '^\[PASS\] gdb\|^\[FAIL\] gdb' "$d/gdb.out" 2>/dev/null; then
        echo "[FAIL] comparaison : resultats absents ou incomplets dans $d"
        status=2
    fi
done
[ "$status" -eq 0 ] || exit "$status"

SA="$(summary "$A" | LC_ALL=C sort)"
SB="$(summary "$B" | LC_ALL=C sort)"
echo "[INFO] $A : $(printf '%s\n' "$SA" | wc -l) lignes de resume ; $B : $(printf '%s\n' "$SB" | wc -l)"
for d in "$A" "$B"; do
    echo "[INFO] $d : $(grep -m1 -hoE 'mm: [0-9]+ regions, [0-9]+ KiB available' "$d/gdb_vga.serial.log" 2>/dev/null || echo 'carte memoire non relevee')"
done
# Mesures différentes sous un même verdict : affichées pour rester visibles.
details() { grep -hE '^\[(PASS|FAIL|SKIP)\] .* \([^()]*\)$' "$1/qemu.out" "$1/gdb.out" 2>/dev/null | LC_ALL=C sort; }
diff <(details "$A") <(details "$B") | sed -n 's/^< /[INFO] mesure differente, A : /p; s/^> /[INFO] mesure differente, B : /p'
if [ "$SA" = "$SB" ]; then
    echo "[PASS] comparaison : verdicts, bilans et SKIP identiques sous les deux emulateurs"
    exit 0
fi
echo "[FAIL] comparaison : DIVERGENCE entre $A et $B"
diff <(printf '%s\n' "$SA") <(printf '%s\n' "$SB") | sed 's/^/        /'
exit 1
