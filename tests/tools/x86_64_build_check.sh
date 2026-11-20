#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/x86_64_build_check.sh        (K1.1-M64-1)
# -----------------------------------------------------------------------------
# Vérification de la chaîne de construction x86-64 (make test-x86_64-build).
# Elle ne démarre AUCUNE image : le squelette ELF64 n'est pas amorçable.
#
#   A. Drapeaux : chaque compilation C x86-64 porte les options requises
#      (-mno-red-zone, -mgeneral-regs-only, -fno-pic, -mcmodel=small, ...) et
#      aucune option interdite ; NASM en elf64 ; ld en elf_x86_64 avec
#      --fatal-warnings. Tests négatifs F1 à F6 : drapeaux falsifiés par
#      surcharge de variable (make -n : rien n'est construit).
#      F7 : options de ld qui masqueraient un symbole indéfini interdites.
#      F8 à F15 : options contraires ajoutées APRÈS les options requises
#      (-mcmodel=large, -msse4.2, -fno-stack-protector, -Wno-error,
#      -fcf-protection=full, -fhosted, -fomit-frame-pointer, -fstack-protector) ;
#      pour chaque option requise, la dernière de sa famille doit être elle.
#      F16 à F20 : idem pour ld et NASM (--no-fatal-warnings, -z execstack,
#      second -m, second -T, second -f).
#   B. Séparation : tout objet de build/x86_64 est ELF64 x86-64 ; tout objet
#      i386 (build/prod, build/test) est ELF32 i386. Test négatif S1.
#   B'. Symboles des entrées de liaison (L0 à L2) : tout symbole référencé
#      par un objet du squelette est défini par un autre objet du même jeu,
#      et aucun symbole faible n'est indéfini. Ce contrôle porte sur les
#      OBJETS, car GNU ld résout un symbole indéfini faible (ou forcé par
#      --unresolved-symbols=ignore-all) à 0 et le RETIRE de la table des
#      symboles de l'exécutable statique : la règle X8 de check_elf64.sh ne
#      peut donc pas le voir dans l'image (constaté en M64-1).
#   C. Contrôles ELF64 (tools/check_elf64.sh) sur les deux images, dans leur
#      mode, puis contre-épreuves croisées.
#   D. Tests négatifs du vérificateur. Pour chacun (révision M64-1-AUDIT) :
#      1. une SONDE indépendante du vérificateur (readelf, objdump, od)
#         prouve que le défaut est réellement présent dans l'image falsifiée ;
#      2. le vérificateur doit sortir avec le statut 1 (règle en échec), et
#         non 0 (accepté) ni 2 (prérequis manquant, plantage) ;
#      3. la règle PRINCIPALE attendue doit échouer, et l'ensemble des règles
#         en échec doit rester dans l'ensemble déclaré pour ce défaut ;
#         toute autre règle en échec signale un refus étranger au défaut.
#      Une liaison refusée par une assertion R4 compte seulement si le
#      message attendu apparaît.
#   E. Fraîcheur et reproductibilité : deux constructions neuves dans deux
#      répertoires ; les contrôles C portent sur la première ; les images de
#      build/x86_64 doivent lui être identiques (sinon : images PÉRIMÉES, par
#      exemple construites avec d'autres drapeaux, que Make ne détecte pas) ;
#      les deux constructions neuves doivent être identiques entre elles.
# Sortie : 0 si tout est conforme, 1 sinon.
# =============================================================================
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 1
X64="build/x86_64"
PROD="$X64/samos64.elf"
TEST="$X64/samos64-test.elf"
CHK="tools/check_elf64.sh"
W="build/tests/x86_64_build"   # hors de build/x86_64 : non compté par la séparation
rm -rf "$W"; mkdir -p "$W"
fail=0
ok()  { echo "[PASS] x86_64-build $1"; }
ko()  { echo "[FAIL] x86_64-build $1"; fail=1; }
for f in "$PROD" "$TEST" build/samos.elf build/samos-test.elf; do
    [ -f "$f" ] || { echo "[FAIL] x86_64-build: image absente $f (make all x86_64)"; exit 1; }
done
# Deux constructions neuves (indépendantes de tout fichier généré antérieur).
FRESH1="$W/fresh1"; FRESH2="$W/fresh2"
for d in "$FRESH1" "$FRESH2"; do
    make -s --no-print-directory X64_BUILD="$d" "$d/samos64.elf" "$d/samos64-test.elf" > "$d.log" 2>&1 \
        || { echo "[FAIL] x86_64-build: construction neuve impossible dans $d (voir $d.log)"; exit 1; }
