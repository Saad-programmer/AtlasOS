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
    "SAMOS: K0.5 boot complete, interrupts enabled" \
    "irq: 8259 PIC remapped to vectors 0x20-0x2f" \
    "timer: PIT channel 0, 100 Hz (divisor 11932)" \
    "keyboard: 8042 self-test ok, PS/2 keyboard reset ok, IRQ1 enabled" \
    "cpu: GDT loaded" \
    "cpu: IDT loaded (32 exception gates, #DF task gate)" \
    "!KERNEL PANIC"

# K0.3 — Le pilote série transmet réellement : motifs lus dans le seul journal COM1.
TIMEOUT=3 run_case serial_output "" 124 \
    "serial:SAMOS: kernel entry" \
    "serial:console: debugcon + serial COM1 115200 8N1 + VGA 80x25" \
    "serial:SAMOS: K0.5 boot complete"

# Autotests noyau, toutes suites.
run_case selftest "test" 33 \
    "[TEST] suite K0.1/K0.2 boot" \
    "[TEST] suite K0.2 layout" \
    "[TEST] suite K0.3 console" \
    "[TEST] suite K0.4 cpu" \
    "[TEST] suite K0.5 irq" \
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

# ---------------------------------------------------------------------------
# K0.4 — Exceptions fatales : rapport complet puis panique (statut 37).
# ---------------------------------------------------------------------------
run_case exc_ud "test=ud" 37 \
    "serial:*** CPU exception 6: Invalid Opcode (#UD), error code 0x00000000" \
    "serial:EIP=0x001" "serial:CS=0008" "serial:CR0=" \
    "serial:KERNEL PANIC: unhandled Invalid Opcode (#UD)"

run_case exc_de "test=de" 37 \
    "serial:*** CPU exception 0: Divide Error (#DE)" \
    "serial:KERNEL PANIC: unhandled Divide Error (#DE)"

# MOV DS, 0x0F00 : index 480 au-delà de la limite de la GDT (5 entrées).
run_case exc_gp "test=gp" 37 \
    "serial:*** CPU exception 13: General Protection (#GP), error code 0x00000f00" \
    "serial:selector error: GDT index 480" \
    "serial:KERNEL PANIC: unhandled General Protection (#GP)"

# INT 0x2E (IRQ 14, inutilisée) : porte de type valide mais P = 0 => #NP,
# code = 0x2E*8 + 2 (bit IDT) = 0x172.
run_case exc_np "test=np" 37 \
    "serial:*** CPU exception 11: Segment Not Present (#NP), error code 0x00000172" \
    "serial:selector error: IDT index 46" \
    "serial:KERNEL PANIC: unhandled Segment Not Present (#NP)"

# INT 0x80 : entrée entièrement nulle (type de porte invalide). Le CPU vérifie
# le TYPE avant le bit P => #GP (et non #NP), code = 0x80*8 + 2. (Constaté en K0.4.)
run_case exc_int_invalid "test=intbad" 37 \
    "serial:*** CPU exception 13: General Protection (#GP), error code 0x00000402" \
    "serial:selector error: IDT index 128"

# Double faute : capturée par la porte de tâche, sur la pile dédiée, SANS reset.
run_case exc_df "test=df" 37 \
    "serial:*** DOUBLE FAULT (#DF) caught by task gate, running on dedicated stack" \
    "serial:interrupted task: EIP=0x001" \
    "serial:KERNEL PANIC: double fault" \
    "!CPU exception 11"

# Triple fault volontaire : le banc de test DOIT le détecter (« Triple fault »
# dans le journal QEMU). Avec -no-reboot, QEMU s'arrête (statut 0).
EXPECT_TRIPLE=1 run_case exc_triple "test=triple" 0 \
    "[TEST] fatal mode 'triple'" \
    "!KERNEL PANIC"

# ---------------------------------------------------------------------------
# K0.5 — Clavier de bout en bout : frappes réelles injectées par le moniteur
# QEMU -> contrôleur 8042 émulé -> IRQ 1 -> file circulaire -> décodeur.
# Majuscules par Shift, chiffres et ponctuation, Entrée termine la ligne.
# ---------------------------------------------------------------------------
TIMEOUT=20 run_case_keys keyboard_e2e "test=kbd" 33 \
    "shift-s a m o s spc shift-k 0 dot 5 spc shift-1 ret" \
    "serial:[TEST] kbd ready" \
    "serial:[TEST] kbd line: 'Samos K0.5 !'" \
    "serial:dropped=0"
