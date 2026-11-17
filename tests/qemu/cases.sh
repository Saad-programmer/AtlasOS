# shellcheck shell=bash
# =============================================================================
# SAMOS / AtlasOS — tests/qemu/cases.sh
# Liste des cas de test QEMU, chargée par run.sh. Un cas par appel :
#   run_case NOM "LIGNE_DE_COMMANDE" STATUT_ATTENDU "motif 1" "motif 2" ...
# Motifs : "texte" (debugcon ou série), "serial:texte" (série uniquement),
#          "!texte" (ne doit apparaître nulle part).
# =============================================================================

# Démarrage normal : le noyau valide Multiboot puis s'arrête par HLT.
# QEMU ne se termine pas seul : on attend le timeout (124) et on vérifie
# l'absence de triple fault.
TIMEOUT=3 run_case boot_halt "" 124 \
    "SAMOS: kernel entry" \
    "SAMOS: multiboot magic ok" \
    "SAMOS: kernel image 0x00100000-" \
    "---G stack-guard" \
    "bytes, guard intact" \
    "SAMOS: K0.3 boot complete, halting" \
    "!KERNEL PANIC"

# K0.3 — Le pilote série transmet réellement : motifs lus dans le seul journal COM1.
TIMEOUT=3 run_case serial_output "" 124 \
    "serial:SAMOS: kernel entry" \
    "serial:console: debugcon + serial COM1 115200 8N1 + VGA 80x25" \
    "serial:SAMOS: K0.3 boot complete, halting"

# Autotests noyau, toutes suites.
run_case selftest "test" 33 \
    "[TEST] suite K0.1/K0.2 boot" \
    "[TEST] suite K0.2 layout" \
    "[TEST] suite K0.3 console" \
    "[TEST] ALL TESTS PASSED" \
    "![FAIL]"

# K0.3 — Chemin de panique : message, emplacement, trace de pile, statut 37.
run_case panic_path "test=panic" 37 \
    "serial:*** KERNEL PANIC: deliberate test panic (42)" \
    "serial:*** at kernel/tests/ktest.c:" \
    "serial:*** stack trace (" \
    "serial:***   #1 0x001" \
    "serial:*** system halted"

# Mode de test inconnu : refus explicite (statut d'échec), pas de panique.
run_case unknown_mode "test=nope" 35 \
    "[TEST] unknown test mode 'nope'"
