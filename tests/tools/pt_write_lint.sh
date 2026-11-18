#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/pt_write_lint.sh          (K0.7-P4e-2)
# -----------------------------------------------------------------------------
# Contrôle STATIQUE (sources) : aucune écriture logicielle directe d'une
# entrée de répertoire ou de table de pages hors du passage pt_gate_write.
#
# Complète la garantie DYNAMIQUE : depuis P4e-2, une écriture directe sur une
# table publiée faute (#PF code 3, tables en lecture seule). Ce contrôle
# signale le défaut dès la compilation, y compris sur un chemin que les tests
# n'exécuteraient pas. C'est une analyse par motifs (heuristique, documentée) :
#   1. dans le code noyau hors tests, aucune affectation indexée sur un nom de
#      table (g_pd, pt, pd, pde, table, alias, dir) ni sur un pointeur converti
#      « (...)[i] = v » ;
#   2. dans paging.c, exactement deux stockages « *entry = value; », tous deux
#      dans pt_gate_write (démarrage PG = 0, et la fenêtre WP) ;
#   3. aucun autre fichier noyau n'utilise paging_directory() (adresse du
#      répertoire : lecture par les tests seulement).
# Exceptions documentées (design P4e-2 §2) : la remise à zéro d'une table
# NEUVE par le PMM (memset, avant publication et avant scellement).
# Sortie : 0 si conforme, 1 sinon (diagnostics « pt_write_lint: ... »).
# =============================================================================
set -u
cd "$(dirname "$0")/../.." || exit 2
fail=0
bad() { echo "[FAIL] pt_write_lint: $*"; fail=1; }

srcs=$(find kernel -name '*.c' -not -path 'kernel/tests/*' | sort)

# 1. Affectations indexées suspectes.
hits=$(grep -nE '\b(g_pd|pt|pd|pde|table|alias|dir)\s*\[[^]]*\]\s*(=|\|=|&=|\^=)[^=]|\)\s*\[[^]]*\]\s*(=|\|=|&=|\^=)[^=]' $srcs)
if [ -n "$hits" ]; then
    while IFS= read -r h; do bad "ecriture directe d'entree : $h"; done <<<"$hits"
fi

# 2. Stockages du passage : exactement deux, dans pt_gate_write.
stores=$(awk '
    /^[a-zA-Z].*\(/ && !/;/ { fn = $0 }
    /\*entry = value;/ { n++; if (fn !~ /pt_gate_write\(/) print "hors-passage:" FNR }
    END { print "total:" n + 0 }' kernel/arch/i386/paging.c)
grep -q '^hors-passage:' <<<"$stores" && bad "stockage *entry hors de pt_gate_write ($(grep hors-passage <<<"$stores" | tr '\n' ' '))"
grep -qx 'total:2' <<<"$stores" || bad "nombre de stockages *entry = value dans paging.c : $(grep total <<<"$stores") (attendu 2)"

# 3. Adresse du répertoire utilisée hors de paging.c et des tests.
users=$(grep -lE 'paging_directory\(\)' $srcs | grep -v '^kernel/arch/i386/paging.c$')
[ -n "$users" ] && bad "paging_directory() utilise hors de paging.c : $users"

if [ "$fail" -eq 0 ]; then
    echo "[PASS] pt_write_lint: aucune ecriture directe de PDE/PTE hors de pt_gate_write"
fi
exit $fail
