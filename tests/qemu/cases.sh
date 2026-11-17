# shellcheck shell=bash
# =============================================================================
# SAMOS / AtlasOS — tests/qemu/cases.sh
# Liste des cas de test QEMU, chargée par run.sh. Un cas par ligne :
#   run_case NOM "LIGNE_DE_COMMANDE" STATUT_ATTENDU "motif 1" "motif 2" ...
# =============================================================================

# K0.1 — Démarrage normal : le noyau valide Multiboot puis s'arrête par HLT.
# QEMU ne se termine pas seul : on attend le timeout (124) et on vérifie
# l'absence de triple fault.
TIMEOUT=3 run_case k01_boot_halt "" 124 \
    "SAMOS: kernel entry" \
    "SAMOS: multiboot magic ok" \
    "SAMOS: kernel image 0x00100000-" \
    "---G stack-guard" \
    "bytes, guard intact" \
    "SAMOS: K0.2 boot complete, halting"

# K0.1/K0.2 — Autotests noyau.
run_case k01_selftest "test" 33 \
    "[TEST] suite K0.1/K0.2 boot" \
    "[TEST] suite K0.2 layout" \
    "[TEST] ALL TESTS PASSED"