done
# Objets d'entrée réels, tels que le Makefile les définit (pas de « find »
# susceptible de compter des objets périmés).
mkvar() { make -s --no-print-directory X64_BUILD="$FRESH1" --eval "print-var: ; @echo \$($1)" print-var; }
P_OBJS=$(mkvar X64_PROD_OBJS); T_OBJS=$(mkvar X64_TEST_OBJS)
[ -n "$P_OBJS" ] && [ -n "$T_OBJS" ] || { echo "[FAIL] x86_64-build: listes d'objets vides"; exit 1; }
FPROD="$FRESH1/samos64.elf"; FTEST="$FRESH1/samos64-test.elf"

# ---- A. Drapeaux ------------------------------------------------------------
REQ_C="--target=x86_64-unknown-none-elf -march=x86-64 -ffreestanding -nostdlibinc -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small -fstack-protector-strong -fcf-protection=none -fno-omit-frame-pointer -Werror"
# Famille d'une option (révision M64-1-AUDIT, défaut D6) : deux options de la
# même famille se contredisent, et Clang applique la DERNIÈRE. Pour chaque
# option requise, la dernière option de sa famille sur la ligne doit être
# exactement l'option requise ; sinon elle est annulée (ex. -fno-stack-protector
# après -fstack-protector-strong, -Wno-error après -Werror, -fhosted après
# -ffreestanding, -fcf-protection=full après -fcf-protection=none).
# opt_values OPTION MOTS... : valeurs (mot suivant) de chaque occurrence de
# OPTION, séparées par des espaces.
opt_values() {
    local o="$1" out="" prev=""; shift
    for t in "$@"; do [ "$prev" = "$o" ] && out="$out $t"; prev="$t"; done
    echo "${out# }"
}
opt_family() {   # résultat dans REPLY (sans sous-shell : appelée pour chaque option)
    case "$1" in
        --target=*|-target) REPLY=target ;;
        -march=*) REPLY=march ;;
        -mcmodel=*) REPLY=mcmodel ;;
        -fcf-protection|-fcf-protection=*) REPLY=cf-protection ;;
        -fstack-protector*|-fno-stack-protector*) REPLY=stack-protector ;;
        -ffreestanding|-fhosted|-fno-freestanding|-fno-hosted) REPLY=freestanding ;;
        -fpic|-fPIC|-fno-pic|-fno-PIC) REPLY=pic ;;
        -fpie|-fPIE|-fno-pie|-fno-PIE) REPLY=pie ;;
        -Werror|-Wno-error|-Werror=*|-Wno-error=*) REPLY=werror ;;
        -fno-*) REPLY="f:${1#-fno-}" ;;
        -f*) REPLY="f:${1#-f}" ;;
        -mno-*) REPLY="m:${1#-mno-}" ;;
        -m*) REPLY="m:${1#-m}" ;;
        -Wno-*) REPLY="W:${1#-Wno-}" ;;
        -W*) REPLY="W:${1#-W}" ;;
        *) REPLY="=$1" ;;
    esac
}
# Interdits par MOTIF (Clang applique la dernière occurrence d'une option :
# une option contraire ajoutée après une option requise l'annulerait).
forbidden_token() {
    case "$1" in
        -fpic|-fPIC|-fpie|-fPIE|-mred-zone|-m32|-m16|-mx32) return 0 ;;
        -march=x86-64|--target=x86_64-unknown-none-elf|-mcmodel=small) return 1 ;;
        -march=*|--target=*|-target|-mcmodel=*|-mstack-alignment=*) return 0 ;;
        -msse*|-mavx*|-mmmx|-m3dnow*|-m80387|-mx87|-mfpmath=*|-mno-general-regs-only) return 0 ;;
    esac
    return 1
}
# flags_ok [SURCHARGES make...] : 0 si toutes les commandes x86-64 sont conformes.
flags_ok() {
    local log; log="$(make -n -B x86_64 "$@" 2>&1)" || { echo "    make -n en echec"; return 1; }
    local cc as ld r=0 line
    cc=$(grep -E '^clang .* -o build/x86_64/' <<<"$log"); as=$(grep -E '^nasm .* -o build/x86_64/' <<<"$log")
    ld=$(grep -E '^ld .* -o build/x86_64/' <<<"$log")
    [ "$(grep -c . <<<"$cc")" -ge 2 ] && [ "$(grep -c . <<<"$as")" -ge 2 ] && [ "$(grep -c . <<<"$ld")" -eq 2 ] \
        || { echo "    commandes x86-64 manquantes"; return 1; }
    while IFS= read -r line; do
        # Une passe : dernière option de chaque famille sur la ligne.
        local -A lastof=(); local t
        for t in $line; do opt_family "$t"; lastof[$REPLY]="$t"; done
        for f in $REQ_C; do
            grep -qF -- " $f " <<<" $line " || { echo "    option requise absente : $f"; r=1; continue; }
            opt_family "$f"
            [ "${lastof[$REPLY]}" = "$f" ] || { echo "    option requise annulee : $f (par ${lastof[$REPLY]})"; r=1; }
        done
        for f in $line; do forbidden_token "$f" && { echo "    option interdite : $f"; r=1; }; done
    done <<<"$cc"
    while IFS= read -r line; do
        grep -qF -- " -f elf64 " <<<" $line " || { echo "    NASM sans -f elf64"; r=1; }
        # Un seul format de sortie : un second « -f » annulerait le premier.
        [ "$(opt_values -f $line)" = "elf64" ] || { echo "    NASM : format multiple ou autre que elf64 ($(opt_values -f $line))"; r=1; }
    done <<<"$as"
    while IFS= read -r line; do
        for f in "-m elf_x86_64" "--fatal-warnings" "-T kernel/arch/x86_64/linker.ld" "-nostdlib" "-z noexecstack"; do
            grep -qF -- " $f " <<<" $line " || { echo "    ld sans $f"; r=1; }
        done
        # Options à valeur : une seule occurrence, avec la valeur attendue (une
        # seconde occurrence de -m ou -T remplacerait la première).
        [ "$(opt_values -m $line)" = "elf_x86_64" ] || { echo "    ld : emulation multiple ou autre que elf_x86_64 ($(opt_values -m $line))"; r=1; }
        [ "$(opt_values -T $line)" = "kernel/arch/x86_64/linker.ld" ] || { echo "    ld : script de liaison multiple ou autre ($(opt_values -T $line))"; r=1; }
        grep -qE -- ' -z execstack( |$)' <<<" $line " && { echo "    ld : -z execstack"; r=1; }
        grep -qE -- ' --no-fatal-warnings( |$)' <<<" $line " && { echo "    ld : --no-fatal-warnings annule --fatal-warnings"; r=1; }
        # Options qui laisseraient un symbole indéfini produire une image.
        grep -qE -- ' (--unresolved-symbols|--noinhibit-exec|--warn-unresolved-symbols|-pie|--pie|-shared|-r)([= ]|$)' <<<" $line " \
            && { echo "    option ld interdite"; r=1; }
    done <<<"$ld"
    # Aucune commande x86-64 ne doit écrire dans les répertoires i386.
    grep -E '^(clang|nasm|ld) .*--target=x86_64|^(nasm .*-f elf64|ld .*elf_x86_64)' <<<"$log" | grep -qE ' -o build/(prod|test)/| -o build/samos' \
        && { echo "    sortie x86-64 dans un repertoire i386"; r=1; }
    return $r
}
cur=$(make -s --no-print-directory --eval 'print-x64-cflags: ; @echo $(X64_CFLAGS)' print-x64-cflags)
[ -n "$cur" ] || { echo "[FAIL] x86_64-build: X64_CFLAGS illisible"; exit 1; }
if flags_ok > "$W/F0.log"; then ok "A/F0: drapeaux x86-64 conformes (C, NASM, ld)"; else ko "A/F0: drapeaux non conformes"; cat "$W/F0.log"; fi
neg_flags() {   # neg_flags NOM MOTIF_ATTENDU SURCHARGE...
    local n="$1" want="$2"; shift 2
    # Injection réelle : la surcharge doit différer de la valeur courante.
    local var="${1%%=*}" val="${1#*=}" orig
    orig=$(make -s --no-print-directory --eval "print-var: ; @echo \$($var)" print-var)
    if [ -z "$orig" ] || [ "$val" = "$orig" ]; then ko "A/$n: injection impossible ($var inchangee ou vide)"; return; fi
    if flags_ok "$@" > "$W/$n.log"; then ko "A/$n: drapeaux falsifies ACCEPTES"
    elif grep -qF -- "$want" "$W/$n.log"; then ok "A/$n: refuse ($want)"
    else ko "A/$n: refuse pour une autre raison (voir $W/$n.log)"; fi
}
neg_flags F1_no_red_zone_missing "option requise absente : -mno-red-zone" "X64_CFLAGS=${cur/-mno-red-zone/}"
neg_flags F2_pic "option interdite : -fPIC" "X64_CFLAGS=$cur -fPIC"
neg_flags F3_i686_target "option requise absente : --target=x86_64-unknown-none-elf" "X64_CFLAGS=${cur/--target=x86_64-unknown-none-elf/--target=i686-unknown-none-elf}"
neg_flags F4_sse_regs "option requise absente : -mgeneral-regs-only" "X64_CFLAGS=${cur/-mgeneral-regs-only/}"
neg_flags F5_nasm_elf32 "NASM sans -f elf64" "X64_ASFLAGS=-f elf32 -g -F dwarf -w+all -w+error"
neg_flags F6_ld_not_fatal "ld sans --fatal-warnings" "X64_LDFLAGS=-m elf_x86_64 -T kernel/arch/x86_64/linker.ld -nostdlib -z max-page-size=0x1000 -z noexecstack --build-id=none"
neg_flags F8_mcmodel_large "option interdite : -mcmodel=large" "X64_CFLAGS=$cur -mcmodel=large"
neg_flags F9_sse42 "option interdite : -msse4.2" "X64_CFLAGS=$cur -msse4.2"
neg_flags F10_no_stack_protector "option requise annulee : -fstack-protector-strong (par -fno-stack-protector)" "X64_CFLAGS=$cur -fno-stack-protector"
neg_flags F11_wno_error "option requise annulee : -Werror (par -Wno-error)" "X64_CFLAGS=$cur -Wno-error"
neg_flags F12_cf_protection_full "option requise annulee : -fcf-protection=none (par -fcf-protection=full)" "X64_CFLAGS=$cur -fcf-protection=full"
neg_flags F13_hosted "option requise annulee : -ffreestanding (par -fhosted)" "X64_CFLAGS=$cur -fhosted"
neg_flags F14_omit_frame_pointer "option requise annulee : -fno-omit-frame-pointer (par -fomit-frame-pointer)" "X64_CFLAGS=$cur -fomit-frame-pointer"
neg_flags F15_weaker_stack_protector "option requise annulee : -fstack-protector-strong (par -fstack-protector)" "X64_CFLAGS=$cur -fstack-protector"
LD0=$(make -s --no-print-directory --eval 'print-x64-ld: ; @echo $(X64_LDFLAGS)' print-x64-ld)
AS0=$(make -s --no-print-directory --eval 'print-x64-as: ; @echo $(X64_ASFLAGS)' print-x64-as)
neg_flags F16_ld_no_fatal "ld : --no-fatal-warnings annule --fatal-warnings" "X64_LDFLAGS=$LD0 --no-fatal-warnings"
neg_flags F17_ld_execstack "ld : -z execstack" "X64_LDFLAGS=$LD0 -z execstack"
neg_flags F18_ld_second_emulation "ld : emulation multiple ou autre que elf_x86_64" "X64_LDFLAGS=$LD0 -m elf_i386"
neg_flags F19_ld_second_script "ld : script de liaison multiple ou autre" "X64_LDFLAGS=$LD0 -T /dev/null"
neg_flags F20_nasm_second_format "NASM : format multiple ou autre que elf64" "X64_ASFLAGS=$AS0 -f elf32"
neg_flags F7_ld_ignore_unresolved "option ld interdite" "X64_LDFLAGS=-m elf_x86_64 -T kernel/arch/x86_64/linker.ld -nostdlib -z max-page-size=0x1000 -z noexecstack --build-id=none --fatal-warnings --unresolved-symbols=ignore-all"

