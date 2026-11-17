# shellcheck shell=bash
# =============================================================================
# SAMOS / AtlasOS — tests/qemu/cases.sh
# Liste des cas de test QEMU, chargée par run.sh. Un cas par appel :
#   run_case NOM "LIGNE_DE_COMMANDE" STATUT_ATTENDU "motif 1" "motif 2" ...
# Motifs : voir l'en-tête de run.sh ("texte", "serial:", "re:", "!").
# IMG=prod : image de production ; par défaut : image de test.
# =============================================================================

# Démarrage normal de l'image de PRODUCTION : le noyau reste ensuite inactif,
# QEMU ne se termine pas seul : timeout (124) attendu, sans triple fault.
IMG=prod TIMEOUT=3 run_case boot_halt "" 124 \
    "SAMOS: kernel entry" \
    "SAMOS: multiboot magic ok" \
    "SAMOS: kernel image 0x00100000-" \
    "---G stack-guard" \
    "SAMOS: K0.6 boot complete, paging on, interrupts enabled" \
    "paging: enabled (CR0.PG|WP), identity map 0x00001000-0x07fe0000" \
    "guard page unmapped (MMU-enforced)" \
    "irq: 8259 PIC remapped to vectors 0x20-0x2f" \
    "timer: PIT channel 0, 100 Hz (divisor 11932)" \
    "keyboard: 8042 self-test ok, PS/2 keyboard reset ok, IRQ1 enabled" \
    "cpu: GDT loaded" \
    "cpu: IDT loaded (32 exception gates, #DF task gate)" \
    "!KERNEL PANIC"

# K0.3 — Le pilote série transmet réellement : motifs lus dans le seul journal COM1.
# K0.7 — Image de production : aucun octet sur le port debugcon 0xE9.
IMG=prod TIMEOUT=3 run_case serial_output "" 124 \
    "serial:SAMOS: kernel entry" \
    "serial:console: serial COM1 115200 8N1 VGA 80x25 (production image)" \
    "!console: debugcon"
[ -s "$OUT/serial_output.debugcon.log" ] && { echo "[FAIL] production: octets écrits sur debugcon"; fail=1; }

# K0.7 — L'image de production REFUSE les modes de test, y compris destructifs :
# elle démarre normalement, sans panique ni sortie QEMU.
IMG=prod TIMEOUT=3 run_case prod_refuses_test "test=wtext" 124 \
    "serial:test mode requested but not available in the production image" \
    "serial:SAMOS: K0.6 boot complete" \
    "!KERNEL PANIC" "![TEST]"
    "serial:SAMOS: K0.6 boot complete"

# Autotests noyau, toutes suites.
run_case selftest "test" 33 \
    "[TEST] suite K0.1/K0.2 boot" \
    "[TEST] suite K0.2 layout" \
    "[TEST] suite K0.3 console" \
    "[TEST] suite K0.4 cpu" \
    "[TEST] suite K0.5 irq" \
    "[TEST] suite K0.6 mm" \
    "re:suite K0.1/K0.2 boot done: [1-9][0-9]* checks, 0 failures" \
    "re:suite K0.2 layout done: [1-9][0-9]* checks, 0 failures" \
    "re:suite K0.3 console done: [1-9][0-9]* checks, 0 failures" \
    "re:suite K0.4 cpu done: [1-9][0-9]* checks, 0 failures" \
    "re:suite K0.5 irq done: [1-9][0-9]* checks, 0 failures" \
    "re:suite K0.6 mm done: [1-9][0-9]* checks, 0 failures" \
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

# ---------------------------------------------------------------------------
# K0.6 — Protections mémoire : chaque violation est diagnostiquée précisément.
# ---------------------------------------------------------------------------
run_case pf_null "test=null" 37 \
    "serial:*** CPU exception 14: Page Fault (#PF), error code 0x00000000" \
    "serial:page not present, read access, supervisor mode, address CR2=0x00000000" \
    "serial:diagnosis: NULL pointer dereference"

run_case pf_write_text "test=wtext" 37 \
    "serial:error code 0x00000003" \
    "serial:protection violation, write access" \
    "serial:diagnosis: write to read-only kernel code (.text)"

run_case pf_write_rodata "test=wrodata" 37 \
    "serial:diagnosis: write to read-only kernel data (.rodata)"

run_case pf_unmapped "test=unmapped" 37 \
    "serial:address CR2=0xe0000000" \
    "serial:diagnosis: access to an unmapped address (outside identity-mapped RAM)"

# Débordement de pile : page de garde (K0.2) + pagination (K0.6) => #PF non
# livrable => #DF attrapée par la porte de tâche (K0.4), SANS triple fault.
run_case stack_overflow "test=stack" 37 \
    "serial:*** DOUBLE FAULT (#DF) caught by task gate, running on dedicated stack" \
    "serial:kernel stack overflow (guard page below the boot stack)" \
    "serial:KERNEL PANIC: double fault"

# Tailles de RAM : petite, moyenne, au-delà de la limite d'identité (1 GiB),
# et 4 GiB (région au-dessus de 4 GiB dans la carte, ignorée).
QEMU_MEM=16M TIMEOUT=4 run_case mem_16M "" 124 \
    "paging: enabled" "SAMOS: K0.6 boot complete" "!KERNEL PANIC" "!mm: RAM above"
QEMU_MEM=512M TIMEOUT=4 run_case mem_512M "" 124 \
    "128 page tables" "SAMOS: K0.6 boot complete" "!KERNEL PANIC"
QEMU_MEM=1500M TIMEOUT=6 run_case mem_1500M "" 124 \
    "pmm: 262144 frames managed" "mm: RAM above 1024 MiB is not managed in K0" \
    "SAMOS: K0.6 boot complete" "!KERNEL PANIC"
QEMU_MEM=4G TIMEOUT=6 run_case mem_4G "" 124 \
    "mm: 0x100000000-0x13fffffff available" "pmm: 262144 frames managed" \
    "SAMOS: K0.6 boot complete" "!KERNEL PANIC"

# Régression : les autotests passent aussi avec 512 MiB.
QEMU_MEM=512M run_case selftest_512M "test" 33 "[TEST] ALL TESTS PASSED" "![FAIL]"
