#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/canary_check.sh                 (K0.7-R1b, R1c)
# -----------------------------------------------------------------------------
# Vérifie les canaris de pile dans les ARTEFACTS (code machine), pas dans les
# options de compilation : Clang décide fonction par fonction si une
# protection est nécessaire (-fstack-protector-strong).
#
# Définition : une fonction est INSTRUMENTÉE si son code lit __stack_chk_guard
# au moins deux fois (prologue, épilogue) ET appelle __stack_chk_fail.
#
# Pour CHAQUE image (production, test) :
#   1. __stack_chk_guard et __stack_chk_fail sont définis ;
#   2. __stack_chk_guard est dans .rodata, dans un PT_LOAD sans W, et aucun
#      segment inscriptible ne recouvre sa page ;
#   3. les fonctions REQUISES sont instrumentées (fonctions à tableau local
#      dont la protection est vérifiée, liste ci-dessous) ;
#   4. les fonctions EXCLUES ne lisent pas le canari : __stack_chk_fail et le
#      chemin de panique, plus TOUT ce qui est atteignable (call/jmp directs)
#      depuis emergency_puts, le chemin de la panique récursive, qui doit
#      toujours se terminer ;
#   5. cohérence : toute fonction qui appelle __stack_chk_fail lit le canari
#      au moins deux fois ;
#   6. image de test : canary_frame_probe est instrumentée.
# Un résumé (nombre de fonctions instrumentées, sites d'appel) est affiché.
# Usage : canary_check.sh build/samos.elf build/samos-test.elf
# =============================================================================
set -u
PROD="${1:?usage: canary_check.sh PROD_ELF TEST_ELF}"
TEST="${2:?usage: canary_check.sh PROD_ELF TEST_ELF}"
REQUIRED="${CANARY_REQUIRED:-kvprintf kvsnprintf out_number memmap_parse}"
EXCLUDED="__stack_chk_fail panic_at stack_collect emergency_puts df_stack_sentinel_damage"
fail=0
pass() { echo "[PASS] canary: $*"; }
bad()  { echo "[FAIL] canary: $*"; fail=1; }

for t in nm readelf objdump awk sort; do
    command -v "$t" >/dev/null 2>&1 || { echo "[FAIL] canary: prérequis absent : $t"; exit 2; }
done
for f in "$PROD" "$TEST"; do
    [ -s "$f" ] || { echo "[FAIL] canary: fichier absent : $f"; exit 2; }
done
T="$(mktemp -d "${TMPDIR:-/tmp}/samos-canary.XXXXXX")" || exit 2
trap 'rm -rf "$T"' EXIT

sym() { nm "$1" | awk -v s="$2" '$3 == s { print $1; exit }'; }

# table IMAGE GARDE -> lignes « fonction lectures appels »
table() {
    objdump -d --no-show-raw-insn "$1" | awk -v ga="$2," '
        /^[0-9a-f]+ <[^>]+>:$/ { if (fn != "") print fn, r, c; fn = $2; gsub(/[<>:]/, "", fn); r = 0; c = 0; next }
        index($0, ga) { r++ }
        /call.*<__stack_chk_fail>/ { c++ }
        END { if (fn != "") print fn, r, c }'
}
# edges IMAGE -> lignes « appelant appelé » (call, jmp et sauts conditionnels directs)
edges() {
    objdump -d --no-show-raw-insn "$1" | awk '
        /^[0-9a-f]+ <[^>]+>:$/ { fn = $2; gsub(/[<>:]/, "", fn); next }
        /\t(call|jmp|j[a-z]+) +[0-9a-f]+ <[^>+]+>$/ { t = $NF; gsub(/[<>]/, "", t); if (t != fn) print fn, t }' | sort -u
}
closure() {   # closure FICHIER_ARÊTES RACINE...
    local e="$1"; shift
    local todo="$*" seen=" " n c
    while [ -n "${todo// /}" ]; do
        set -- $todo; n="$1"; shift; todo="$*"
        case "$seen" in *" $n "*) continue ;; esac
        seen="$seen$n "
        for c in $(awk -v n="$n" '$1 == n { print $2 }' "$e"); do todo="$todo $c"; done
    done
    echo $seen
}