# ---- B. Séparation des objets ------------------------------------------------
# sep_ok RACINE_X64 RACINES_I386... : classe et machine de chaque objet.
sep_ok() {
    local x="$1" r=0 o h; shift
    local nx=0 ni=0
    while IFS= read -r o; do
        h=$(readelf -h "$o" 2>/dev/null)
        { grep -q 'Class:[[:space:]]*ELF64' <<<"$h" && grep -q 'X86-64' <<<"$h"; } || { echo "    $o n'est pas ELF64 x86-64"; r=1; }
        nx=$((nx + 1))
    done < <(find "$x" -name '*.o')
    for d in "$@"; do
        while IFS= read -r o; do
            h=$(readelf -h "$o" 2>/dev/null)
            { grep -q 'Class:[[:space:]]*ELF32' <<<"$h" && grep -q 'Intel 80386' <<<"$h"; } || { echo "    $o n'est pas ELF32 i386"; r=1; }
            ni=$((ni + 1))
        done < <(find "$d" -name '*.o')
    done
    [ "$nx" -gt 0 ] && [ "$ni" -gt 0 ] || { echo "    aucun objet trouve ($nx x86-64, $ni i386)"; r=1; }
    echo "    objets : $nx x86-64, $ni i386"
    return $r
}
if sep_ok "$X64" build/prod build/test > "$W/S0.log" && sep_ok "$FRESH1" build/prod > "$W/S0f.log"; then ok "B/S0: separation des objets ($(grep -o 'objets.*' "$W/S0.log"))"; else ko "B/S0: contamination croisee"; cat "$W/S0.log"; fi
mkdir -p "$W/S1/x64" "$W/S1/i386"
cp "$(find build/prod -name '*.o' | head -1)" "$W/S1/x64/intrus.o"
cp "$(find "$X64/prod" -name '*.o' | head -1)" "$W/S1/i386/intrus64.o"
if sep_ok "$W/S1/x64" "$W/S1/i386" > "$W/S1.log"; then ko "B/S1: objets croises ACCEPTES"
elif grep -q "n'est pas ELF64" "$W/S1.log" && grep -q "n'est pas ELF32" "$W/S1.log"; then ok "B/S1: objet i386 parmi les x86-64 et inversement : refuse"
else ko "B/S1: refus incomplet (voir $W/S1.log)"; fi

