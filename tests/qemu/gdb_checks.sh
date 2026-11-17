#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/qemu/gdb_checks.sh
# -----------------------------------------------------------------------------
# Vérifications au niveau instruction, impossibles depuis le noyau lui-même :
#
#  1. abi_at_kmain : arrêt EXACT sur la première instruction de kmain
#     (break *kmain) et contrôle de l'état exigé par l'ABI i386 System V :
#       ESP % 16 == 12          (donc ESP+4 aligné sur 16 au moment du call)
#       [ESP+4] == 0x2BADB002   (1er argument : magic)
#       [ESP+8] == EBX initial  (2e argument : adresse multiboot_info, != 0)
#       EFLAGS.DF == 0, EFLAGS.IF == 0
#       boot_stack_bottom < ESP < boot_stack_top
#
#  2. bad_magic : test NÉGATIF. On remplace le 1er argument par 0xDEADBEEF
#     dans GDB avant que kmain ne le lise, et on vérifie que le noyau refuse
#     de démarrer avec un message explicite (aucun chargeur réel ne permet
#     de provoquer ce cas).
# =============================================================================
set -u
KERNEL="${1:?usage: gdb_checks.sh build/samos.elf}"
OUT="$(dirname "$KERNEL")/tests"; mkdir -p "$OUT"
PORT="${GDB_PORT:-1234}"
fail=0

start_qemu() {   # $1 = journal debugcon
    qemu-system-i386 -m 128M -display none -no-reboot -kernel "$KERNEL" \
        -debugcon "file:$1" -gdb "tcp::$PORT" -S </dev/null >/dev/null 2>&1 &
    QPID=$!
    sleep 0.5
}
stop_qemu() { kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; }

# ---- 1. ABI à l'entrée de kmain ----------------------------------------------
start_qemu "$OUT/gdb_abi.debugcon.log"
gdb -q -batch -nx "$KERNEL" \
    -ex "target remote :$PORT" \
    -ex 'break *kmain' -ex 'continue' \
    -ex 'printf "ESPMOD16=%d\n", (unsigned int)$esp % 16' \
    -ex 'printf "ARG0=0x%x\n", *(unsigned int *)($esp + 4)' \
    -ex 'printf "ARG1=0x%x\n", *(unsigned int *)($esp + 8)' \
    -ex 'printf "DF=%d\n", ($eflags >> 10) & 1' \
    -ex 'printf "IF=%d\n", ($eflags >> 9) & 1' \
    -ex 'printf "INSTACK=%d\n", ((unsigned int)$esp > (unsigned int)&boot_stack_bottom) && ((unsigned int)$esp < (unsigned int)&boot_stack_top)' \
    -ex 'kill' > "$OUT/gdb_abi.log" 2>&1
stop_qemu
check() {   # check NOM MOTIF
    if grep -q "^$2\$" "$OUT/gdb_abi.log"; then echo "[PASS] gdb: $1"; else echo "[FAIL] gdb: $1 (voir $OUT/gdb_abi.log)"; fail=1; fi
}
check "ESP % 16 == 12 a l'entree de kmain" "ESPMOD16=12"
check "1er argument = 0x2BADB002" "ARG0=0x2badb002"
if grep -q '^ARG1=0x[1-9a-f]' "$OUT/gdb_abi.log"; then echo "[PASS] gdb: 2e argument (mbi) non nul"; else echo "[FAIL] gdb: 2e argument nul"; fail=1; fi
check "EFLAGS.DF == 0" "DF=0"
check "EFLAGS.IF == 0" "IF=0"
check "ESP dans la pile de demarrage" "INSTACK=1"

# ---- 2. Magic invalide (test négatif) ----------------------------------------
# Après modification de l'argument, `detach` laisse le CPU reprendre
# l'exécution ; le noyau doit afficher l'erreur puis s'arrêter par HLT.
BLOG="$OUT/gdb_bad_magic.debugcon.log"
start_qemu "$BLOG"
gdb -q -batch -nx "$KERNEL" \
    -ex "target remote :$PORT" \
    -ex 'break *kmain' -ex 'continue' \
    -ex 'set {unsigned int}($esp + 4) = 0xdeadbeef' \
    -ex 'delete' -ex 'detach' > "$OUT/gdb_bad_magic.log" 2>&1
sleep 1
stop_qemu
if grep -q 'SAMOS: FATAL: invalid multiboot magic (0xdeadbeef)' "$BLOG" &&
   grep -q 'SAMOS: system halted' "$BLOG" &&
   ! grep -q 'boot complete' "$BLOG"; then
    echo "[PASS] gdb: magic invalide -> refus explicite et arret"
else
    echo "[FAIL] gdb: magic invalide non detecte (voir $BLOG)"; fail=1
fi

exit $fail
