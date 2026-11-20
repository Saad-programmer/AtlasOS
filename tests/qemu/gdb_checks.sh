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
#
#  3. vga_memory (K0.3) : après un démarrage normal, GDB lit la mémoire vidéo
#     0xB8000 DEPUIS L'EXTÉRIEUR du noyau et on décode les octets de
#     caractères (un octet sur deux) : le texte attendu doit être à l'écran.
#
# K1.1-M64-0 — choix de l'émulateur (même image i386, mêmes critères) :
#   QEMU=qemu-system-i386     (défaut) GDB en architecture i386, déduite de l'ELF.
#   QEMU=qemu-system-x86_64   GDB forcé en « i386:x86-64 » AVANT la connexion,
#                             puis symboles chargés par « symbol-file » (raison
#                             et limites mesurées : tests/qemu/emu.sh).
#   Les expressions restent identiques ($esp, $eflags, mémoire brute, adresses
#   de symboles) : elles ont été comparées valeur par valeur sous les deux
#   émulateurs. Aucune n'utilise $eip, le désassemblage ni les arguments C
#   symboliques, faux sous qemu-system-x86_64.
#   Variables : GDB (défaut gdb), GDB_PORT (1234), TEST_OUT (build/tests),
#   GDB_ARCH (test du banc uniquement : impose une architecture, même fausse,
#   pour vérifier qu'une erreur GDB produit bien un [FAIL]).
#
# Une erreur GDB n'est jamais un succès : chaque test exige (a) un statut de
# sortie nul de GDB, (b) l'absence de diagnostic d'échec connu dans son
# journal et (c) les valeurs attendues. Sortie : 0 si tout réussit, 1 si un
# test échoue, 2 si un prérequis manque (émulateur, GDB, image).
# =============================================================================
set -u
KERNEL="${1:?usage: gdb_checks.sh build/samos.elf}"
OUT="${TEST_OUT:-$(dirname "$KERNEL")/tests}"; mkdir -p "$OUT"
PORT="${GDB_PORT:-1234}"
QEMU="${QEMU:-qemu-system-i386}"
GDB="${GDB:-gdb}"
fail=0
npass=0 nfail=0  # vérifications (bilan « GDB: » lu par tests/qemu/emu_compare.sh)
# pass_ / fail_ : seul point d'émission des verdicts de vérification. Les
# lignes « [FAIL] gdb: statut / erreur GDB » de gdb_run sont des diagnostics
# en plus ; elles mettent fail=1 et sont refusées par le comparateur.
pass_() { echo "[PASS] gdb: $1"; npass=$((npass + 1)); }
fail_() { echo "[FAIL] gdb: $1"; nfail=$((nfail + 1)); fail=1; }

# shellcheck source=tests/qemu/emu.sh
source "$(dirname "$0")/emu.sh"
emu_probe "$QEMU" || exit 2
command -v "$GDB" >/dev/null 2>&1 || { echo "[FAIL] GDB introuvable ($GDB)"; exit 2; }
[ -f "$KERNEL" ] || { echo "[FAIL] image absente : $KERNEL"; exit 2; }
# ${GDB_ARCH+x} : une valeur VIDE fournie explicitement est respectée.
[ -n "${GDB_ARCH+x}" ] || GDB_ARCH="$EMU_GDB_ARCH"
emu_print "$QEMU"
echo "[EMU] gdb: $("$GDB" --version | head -1) ; architecture imposee: ${GDB_ARCH:-aucune, deduite du fichier ELF}"

start_qemu() {   # $1 = journal série (image de production : pas de debugcon)
    rm -f "$1"       # jamais de journal périmé d'une exécution précédente
    "$QEMU" -m 128M -display none -no-reboot -kernel "$KERNEL" \
        -serial "file:$1" -gdb "tcp::$PORT" -S </dev/null >/dev/null 2>&1 &
    QPID=$!
    sleep 0.5
}
stop_qemu() { kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; }

# gdb_run JOURNAL COMMANDES... : connexion adaptée à l'émulateur, puis les
# commandes du test. Statut : 0 seulement si GDB a réussi ET si le journal ne
# contient aucun diagnostic d'échec connu (connexion, description de
# registres refusée, cible absente, mémoire inaccessible).
gdb_run() {
    local log="$1"; shift
    local rc
    if [ -z "$GDB_ARCH" ]; then
        "$GDB" -q -batch -nx "$KERNEL" -ex "target remote :$PORT" "$@" > "$log" 2>&1
    else
        "$GDB" -q -batch -nx -ex "set architecture $GDB_ARCH" -ex "target remote :$PORT" \
            -ex "symbol-file $KERNEL" "$@" > "$log" 2>&1
    fi
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "[FAIL] gdb: statut $rc (voir $log)"; return 1
    fi
    if grep -qE "Remote 'g' packet reply is too long|Architecture rejected|not compatible with reported target|could not connect|Connection refused|No registers|The program is not being run|Cannot access memory|Truncated register|not supported by the target" "$log"; then
        echo "[FAIL] gdb: erreur GDB dans $log :"
        grep -E "Remote 'g' packet|rejected|not compatible|could not connect|refused|No registers|not being run|Cannot access|Truncated|not supported" "$log" | head -3 | sed 's/^/        /'
        return 1
    fi
    return 0
}