# ---- B'. Symboles des entrées de liaison ------------------------------------
# syms_ok OBJETS... : 0 si chaque symbole indéfini (U) est défini par un autre
# objet du jeu et si aucun symbole faible indéfini (w, v) n'existe.
syms_ok() {
    local und def weak r=0 s
    und=$(nm -u "$@" 2>/dev/null | awk '$1 == "U" {print $2}' | sort -u)
    weak=$(nm -u "$@" 2>/dev/null | awk '$1 == "w" || $1 == "v" {print $2}' | sort -u)
    def=$(nm --defined-only "$@" 2>/dev/null | awk 'NF == 3 {print $3}' | sort -u)
    for s in $und; do grep -qxF "$s" <<<"$def" || { echo "    symbole non defini par le jeu d'objets : $s"; r=1; }; done
    for s in $weak; do echo "    symbole faible indefini (resolu a 0 en silence) : $s"; r=1; done
    return $r
}
# shellcheck disable=SC2086
if syms_ok $P_OBJS > "$W/L0p.log" && syms_ok $T_OBJS > "$W/L0t.log"; then
    ok "B'/L0: entrees de liaison production et test : tout symbole reference est defini"
else ko "B'/L0: symbole indefini dans les entrees reelles"; cat "$W/L0p.log" "$W/L0t.log"; fi
printf 'bits 64\nsection .text\nextern samos64_missing_symbol\nglobal undef_user\nundef_user: call samos64_missing_symbol\nsection .note.GNU-stack noalloc noexec nowrite progbits\n' > "$W/undef.asm"
printf 'bits 64\nsection .text\nextern samos64_missing_weak:weak\nglobal weak_user\nweak_user: mov rax, samos64_missing_weak\n    ret\nsection .note.GNU-stack noalloc noexec nowrite progbits\n' > "$W/weak.asm"
nasm -f elf64 "$W/undef.asm" -o "$W/undef.o" && nasm -f elf64 "$W/weak.asm" -o "$W/weak.o"
# shellcheck disable=SC2086
if syms_ok $P_OBJS "$W/undef.o" > "$W/L1.log"; then ko "B'/L1: symbole indefini ACCEPTE"
elif grep -q 'non defini par le jeu.*samos64_missing_symbol' "$W/L1.log"; then ok "B'/L1: symbole indefini refuse (samos64_missing_symbol)"
else ko "B'/L1: refus sans le diagnostic attendu (voir $W/L1.log)"; fi
# shellcheck disable=SC2086
if syms_ok $P_OBJS "$W/weak.o" > "$W/L2.log"; then ko "B'/L2: symbole faible indefini ACCEPTE"
elif grep -q 'faible indefini.*samos64_missing_weak' "$W/L2.log"; then ok "B'/L2: symbole faible indefini refuse (samos64_missing_weak)"
else ko "B'/L2: refus sans le diagnostic attendu (voir $W/L2.log)"; fi

