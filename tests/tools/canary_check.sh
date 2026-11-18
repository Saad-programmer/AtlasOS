#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/canary_check.sh                         (K0.7-R1b)
# -----------------------------------------------------------------------------
# Vérifie, dans les ARTEFACTS (et non dans les journaux), l'infrastructure des
# canaris de pile :
#   1. __stack_chk_guard et __stack_chk_fail sont définis (les deux images) ;
#   2. __stack_chk_guard est dans .rodata, dans un segment PT_LOAD sans W, et
#      AUCUN segment inscriptible ne recouvre sa page de 4 KiB ;
#   3. image de test : canary_frame_probe lit le canari au moins deux fois
#      (prologue, épilogue) et appelle __stack_chk_fail ; tout appel à
#      __stack_chk_fail provient d'une fonction de test_canary.c ;
#   4. image de production : AUCUN appel à __stack_chk_fail (pas encore
#      d'instrumentation, R1b).
# Usage : canary_check.sh build/samos.elf build/samos-test.elf
# =============================================================================
set -u
PROD="${1:?usage: canary_check.sh PROD_ELF TEST_ELF}"
TEST="${2:?usage: canary_check.sh PROD_ELF TEST_ELF}"
CANARY_OBJ="${CANARY_OBJ:-build/test/kernel/tests/test_canary.o}"
fail=0
pass() { echo "[PASS] canary: $*"; }
bad()  { echo "[FAIL] canary: $*"; fail=1; }

for t in nm readelf objdump awk; do
    command -v "$t" >/dev/null 2>&1 || { echo "[FAIL] canary: prérequis absent : $t"; exit 2; }
done
for f in "$PROD" "$TEST" "$CANARY_OBJ"; do
    [ -s "$f" ] || { echo "[FAIL] canary: fichier absent : $f"; exit 2; }
done

sym() { nm "$1" | awk -v s="$2" '$3 == s { print $1; exit }'; }

for img in "$PROD" "$TEST"; do
    name="$(basename "$img")"
    g="$(sym "$img" __stack_chk_guard)"; f="$(sym "$img" __stack_chk_fail)"
    rs="$(sym "$img" __rodata_start)"; re="$(sym "$img" __rodata_end)"
    if [ -z "$g" ] || [ -z "$f" ] || [ -z "$rs" ] || [ -z "$re" ]; then
        bad "$name : symbole manquant (guard=${g:-?} fail=${f:-?} rodata=${rs:-?}-${re:-?})"
        continue
    fi
    gi=$((16#$g)); page=$(( gi & ~0xFFF ))
    if [ "$gi" -ge $((16#$rs)) ] && [ $((gi + 4)) -le $((16#$re)) ]; then
        pass "$name : __stack_chk_guard = 0x$g dans .rodata [0x$rs, 0x$re)"
    else
        bad "$name : __stack_chk_guard = 0x$g hors de .rodata"
    fi
    # Segments PT_LOAD : celui qui contient le canari doit être sans W ; aucun
    # segment W ne doit toucher la page du canari.
    seg_ok=0; overlap=""
    while read -r va msz flg; do
        v=$((va)); m=$((msz))
        if [ "$gi" -ge "$v" ] && [ "$gi" -lt $((v + m)) ] && [[ "$flg" != *W* ]]; then seg_ok=1; fi
        if [[ "$flg" == *W* ]] && [ "$v" -lt $((page + 4096)) ] && [ $((v + m)) -gt "$page" ]; then
            overlap="$overlap $(printf '0x%08x' "$v")"
        fi
    done < <(readelf -lW "$img" | awk '$1 == "LOAD" { fl = ""; for (i = 7; i <= NF - 1; i++) fl = fl $i; print $3, $6, fl }')
    [ "$seg_ok" -eq 1 ] && pass "$name : segment du canari sans droit d'écriture" \
                        || bad "$name : canari hors d'un segment lecture seule"
    [ -z "$overlap" ] && pass "$name : page 0x$(printf '%08x' "$page") partagée avec aucun segment inscriptible" \
                      || bad "$name : page du canari partagée avec un segment W :$overlap"
done

# Sites d'appel de __stack_chk_fail, par fonction.
callers() {
    objdump -d --no-show-raw-insn "$1" | awk '
        /^[0-9a-f]+ <[^>]+>:$/ { fn = $2; gsub(/[<>:]/, "", fn) }
        /call.*<__stack_chk_fail>/ { print fn }' | sort -u
}
n_prod="$(objdump -d "$PROD" | grep -c 'call.*<__stack_chk_fail>' || true)"
[ "$n_prod" -eq 0 ] && pass "samos.elf : aucun appel à __stack_chk_fail (production non instrumentée)" \
                    || bad "samos.elf : $n_prod appel(s) à __stack_chk_fail (instrumentation inattendue)"

g="$(sym "$TEST" __stack_chk_guard)"; gaddr="0x$(echo "$g" | sed 's/^0*//')"
probe="$(objdump -d --no-show-raw-insn "$TEST" | awk '/<canary_frame_probe>:$/ { p = 1; next } p && /^$/ { exit } p')"
reads="$(printf '%s\n' "$probe" | grep -c -- "$gaddr," || true)"
calls="$(printf '%s\n' "$probe" | grep -c 'call.*<__stack_chk_fail>' || true)"
if [ "$reads" -ge 2 ] && [ "$calls" -ge 1 ]; then
    pass "samos-test.elf : canary_frame_probe lit le canari $reads fois et appelle __stack_chk_fail"
else
    bad "samos-test.elf : canary_frame_probe non instrumentée (lectures=$reads, appels=$calls)"
fi
allowed="$(nm "$CANARY_OBJ" | awk '$2 ~ /^[tT]$/ { print $3 }' | sort -u)"
stray=""
for fn in $(callers "$TEST"); do
    printf '%s\n' "$allowed" | grep -qx -- "$fn" || stray="$stray $fn"
done
[ -z "$stray" ] && pass "samos-test.elf : appels à __stack_chk_fail limités à test_canary.c ($(callers "$TEST" | tr '\n' ' '))" \
                || bad "samos-test.elf : fonctions instrumentées hors de test_canary.c :$stray"
exit $fail