for img in "$PROD" "$TEST"; do
    name="$(basename "$img")"
    g="$(sym "$img" __stack_chk_guard)"; f="$(sym "$img" __stack_chk_fail)"
    rs="$(sym "$img" __rodata_start)"; re="$(sym "$img" __rodata_end)"
    if [ -z "$g" ] || [ -z "$f" ] || [ -z "$rs" ] || [ -z "$re" ]; then
        bad "$name : symbole manquant (guard=${g:-?} fail=${f:-?} rodata=${rs:-?}-${re:-?})"
        continue
    fi
    # --- 2. emplacement et droits du canari
    gi=$((16#$g)); page=$(( gi & ~0xFFF ))
    if [ "$gi" -ge $((16#$rs)) ] && [ $((gi + 4)) -le $((16#$re)) ]; then
        pass "$name : __stack_chk_guard = 0x$g dans .rodata [0x$rs, 0x$re)"
    else
        bad "$name : __stack_chk_guard = 0x$g hors de .rodata"
    fi
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

    # --- tables d'instrumentation
    ga="0x$(echo "$g" | sed 's/^0*//')"
    table "$img" "$ga" > "$T/$name.tab"
    awk '$3 > 0 && $2 >= 2 { print $1 }' "$T/$name.tab" | sort -u > "$T/$name.ins"
    nins=$(wc -l < "$T/$name.ins"); nsites=$(awk '{ s += $3 } END { print s + 0 }' "$T/$name.tab")
    nfn=$(wc -l < "$T/$name.tab")
    echo "  $name : $nins fonctions instrumentées sur $nfn, $nsites site(s) d'appel de __stack_chk_fail"

    # --- 3. fonctions requises
    missing=""
    for fn in $REQUIRED; do grep -qx -- "$fn" "$T/$name.ins" || missing="$missing $fn"; done
    [ -z "$missing" ] && pass "$name : fonctions requises instrumentées ($REQUIRED)" \
                      || bad "$name : fonctions requises NON instrumentées :$missing"

    # --- 4. exclusions, y compris la fermeture du chemin de secours
    edges "$img" > "$T/$name.edges"
    secours="$(closure "$T/$name.edges" emergency_puts)"
    leaked=""
    for fn in $EXCLUDED; do
        # Une fonction exclue absente rendrait la vérification vide de sens.
        awk -v f="$fn" '$1 == f { found = 1 } END { exit !found }' "$T/$name.tab" \
            || bad "$name : fonction exclue absente de l'image : $fn"
    done
    for fn in $EXCLUDED $secours; do
        r=$(awk -v f="$fn" '$1 == f { print $2 }' "$T/$name.tab")
        [ "${r:-0}" -eq 0 ] || leaked="$leaked $fn"
    done
    [ -z "$leaked" ] && pass "$name : exclusions respectées ($EXCLUDED ; chemin de secours : $secours)" \
                     || bad "$name : fonctions exclues qui lisent le canari :$leaked"

    # --- 5. cohérence
    odd="$(awk '$3 > 0 && $2 < 2 { printf " %s", $1 }' "$T/$name.tab")"
    [ -z "$odd" ] && pass "$name : chaque appel à __stack_chk_fail suit deux lectures du canari" \
                  || bad "$name : appel à __stack_chk_fail sans double lecture :$odd"
done

# --- 6. sonde de l'image de test
if grep -qx canary_frame_probe "$T/$(basename "$TEST").ins" 2>/dev/null; then
    pass "samos-test.elf : canary_frame_probe instrumentée"
else
    bad "samos-test.elf : canary_frame_probe NON instrumentée"
fi
exit $fail