# ---- C. Contrôles ELF64 sur les images réelles -------------------------------
if "$CHK" "$FPROD" --production > "$W/C0.log"; then ok "C/C0: image de production conforme ($(grep -c '^\[PASS\]' "$W/C0.log") regles)"; else ko "C/C0: production non conforme"; grep FAIL "$W/C0.log"; fi
if "$CHK" "$FTEST" --test > "$W/C1.log"; then ok "C/C1: image de test conforme ($(grep -c '^\[PASS\]' "$W/C1.log") regles)"; else ko "C/C1: test non conforme"; grep FAIL "$W/C1.log"; fi

# ---- D. Tests négatifs du vérificateur ---------------------------------------
# must_fail NOM PRINCIPALE AUTORISEES IMAGE MODE SONDE... : SONDE (commande
# indépendante du vérificateur) prouve l'injection ; puis statut 1, règle
# PRINCIPALE en échec, règles en échec incluses dans AUTORISEES.
must_fail() {
    local n="$1" rule="$2" allowed="$3" img="$4" mode="$5"; shift 5
    if [ ! -f "$img" ]; then ko "D/$n: image falsifiee non produite (voir $W/$n.link.log)"; return; fi
    if ! "$@" > "$W/$n.probe.log" 2>&1; then ko "D/$n: defaut NON injecte (sonde en echec : $*)"; return; fi
    "$CHK" "$img" "$mode" > "$W/$n.log" 2>&1; local rc=$?
    local failed extra
    failed=$(grep -oE '^\[FAIL\] elf64 X[0-9]+' "$W/$n.log" | awk '{print $3}' | sort -u | tr '\n' ' ')
    extra=""; for r in $failed; do case " $allowed " in *" $r "*) ;; *) extra="$extra $r" ;; esac; done
    if [ "$rc" -eq 0 ]; then ko "D/$n: anomalie ACCEPTEE par check_elf64"
    elif [ "$rc" -ne 1 ]; then ko "D/$n: verificateur en echec de prerequis ou plantage (statut $rc, voir $W/$n.log)"
    elif ! grep -qw "$rule" <<<"$failed"; then ko "D/$n: refuse sans la regle $rule (echecs : $failed)"
    elif [ -n "$extra" ]; then ko "D/$n: refus etranger au defaut (regles$extra hors de {$allowed})"
    else ok "D/$n: defaut injecte (sonde) puis refuse par $rule (echecs : ${failed% })"; fi
}
LDS=kernel/arch/x86_64/linker.ld
POBJ="$P_OBJS"
LDBASE="-m elf_x86_64 -nostdlib -z max-page-size=0x1000 -z noexecstack --build-id=none"
asm() { printf '%s\n' "$2" > "$W/$1.asm" && nasm -f elf64 -w+all "$W/$1.asm" -o "$W/$1.o"; }
link() { local n="$1" s="$2"; shift 2; ld $LDBASE -T "$s" -o "$W/$n.elf" "$@" > "$W/$n.link.log" 2>&1; }
noassert() { sed '/^ASSERT/d' "$LDS" > "$W/$1.ld"; }
# Sondes indépendantes du vérificateur.
has_class32() { readelf -h "$1" | grep -q 'Class:[[:space:]]*ELF32'; }
has_sym()     { nm "$1" | awk '{print $NF}' | grep -qx "$2"; }
lacks_sym()   { ! has_sym "$@"; }
has_rwx()     { readelf -lW "$1" | awk '$1 == "LOAD"' | grep -qE ' RWE '; }
# Numéro de section retiré d'abord : « [ 1] » et « [10] » donnent un nombre
# de champs différent (même normalisation que check_elf64.sh).
sec_unaligned() { local a; a=$(readelf -SW "$1" | sed -n 's/^ *\[ *[0-9]*\] //p' | awk -v s="$2" '$1 == s {print $3}'); [ -n "$a" ] && [ $((0x$a % 4096)) -ne 0 ]; }
has_magic()   { od -An -v -tx4 -N 32768 "$1" | tr -s ' ' '\n' | grep -qix "$2"; }
has_insn()    { objdump -d -M att --no-show-raw-insn "$1" | grep -qE "$2"; }
has_sec()     { readelf -SW "$1" | grep -qF " $2 "; }
entry_not_start() { local e s; e=$(readelf -h "$1" | awk '/Entry point/ {print $4}'); s=$(nm "$1" | awk '$3 == "_start" {print "0x" $1}'); [ $((e)) -ne $((s)) ]; }
first_load_below() { local v; v=$(readelf -lW "$1" | awk '$1 == "LOAD" {print $3; exit}'); [ $((v)) -lt $((0x100000)) ]; }
rodata_in_rw() { readelf -lW "$1" | sed -n '/Section to Segment/,$p' | grep -qE '^ +02 +\.rodata'; }