# ---- 1. ABI à l'entrée de kmain ----------------------------------------------
start_qemu "$OUT/gdb_abi.serial.log"
gdb_run "$OUT/gdb_abi.log" \
    -ex 'break *kmain' -ex 'continue' \
    -ex 'printf "ESPMOD16=%d\n", (unsigned int)$esp % 16' \
    -ex 'printf "ARG0=0x%x\n", *(unsigned int *)($esp + 4)' \
    -ex 'printf "ARG1=0x%x\n", *(unsigned int *)($esp + 8)' \
    -ex 'printf "DF=%d\n", ($eflags >> 10) & 1' \
    -ex 'printf "IF=%d\n", ($eflags >> 9) & 1' \
    -ex 'printf "INSTACK=%d\n", ((unsigned int)$esp > (unsigned int)&boot_stack_bottom) && ((unsigned int)$esp < (unsigned int)&boot_stack_top)' \
    -ex 'kill' || fail=1
stop_qemu
check() {   # check NOM MOTIF
    if grep -q "^$2\$" "$OUT/gdb_abi.log"; then pass_ "$1"; else fail_ "$1 (voir $OUT/gdb_abi.log)"; fi
}
check "ESP % 16 == 12 a l'entree de kmain" "ESPMOD16=12"
check "1er argument = 0x2BADB002" "ARG0=0x2badb002"
if grep -q '^ARG1=0x[1-9a-f]' "$OUT/gdb_abi.log"; then pass_ "2e argument (mbi) non nul"; else fail_ "2e argument nul"; fi
check "EFLAGS.DF == 0" "DF=0"
check "EFLAGS.IF == 0" "IF=0"
check "ESP dans la pile de demarrage" "INSTACK=1"

# ---- 2. Magic invalide (test négatif) ----------------------------------------
# Après modification de l'argument, `detach` laisse le CPU reprendre
# l'exécution ; le noyau doit afficher l'erreur puis s'arrêter par HLT.
BLOG="$OUT/gdb_bad_magic.serial.log"
start_qemu "$BLOG"
gdb_run "$OUT/gdb_bad_magic.log" \
    -ex 'break *kmain' -ex 'continue' \
    -ex 'set {unsigned int}($esp + 4) = 0xdeadbeef' \
    -ex 'delete' -ex 'detach' || fail=1
sleep 1
stop_qemu
if grep -q 'KERNEL PANIC: invalid multiboot magic 0xdeadbeef' "$BLOG" &&
   grep -q '\*\*\* system halted' "$BLOG" &&
   ! grep -q 'boot complete' "$BLOG"; then
    pass_ "magic invalide -> refus explicite et arret"
else
    fail_ "magic invalide non detecte (voir $BLOG)"
fi

# ---- 3. Contenu réel de la mémoire VGA ------------------------------------------
VLOG="$OUT/gdb_vga.serial.log"
# Supprimer le journal précédent AVANT de lancer QEMU : sinon la boucle
# d'attente ci-dessous trouve « boot complete » dans l'ancien fichier, GDB
# s'attache pendant que le CPU est encore dans le BIOS, et le test échoue de
# façon intermittente (défaut réellement observé, voir Walkthrough K0.3 §H).
rm -f "$VLOG" "$OUT/vga.bin" "$OUT/vga.txt"
"$QEMU" -m 128M -display none -no-reboot -kernel "$KERNEL" \
    -serial "file:$VLOG" -gdb "tcp::$PORT" </dev/null >/dev/null 2>&1 &
QPID=$!
# Attendre que le noyau ait terminé son démarrage (message final sur la série).
for _ in $(seq 1 50); do grep -q 'boot complete' "$VLOG" 2>/dev/null && break; sleep 0.1; done
gdb_run "$OUT/gdb_vga.log" \
    -ex "dump binary memory $OUT/vga.bin 0xb8000 0xb8fa0" -ex 'kill' || fail=1
stop_qemu
# Octets pairs = caractères, octets impairs = attributs ; 80 caractères par ligne.
# Sans vga.bin (GDB en échec), vga.txt reste vide et le test échoue.
: > "$OUT/vga.txt"
[ -f "$OUT/vga.bin" ] &&
od -An -v -tu1 "$OUT/vga.bin" | tr -s ' ' '\n' | sed '/^$/d' | awk 'NR % 2 == 1' |
    awk '{ printf "%c", ($1 >= 32 && $1 < 127) ? $1 : 32; if (NR % 80 == 0) printf "\n" }' > "$OUT/vga.txt"
# Le journal de démarrage dépasse 25 lignes depuis K0.6 : l'écran a défilé et
# les premières lignes ne sont plus dans le tampon. On cherche donc des lignes
# de FIN de démarrage, toujours visibles (le défilement est testé en K0.3).
if grep -q 'timer: PIT channel 0' "$OUT/vga.txt" && grep -q 'boot complete' "$OUT/vga.txt"; then
    pass_ "texte de demarrage present en memoire VGA 0xB8000"
else
    fail_ "texte absent de la memoire VGA (voir $OUT/vga.txt)"
fi

echo "GDB: $((npass + nfail)) verifications, $npass reussies, $nfail echecs"
exit $fail