must_fail N1_i386_image X1 "X1 X2 X5 X6 X7 X8 X9 X11 X12 X13" build/samos.elf --production has_class32 build/samos.elf
must_fail N2_test_as_production X10 "X10" "$FTEST" --production has_sym "$FTEST" ktest_m64_skeleton_marker
must_fail N3_production_as_test X10 "X10" "$FPROD" --test lacks_sym "$FPROD" ktest_m64_skeleton_marker
# N4 : segment RWX (PHDRS text FLAGS(7)), lié sans --fatal-warnings.
sed 's/text   PT_LOAD FLAGS(5)/text   PT_LOAD FLAGS(7)/' "$LDS" > "$W/N4.ld"; link N4_rwx_segment "$W/N4.ld" $POBJ
must_fail N4_rwx_segment X7 "X7" "$W/N4_rwx_segment.elf" --production has_rwx "$W/N4_rwx_segment.elf"
# N5a : .rodata non alignée, script original : l'assertion R4 refuse la liaison.
sed 's/^    \.rodata ALIGN(PAGE_SIZE) :/    .rodata :/' "$LDS" > "$W/N5a.ld"
if link N5a_rodata_unaligned "$W/N5a.ld" $POBJ; then ko "D/N5a: liaison ACCEPTEE malgre .rodata non alignee"
elif grep -q "R4 : .rodata non alignee" "$W/N5a_rodata_unaligned.link.log"; then ok "D/N5a: liaison refusee par l'assertion R4 (.rodata)"
else ko "D/N5a: liaison refusee pour une autre raison (voir $W/N5a_rodata_unaligned.link.log)"; fi
# N5b : même défaut sans assertions : check_elf64 doit le refuser seul.
sed -e 's/^    \.rodata ALIGN(PAGE_SIZE) :/    .rodata :/' -e '/^ASSERT/d' "$LDS" > "$W/N5b.ld"; link N5b_rodata_unaligned "$W/N5b.ld" $POBJ
must_fail N5b_rodata_unaligned X6 "X6 X7" "$W/N5b_rodata_unaligned.elf" --production sec_unaligned "$W/N5b_rodata_unaligned.elf" .rodata
# N6 : (supprimé de l'image) un symbole indéfini n'est pas visible dans un
# exécutable statique lié par GNU ld : il est contrôlé sur les objets (B'/L1,
# B'/L2) et par l'interdiction des options de ld qui le toléreraient (A/F7).
# N7 : magic Multiboot 1 dans .rodata (mécanisme de chargement avant A-1).
asm mb1 "bits 64
section .rodata
dd 0x1BADB002, 0, 0"
link N7_multiboot_magic "$LDS" $POBJ "$W/mb1.o"
must_fail N7_multiboot_magic X5 "X5" "$W/N7_multiboot_magic.elf" --production has_magic "$W/N7_multiboot_magic.elf" 1badb002
# N7b : magic Multiboot 2.
asm mb2 "bits 64
section .rodata
dd 0xE85250D6, 0, 0, 0"
link N7b_multiboot2_magic "$LDS" $POBJ "$W/mb2.o"
must_fail N7b_multiboot2_magic X5 "X5" "$W/N7b_multiboot2_magic.elf" --production has_magic "$W/N7b_multiboot2_magic.elf" e85250d6
# N8 à N12 : instructions hors périmètre M64-1.
i=0
for spec in "N8_write_cr4|mov cr4, rax|%cr4" "N9_wrmsr|wrmsr|wrmsr" "N10_lgdt|lgdt [rax]|lgdt" "N11_sse|movaps xmm0, xmm1|%xmm" "N12_read_cr3|mov rax, cr3|%cr3" \
            "N18_x87_fninit|fninit|fninit" "N19_x87_fldz|fldz|fldz" "N20_mmx_emms|emms|emms" "N21_sse_ldmxcsr|ldmxcsr [rax]|ldmxcsr" \
            "N22_fxsave|fxsave [rax]|fxsave" "N23_avx_vzeroupper|vzeroupper|vzeroupper" "N24_cr0_lmsw|lmsw ax|lmsw" "N25_cr0_clts|clts|clts" \
            "N26_invlpg|invlpg [rax]|invlpg" "N27_iretd|iretd|iret" "N28_debug_reg|mov rax, dr7|%db7"; do
    IFS='|' read -r n ins probe <<<"$spec"; i=$((i + 1))
    asm "ins$i" "bits 64
section .text
global forbidden_$i
forbidden_$i: $ins
    ret"
    link "$n" "$LDS" $POBJ "$W/ins$i.o"
    must_fail "$n" X11 "X11" "$W/$n.elf" --production has_insn "$W/$n.elf" "$probe"
done
# N13 : section ALLOC inattendue.
asm extra "bits 64
section .samos_extra progbits alloc noexec nowrite
dd 1"
noassert N13; link N13_extra_section "$W/N13.ld" $POBJ "$W/extra.o"
must_fail N13_extra_section X6 "X6 X7 X13" "$W/N13_extra_section.elf" --production has_sec "$W/N13_extra_section.elf" .samos_extra
# N14 : point d'entrée autre que _start.
link N14_entry "$LDS" $POBJ -e samos64_skeleton_probe
must_fail N14_entry X4 "X4" "$W/N14_entry.elf" --production entry_not_start "$W/N14_entry.elf"
# N15 : image liée sous 1 MiB (assertions retirées).
sed -e 's/^KERNEL_LOAD_ADDR = 0x00100000;/KERNEL_LOAD_ADDR = 0x00010000;/' -e '/^ASSERT/d' "$LDS" > "$W/N15.ld"; link N15_below_1mib "$W/N15.ld" $POBJ
must_fail N15_below_1mib X7 "X7 X9" "$W/N15_below_1mib.elf" --production first_load_below "$W/N15_below_1mib.elf"
# N16 : image partageant une page entre .rodata et .data (ALIGN de .data retiré).
sed -e 's/^    \.data ALIGN(PAGE_SIZE) :/    .data :/' -e '/^ASSERT/d' "$LDS" > "$W/N16.ld"; link N16_shared_page "$W/N16.ld" $POBJ
must_fail N16_shared_page X6 "X6 X7" "$W/N16_shared_page.elf" --production sec_unaligned "$W/N16_shared_page.elf" .data

# N17 : .rodata placée dans le segment RW (constantes inscriptibles).
sed 's/^    } :rodata$/    } :data/' "$LDS" > "$W/N17.ld"; link N17_rodata_in_rw "$W/N17.ld" $POBJ
must_fail N17_rodata_in_rw X13 "X7 X13" "$W/N17_rodata_in_rw.elf" --production rodata_in_rw "$W/N17_rodata_in_rw.elf"
# N29 : fichier non ELF : refus X1 (statut 1), pas d'acceptation.
printf 'not an ELF file\n' > "$W/N29.elf"
must_fail N29_not_elf X1 "X1" "$W/N29.elf" --production test -f "$W/N29.elf"
# T1 : outil absent (objdump masqué) : statut 2, jamais un PASS.
mkdir -p "$W/nobin"; for t in readelf nm od; do ln -sf "$(command -v $t)" "$W/nobin/$t"; done
# bash est lancé par son chemin absolu : seul objdump doit manquer (un PATH
# réduit ferait sinon échouer « env bash » lui-même, statut 127).
BASH_BIN="$(command -v bash)"
PATH="$W/nobin:/nonexistent" "$BASH_BIN" "$CHK" "$FPROD" --production > "$W/T1.log" 2>&1; rc=$?
if [ "$rc" -eq 2 ] && grep -q 'outil absent (objdump)' "$W/T1.log" && ! grep -q '^\[PASS\]' "$W/T1.log"; then ok "D/T1: objdump absent : statut 2, aucun PASS"
else ko "D/T1: outil absent mal traite (statut $rc, voir $W/T1.log)"; fi

# M1 à M4 : MÉTA-TESTS du mécanisme must_fail lui-même. Chacun appelle
# must_fail dans un sous-shell sur un cas où il DOIT conclure à l'échec, et
# exige le diagnostic correspondant ; sinon le banc pourrait accepter un
# défaut, un plantage ou une injection ratée sans le signaler.
meta() {   # meta NOM MOTIF_ATTENDU ARGS_must_fail...
    local n="$1" want="$2"; shift 2
    local out; out=$( fail=0; must_fail "$@" ; exit "$fail" ); local r=$?
    if [ "$r" -ne 0 ] && grep -q "^\[FAIL\] x86_64-build D/.*$want" <<<"$out"; then ok "D/$n: must_fail conclut a l'echec ($want)"
    else ko "D/$n: must_fail n'a pas detecte le cas (statut $r : $out)"; fi
}
meta M1_defect_accepted "ACCEPTEE" M1 X7 "X7" "$FPROD" --production true
meta M2_not_injected "NON injecte" M2 X7 "X7" "$FPROD" --production has_rwx "$FPROD"
meta M3_wrong_rule "refuse sans la regle X5" M3 X5 "X5 X7" "$W/N4_rwx_segment.elf" --production has_rwx "$W/N4_rwx_segment.elf"
meta M4_checker_crash "plantage" M4 X7 "X7" "$W/N4_rwx_segment.elf" --bogus-mode has_rwx "$W/N4_rwx_segment.elf"

# ---- E. Fraîcheur et reproductibilité ------------------------------------------
if cmp -s "$PROD" "$FPROD" && cmp -s "$TEST" "$FTEST"; then
    ok "E0: images de build/x86_64 identiques a une construction neuve (aucune image perimee)"
else
    ko "E0: images de build/x86_64 PERIMEES ou differentes d'une construction neuve (make clean x86_64)"
fi
# E2 : contre-test de E0. Une image construite avec d'autres drapeaux (-O0)
# doit DIFFÉRER d'une construction neuve : la comparaison détecte bien une
# image périmée.
STALE="$W/stale"
make -s --no-print-directory X64_BUILD="$STALE" "X64_CFLAGS=$cur -O0" "$STALE/samos64.elf" > "$STALE.log" 2>&1
if [ -f "$STALE/samos64.elf" ] && ! cmp -s "$STALE/samos64.elf" "$FPROD"; then ok "E2: image construite avec d'autres drapeaux detectee comme differente (contre-test de E0)"
else ko "E2: une image perimee ne serait pas detectee (voir $STALE.log)"; fi
if cmp -s "$FPROD" "$FRESH2/samos64.elf" && cmp -s "$FTEST" "$FRESH2/samos64-test.elf"; then
    ok "E1: deux constructions neuves identiques octet pour octet ($(sha256sum "$FPROD" | cut -c1-16)..., $(sha256sum "$FTEST" | cut -c1-16)...), meme machine et meme repertoire source"
else
    ko "E1: deux constructions neuves differentes"
fi

exit $fail
