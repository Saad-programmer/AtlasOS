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
#      T1 à T17 (T2 et suivants : K1.1-M64-2-L1-1) : ÉCHEC FERMÉ du
#      vérificateur. Outil absent, outil en échec (statut non nul, sortie
#      partielle, sortie vide hors contrat), sortie illisible, erreur interne :
#      statut 2 (non nul pour T17), diagnostic nommé, aucun PASS ; chaque panne
#      est prouvée par un marqueur de l'outil factice. T12 : sortie vide
#      LÉGITIME (nm -u) acceptée. M5, M6 : méta-tests de ce mécanisme.
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

# T2 à T17 (K1.1-M64-2-L1-1) : ÉCHEC FERMÉ sur erreur d'outil ou d'analyse.
# Avant L1-1, un « nm -u » en échec donnait X8 PASS et le statut 0 (le statut
# d'un pipeline est celui de sa dernière commande, et 2>/dev/null cachait
# l'erreur) ; od, readelf et nm en échec, ou une sortie illisible, rendaient
# de même X5, X7, X8, X10 ou X12 PASS. Chaque cas place UN outil factice en
# tête du PATH (les autres outils restent les vrais). L'outil factice écrit un
# MARQUEUR quand il injecte sa panne : sans marqueur, l'injection n'a pas eu
# lieu et le cas échoue, car il ne prouverait rien. Attendu : statut 2, le
# message nommé du vérificateur, et AUCUNE ligne [PASS] (aucune règle n'est
# évaluée après une erreur d'outil ou d'analyse).
FK="$W/fake"; rm -rf "$FK"; mkdir -p "$FK"
REAL_NM=$(command -v nm); REAL_OD=$(command -v od); REAL_READELF=$(command -v readelf); REAL_OBJDUMP=$(command -v objdump)
# fake CAS OUTIL : crée $FK/CAS/OUTIL à partir du corps lu sur l'entrée
# standard (script sh ; MARK désigne le fichier marqueur, REAL l'outil réel).
fake() {
    local d="$FK/$1"; mkdir -p "$d"
    { printf '#!/bin/sh\nMARK=%s\nREAL=%s\n' "$W/$1.marker" "$(command -v "$2")"; cat; } > "$d/$2"
    chmod +x "$d/$2"; rm -f "$W/$1.marker"
}
# must_error CAS MOTIF IMAGE MODE : vérificateur lancé avec l'outil factice du
# CAS ; exige marqueur présent, statut 2, MOTIF dans le journal, aucun PASS.
must_error() {
    local n="$1" want="$2" img="$3" mode="$4"
    PATH="$FK/$n:$PATH" "$CHK" "$img" "$mode" > "$W/$n.log" 2>&1; local rc=$?
    local npass; npass=$(grep -c '^\[PASS\]' "$W/$n.log")
    # L1-1-R1 : un compteur non numérique (journal illisible) ferait échouer
    # « [ "$npass" -ne 0 ] » avec le statut 2, donc conclure au succès dans la
    # dernière branche : il est refusé explicitement.
    if ! [[ $npass =~ ^[0-9]+$ ]]; then ko "D/$n: compteur de PASS illisible (« $npass »)"
    elif [ ! -s "$W/$n.marker" ]; then ko "D/$n: panne NON injectee (aucun marqueur : l'outil factice n'a pas agi)"
    elif [ "$rc" -eq 0 ]; then ko "D/$n: erreur d'outil ACCEPTEE (statut 0, $npass PASS)"
    elif [ "$rc" -ne 2 ]; then ko "D/$n: statut $rc au lieu de 2 (voir $W/$n.log)"
    elif ! grep -qE "$want" "$W/$n.log"; then ko "D/$n: statut 2 sans le diagnostic attendu ($want)"
    elif [ "$npass" -ne 0 ]; then ko "D/$n: $npass ligne(s) PASS emise(s) malgre l'erreur"
    else ok "D/$n: panne injectee (marqueur) puis refus nomme, statut 2, aucun PASS"; fi
}
# T2 : nm -u en échec silencieux (le défaut d'origine), production puis test.
for n in T2_nm_u_fails T2b_nm_u_fails_test; do fake "$n" nm <<'EOF'
for a in "$@"; do [ "$a" = "-u" ] && { echo "$*" >> "$MARK"; echo "nm: panne simulee" >&2; exit 1; }; done
exec "$REAL" "$@"
EOF
done
must_error T2_nm_u_fails "erreur d'outil : nm -u .*\(statut 1\)" "$FPROD" --production
must_error T2b_nm_u_fails_test "erreur d'outil : nm -u .*\(statut 1\)" "$FTEST" --test
# T3 : nm (liste complète) : sortie PARTIELLE puis statut non nul (X4, X9 et
# X10 en dépendaient ; en production, X10 passait sur une liste vide).
fake T3_nm_partial nm <<'EOF'
case " $* " in *" -u "*) exec "$REAL" "$@" ;; esac
echo "$*" >> "$MARK"; "$REAL" "$@" | head -n 2; exit 1
EOF
must_error T3_nm_partial "erreur d'outil : nm .*samos64\.elf \(statut 1\)" "$FPROD" --production
# T4 : od : sortie partielle puis statut 1 (X5 passait : aucun magic dans la partie lue).
fake T4_od_partial od <<'EOF'
echo "$*" >> "$MARK"; "$REAL" "$@" | head -n 4; exit 1
EOF
must_error T4_od_partial "erreur d'outil : od .*\(statut 1\)" "$FPROD" --production
# T5 : readelf -S en échec au milieu de sa sortie (l'ancien code le lisait dans
# un pipeline « readelf | sed | awk » : X8 « dynamique » et X12 passaient).
fake T5_readelf_S_partial readelf <<'EOF'
case " $* " in *" -SW "*) echo "$*" >> "$MARK"; "$REAL" "$@" | head -n 6; exit 1 ;; esac
exec "$REAL" "$@"
EOF
must_error T5_readelf_S_partial "erreur d'outil : readelf -SW .*\(statut 1\)" "$FPROD" --production
# T6 : readelf -l en échec (X7 INTERP/GNU_STACK passaient sur une sortie vide).
fake T6_readelf_l_fails readelf <<'EOF'
case " $* " in *" -lW "*) echo "$*" >> "$MARK"; exit 3 ;; esac
exec "$REAL" "$@"
EOF
must_error T6_readelf_l_fails "erreur d'outil : readelf -lW .*\(statut 3\)" "$FPROD" --production
# T7 : objdump : sortie partielle puis statut 1 (désormais erreur d'outil, statut 2).
fake T7_objdump_partial objdump <<'EOF'
echo "$*" >> "$MARK"; "$REAL" "$@" | head -n 20; exit 1
EOF
must_error T7_objdump_partial "erreur d'outil : objdump .*\(statut 1\)" "$FPROD" --production
# T8 : od réussit mais ne produit RIEN : contrat « sortie non vide » violé.
fake T8_od_empty od <<'EOF'
echo "$*" >> "$MARK"; exit 0
EOF
must_error T8_od_empty "erreur d'outil : od .*sortie vide" "$FPROD" --production
# T9 : erreurs INTERNES d'analyse (sortie d'outil illisible, statut 0) : sans
# L1-1, bash abandonnait la boucle de X6, X7 ou X12 et la règle passait.
fake T9_readelf_S_garbage readelf <<'EOF'
case " $* " in *" -SW "*) echo "$*" >> "$MARK"; "$REAL" "$@" | sed 's/\(\] \.rodata *PROGBITS *\)[0-9a-f]\{16\}/\1zzzzzzzzzzzzzzzz/'; exit 0 ;; esac
exec "$REAL" "$@"
EOF
must_error T9_readelf_S_garbage "erreur interne d'analyse : section \.rodata" "$FPROD" --production
fake T10_readelf_l_garbage readelf <<'EOF'
case " $* " in *" -lW "*) echo "$*" >> "$MARK"; "$REAL" "$@" | sed '0,/^  LOAD  *0x[0-9a-f]* 0x[0-9a-f]*/s//  LOAD           0x001000 0xZZZZ/'; exit 0 ;; esac
exec "$REAL" "$@"
EOF
must_error T10_readelf_l_garbage "erreur interne d'analyse : segment PT_LOAD" "$FPROD" --production
fake T11_nm_garbage nm <<'EOF'
case " $* " in *" -u "*) exec "$REAL" "$@" ;; esac
echo "$*" >> "$MARK"; "$REAL" "$@" | sed 's/^[0-9a-f]\{16\} T _start$/zzzzzzzzzzzzzzzz T _start/'; exit 0
EOF
must_error T11_nm_garbage "erreur interne d'analyse : valeur de symbole" "$FPROD" --production
# T12 : sortie vide LÉGITIME. Sonde indépendante : nm -u réussit (statut 0)
# sans aucune ligne sur l'image de production ; le vérificateur doit alors
# passer X8 (contrat « any » : le vide est le cas normal, pas une erreur).
nmu=$(nm -u "$FPROD"); nrc=$?
if [ "$nrc" -ne 0 ] || [ -n "$nmu" ]; then ko "D/T12: sonde : nm -u en echec ou non vide (statut $nrc) : cas legitime non etabli"
elif "$CHK" "$FPROD" --production > "$W/T12.log" 2>&1 && grep -q '^\[PASS\] elf64 X8: aucun symbole indefini' "$W/T12.log"; then ok "D/T12: nm -u vide (statut 0) : X8 PASS, statut 0"
else ko "D/T12: sortie vide legitime refusee (voir $W/T12.log)"; fi
# T13 à T15, T18 : outils absents (statut 2, outil nommé, aucun PASS), comme
# T1. Depuis L1-1-R1, le vérificateur n'exécute QUE readelf, nm, objdump, od
# et stat (T18 : stat, qui donne la taille du fichier aux contrôles de
# structure et de complétude).
CHK_TOOLS="readelf nm objdump od stat"
for spec in "T13_nm_absent|nm" "T14_od_absent|od" "T15_readelf_absent|readelf" "T18_stat_absent|stat"; do
    IFS='|' read -r n miss <<<"$spec"
    d="$W/nobin_$n"; rm -rf "$d"; mkdir -p "$d"
    for t in $CHK_TOOLS; do [ "$t" = "$miss" ] || ln -sf "$(command -v "$t")" "$d/$t"; done
    PATH="$d:/nonexistent" "$BASH_BIN" "$CHK" "$FPROD" --production > "$W/$n.log" 2>&1; rc=$?
    if [ "$rc" -eq 2 ] && grep -q "outil absent ($miss)" "$W/$n.log" && ! grep -q '^\[PASS\]' "$W/$n.log"; then ok "D/$n: $miss absent : statut 2, aucun PASS"
    else ko "D/$n: outil absent mal traite (statut $rc, voir $W/$n.log)"; fi
done
# T17 : erreur interne du vérificateur lui-même (bash). Le vérificateur est
# copié et une variable non définie y est introduite (set -u). Révision
# L1-1-R1 : la garde de terminaison doit donner le statut 2 et son diagnostic
# (avant elle, set -u sortait avec le statut 1, confondu avec un refus).
sed 's/^fail=0$/fail=0; : "$__l1_1_variable_non_definie"/' "$CHK" > "$W/T17_check_elf64.sh"
if ! grep -q '__l1_1_variable_non_definie' "$W/T17_check_elf64.sh"; then ko "D/T17: erreur interne NON injectee"
else "$BASH_BIN" "$W/T17_check_elf64.sh" "$FPROD" --production > "$W/T17.log" 2>&1; rc=$?
    if [ "$rc" -eq 2 ] && grep -q 'terminaison imprevue' "$W/T17.log" && ! grep -q '^\[PASS\]' "$W/T17.log"; then ok "D/T17: erreur interne du verificateur : statut 2, diagnostic nomme, aucun PASS"
    else ko "D/T17: erreur interne du verificateur masquee (statut $rc)"; fi
fi
# ---- T16, T19 à T40 (K1.1-M64-2-L1-1-R1) : constats B1, B2, M1, M2, M3 de
# l'audit contradictoire de L1-1. Chaque cas est QUALIFIÉ hors dépôt : il
# échoue contre le candidat 6a35a82 et contre un vérificateur dont le
# mécanisme de détection correspondant est désactivé (rapport L1-1-R1).
# same_verdict CAS REPERTOIRE_PATH IMAGE MODE [MARQUEUR] : le vérificateur
# lancé avec le PATH réduit à REPERTOIRE_PATH (plus /nonexistent) donne le
# MÊME statut et les MÊMES lignes PASS/FAIL qu'avec les vrais outils ; si
# MARQUEUR est donné, il doit rester ABSENT (l'outil factice n'a pas été
# appelé).
same_verdict() {
    local n="$1" d="$2" img="$3" mode="$4" mk="${5:-}" rr rc
    "$BASH_BIN" "$CHK" "$img" "$mode" > "$W/$n.ref.log" 2>&1; rr=$?
    PATH="$d:/nonexistent" "$BASH_BIN" "$CHK" "$img" "$mode" > "$W/$n.log" 2>&1; rc=$?
    if [ "$rr" -eq 2 ]; then ko "D/$n: reference en erreur (statut 2, voir $W/$n.ref.log)"
    elif [ "$rc" -ne "$rr" ]; then ko "D/$n: statut $rc au lieu de $rr (voir $W/$n.log)"
    elif ! cmp -s <(grep -E '^\[(PASS|FAIL)\]' "$W/$n.ref.log") <(grep -E '^\[(PASS|FAIL)\]' "$W/$n.log"); then ko "D/$n: verdict different de la reference (voir $W/$n.log)"
    elif [ -n "$mk" ] && [ -e "$mk" ]; then ko "D/$n: utilitaire de texte appele par le verificateur ($mk)"
    else ok "D/$n: verdict identique a la reference (statut $rc$( [ -n "$mk" ] && echo ', utilitaires factices jamais appeles'))"; fi
}
# T16 (révisé) : AUCUN utilitaire de texte dans le PATH (awk, sed, grep, tr,
# paste, head) : seuls les cinq outils d'analyse. Constat B1 : l'analyse ne
# dépend plus d'aucun utilitaire dont le statut pourrait être perdu.
d="$W/nobin_T16"; rm -rf "$d"; mkdir -p "$d"
for t in $CHK_TOOLS; do ln -sf "$(command -v "$t")" "$d/$t"; done
same_verdict T16_text_utils_absent_prod "$d" "$FPROD" --production
same_verdict T16b_text_utils_absent_N7 "$d" "$W/N7_multiboot_magic.elf" --production
same_verdict T16c_text_utils_absent_N9 "$W/nobin_T16" "$W/N9_wrmsr.elf" --production
# T19 : utilitaires de texte EN PANNE (statut 2, marqueur) en tête du PATH.
# Sur 6a35a82, « tr -s » ou « grep » en panne rendaient X5, X10 ou X11 PASS
# (statut 0) sur des images défectueuses (audit, I1 à I4).
d="$W/broken_T19"; rm -rf "$d"; mkdir -p "$d"
for t in $CHK_TOOLS; do ln -sf "$(command -v "$t")" "$d/$t"; done
for t in awk sed grep tr paste head; do
    printf '#!/bin/sh\necho "%s $*" >> %s\necho "%s: panne simulee" >&2\nexit 2\n' "$t" "$W/T19.marker" "$t" > "$d/$t"; chmod +x "$d/$t"
done
rm -f "$W/T19.marker"
same_verdict T19_text_utils_broken_N7 "$d" "$W/N7_multiboot_magic.elf" --production "$W/T19.marker"
same_verdict T19b_text_utils_broken_N9 "$d" "$W/N9_wrmsr.elf" --production "$W/T19.marker"
same_verdict T19c_text_utils_broken_testimg "$d" "$FTEST" --production "$W/T19.marker"
same_verdict T19d_text_utils_broken_prod "$d" "$FPROD" --production "$W/T19.marker"
# rfake CAS OPTION SED : readelf factice qui réécrit la sortie de « readelf
# OPTION » par l'expression SED, statut 0 (sortie falsifiée mais « réussie »).
rfake() { fake "$1" readelf <<EOF
case " \$* " in *" $2 "*) echo "\$*" >> "\$MARK"; "\$REAL" "\$@" | sed '$3'; exit 0 ;; esac
exec "\$REAL" "\$@"
EOF
}
# T20 à T29 : CONTRAT NUMÉRIQUE par origine de champ (constats B2, M3).
# Avant R1 : préfixe 0x accepté sur une section (« 0x0x… » abandonnait la
# boucle de X6 : PASS), segment sans préfixe lu en OCTAL ou rejeté (chiffre
# 8), 17 chiffres acceptés, colonne manquante non vue, a + s débordant 2^63.
rfake T20_sec_addr_0x -SW 's/\(\] \.rodata *PROGBITS *\)[0-9a-f]\{16\}/\10x0000000000101010/'
must_error T20_sec_addr_0x "erreur interne d'analyse : section \.rodata : adresse .*hors contrat" "$FPROD" --production
rfake T21_load_noprefix_digit8 -lW '0,/^  LOAD  *\(0x[0-9a-f]*\) 0x[0-9a-f]* 0x[0-9a-f]*/s//  LOAD           \1 0000000000180000 0000000000180000/'
must_error T21_load_noprefix_digit8 "erreur interne d'analyse : segment PT_LOAD .*VirtAddr .*hors contrat" "$FPROD" --production
rfake T22_load_noprefix_octal -lW '0,/^  LOAD  *\(0x[0-9a-f]*\) 0x\([0-9a-f]*\) 0x\([0-9a-f]*\)/s//  LOAD           \1 \2 \3/'
must_error T22_load_noprefix_octal "erreur interne d'analyse : segment PT_LOAD .*VirtAddr .*hors contrat" "$FPROD" --production
rfake T23_load_17_digits -lW '0,/^  LOAD  *\(0x[0-9a-f]*\) 0x[0-9a-f]* 0x[0-9a-f]*/s//  LOAD           \1 0x10000000000100000 0x10000000000100000/'
must_error T23_load_17_digits "erreur interne d'analyse : segment PT_LOAD .*VirtAddr .*hors contrat" "$FPROD" --production
rfake T24_sec_size_missing -SW 's/\(\] \.rodata *PROGBITS *[0-9a-f]\{16\} [0-9a-f]\{6\}\) [0-9a-f]\{6\}/\1/'
must_error T24_sec_size_missing "erreur interne d'analyse : section \.rodata : " "$FPROD" --production
rfake T25_sec_beyond_domain -SW 's/\(\] \.bss *NOBITS *\)[0-9a-f]\{16\} \([0-9a-f]\{6\}\) [0-9a-f]\{6\}/\17ffffffffffff000 \2 002000/'
must_error T25_sec_beyond_domain "erreur interne d'analyse : section \.bss : adresse .*hors du domaine" "$FPROD" --production
fake T26_nm_value_0x nm <<'EOF'
case " $* " in *" -u "*) exec "$REAL" "$@" ;; esac
echo "$*" >> "$MARK"; "$REAL" "$@" | sed 's/^\([0-9a-f]\{16\}\) T _start$/0x\1 T _start/'; exit 0
EOF
must_error T26_nm_value_0x "erreur interne d'analyse : valeur de symbole _start .*hors contrat" "$FPROD" --production
rfake T27_entry_noprefix -hW 's/^\(  Entry point address: *\)0x/\1/'
must_error T27_entry_noprefix "erreur interne d'analyse : point d'entree .*hors contrat" "$FPROD" --production
rfake T28_dec_leading_zero -hW 's/^\(  Number of program headers: *\)3$/\103/'
must_error T28_dec_leading_zero "erreur interne d'analyse : readelf -h : Number of program headers : « 03 » hors contrat" "$FPROD" --production
rfake T29_hdr_value_empty -hW 's/^\(  Start of section headers: *\)[0-9]* /\1/'
must_error T29_hdr_value_empty "erreur interne d'analyse : readelf -h : « Start of section headers » illisible" "$FPROD" --production
# T30 à T36 : COMPLÉTUDE (constat M1) : sortie TRONQUÉE mais statut 0. Avant
# R1 : od tronqué cachait le magic (X5 PASS), nm sans les symboles de test
# rendait X10 PASS en production, objdump tronqué cachait WRMSR (X11 PASS).
tfake() {   # tfake CAS OUTIL OPTION FILTRE : OUTIL OPTION ... | FILTRE, statut 0
    fake "$1" "$2" <<EOF
case " \$* " in *" $3 "*) echo "\$*" >> "\$MARK"; "\$REAL" "\$@" | $4; exit 0 ;; esac
exec "\$REAL" "\$@"
EOF
}
tfake T30_od_truncated od -tx4 "head -n 4"
must_error T30_od_truncated "erreur interne d'analyse : od : ligne de fin absente" "$W/N7_multiboot_magic.elf" --production
fake T31_nm_without_test_syms nm <<'EOF'
case " $* " in *" -u "*) exec "$REAL" "$@" ;; esac
echo "$*" >> "$MARK"; "$REAL" "$@" | grep -v 'test_'; exit 0
EOF
must_error T31_nm_without_test_syms "erreur interne d'analyse : nm et readelf -sW divergent" "$FTEST" --production
tfake T32_objdump_truncated objdump -d "head -n 10"
must_error T32_objdump_truncated "erreur interne d'analyse : objdump : section \.text couverte" "$W/N9_wrmsr.elf" --production
tfake T33_readelf_S_truncated readelf -SW "head -n 8"
must_error T33_readelf_S_truncated "erreur interne d'analyse : readelf -SW : section [0-9]+ absente" "$FPROD" --production
rfake T34_readelf_l_no_mapping -lW '/Section to Segment/,$d'
must_error T34_readelf_l_no_mapping "erreur interne d'analyse : readelf -lW : correspondance sections -> segments absente" "$W/N4_rwx_segment.elf" --production
rfake T35_readelf_s_truncated -sW '10,$d'
must_error T35_readelf_s_truncated "erreur interne d'analyse : readelf -sW : entree [0-9]+ de \.symtab absente" "$FPROD" --production
rfake T36_readelf_h_truncated -hW '10,$d'
must_error T36_readelf_h_truncated "erreur interne d'analyse : readelf -h : champ .* absent" "$FPROD" --production
# T37 à T39 : IMAGES STRUCTURELLEMENT INVALIDES (constat M2) : refus NOMMÉ,
# statut 1, comme avant L1-1, et non une « erreur d'outil » (statut 2).
# Sondes : en-tête lu par readelf -h, taille par stat (indépendantes du
# vérificateur).
hval() { readelf -h "$1" | awk -F: -v k="$2" '$1 ~ k {gsub(/[^0-9]/, "", $2); print $2}'; }
shdr_out_of_file() { local o n e; o=$(hval "$1" "Start of section headers"); n=$(hval "$1" "Number of section headers"); e=$(hval "$1" "Size of section headers")
    [ -n "$o" ] && [ -n "$n" ] && [ -n "$e" ] && [ $((o + n * e)) -gt "$(stat -c %s "$1")" ]; }
no_shdr() { [ "$(hval "$1" "Number of section headers")" = 0 ]; }
put() { printf "$2" | dd of="$1" bs=1 seek="$3" conv=notrunc status=none; }   # put IMAGE OCTETS DECALAGE
head -c 3000 "$FPROD" > "$W/T37_truncated.elf"
must_fail T37_truncated_image X1 "X1" "$W/T37_truncated.elf" --production shdr_out_of_file "$W/T37_truncated.elf"
cp "$FPROD" "$W/T38_bad_shoff.elf"; put "$W/T38_bad_shoff.elf" '\377\377\377\177\000\000\000\000' 40   # e_shoff = 0x7fffffff
must_fail T38_bad_shoff X1 "X1" "$W/T38_bad_shoff.elf" --production shdr_out_of_file "$W/T38_bad_shoff.elf"
cp "$FPROD" "$W/T39_no_shdr.elf"; put "$W/T39_no_shdr.elf" '\000\000\000\000\000\000\000\000' 40; put "$W/T39_no_shdr.elf" '\000\000\000\000' 60   # e_shoff, e_shnum, e_shstrndx = 0
# R2 (M-4) : sans en-têtes de section, X7 (contenu exécutable non couvert par
# du code analysé), X8, X10 et X12 (données absentes) échouent aussi.
must_fail T39_no_section_headers X6 "X4 X6 X7 X8 X9 X10 X11 X12 X13" "$W/T39_no_shdr.elf" --production no_shdr "$W/T39_no_shdr.elf"
# T40 : erreur d'expression rationnelle (statut 2 de [[ =~ ]]) : jamais
# confondue avec une absence de correspondance. Copie du vérificateur dont une
# expression est rendue invalide.
sed "s/rx \"\$t_\" '^\[a-z\]' || break/rx \"\$t_\" '^[a-z' || break/" "$CHK" > "$W/T40_check_elf64.sh"
if ! grep -qF "'^[a-z' || break" "$W/T40_check_elf64.sh"; then ko "D/T40: expression invalide NON injectee"
else "$BASH_BIN" "$W/T40_check_elf64.sh" "$FPROD" --production > "$W/T40.log" 2>&1; rc=$?
    if [ "$rc" -eq 2 ] && grep -q "expression rationnelle invalide" "$W/T40.log" && ! grep -q '^\[PASS\]' "$W/T40.log"; then ok "D/T40: expression rationnelle invalide : statut 2, diagnostic nomme, aucun PASS"
    else ko "D/T40: erreur d'expression rationnelle masquee (statut $rc, voir $W/T40.log)"; fi
fi
# ---- T41 à T77 (K1.1-M64-2-L1-1-R2) : constats B-1, B-2, M-1 à M-4 de l'audit
# R1-AUDIT. Images falsifiées par écriture directe des en-têtes (put64) ; chaque
# défaut est prouvé par une sonde readelf/od indépendante du vérificateur.
put64() { local i b=""; for ((i = 0; i < 8; i++)); do b+=$(printf '\\x%02x' $(( ($3 >> (8 * i)) & 255 ))); done; put "$1" "$b" "$2"; }   # put64 IMAGE DECALAGE VALEUR
putb()  { put "$1" "$(printf '%s' "$3" | sed 's/\(..\)/\\x\1/g')" "$2"; }   # putb IMAGE DECALAGE HEXA
PH=64; PHE=56; SHO=$(hval "$FPROD" "Start of section headers")
ph() { echo $((PH + PHE * $1 + $2)); }   # ph SEGMENT CHAMP (8 p_offset, 16 p_vaddr, 24 p_paddr, 32 p_filesz, 40 p_memsz)
sh() { echo $((SHO + 64 * $1 + $2)); }   # sh SECTION CHAMP (16 sh_addr, 24 sh_offset, 32 sh_size)
mk() { cp "$FPROD" "$W/$1.elf"; }
# Sondes.
load_field() { readelf -lW "$1" | awk -v n="$2" -v f="$3" '$1 == "LOAD" {k++; if (k == n) print $f}'; }   # load_field IMAGE N CHAMP(2 off, 3 va, 4 pa, 5 fs, 6 ms)
sec_field()  { readelf -SW "$1" | sed -n 's/^ *\[ *[0-9]*\] //p' | awk -v s="$2" -v f="$3" '$1 == s {print $f}'; }   # 3 addr, 4 off, 5 size
not_congruent() { local o v; o=$(load_field "$1" 1 2); v=$(load_field "$1" 1 3); [ $(( o % 4096 )) -ne $(( v % 4096 )) ]; }
congruent_but_misplaced() { local o v a so; o=$(load_field "$1" 1 2); v=$(load_field "$1" 1 3); a=0x$(sec_field "$1" .text 3); so=0x$(sec_field "$1" .text 4)
    [ $(( o % 4096 )) -eq $(( v % 4096 )) ] && [ $(( so )) -ne $(( o + a - v )) ]; }
nonzero_at() { [ "$(od -An -v -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n' | tr -d 0)" != "" ]; }   # nonzero_at IMAGE DECALAGE N
seg_beyond_text() { local fs ts; fs=$(load_field "$1" 1 5); ts=0x$(sec_field "$1" .text 5); [ $(( fs )) -gt $(( ts )) ]; }
seg_memsz_gt_filesz() { [ $(( $(load_field "$1" 1 6) )) -gt $(( $(load_field "$1" 1 5) )) ]; }
bss_in_file_part() { local v fs b; v=$(load_field "$1" 3 3); fs=$(load_field "$1" 3 5); b=0x$(sec_field "$1" .bss 3); [ $(( b )) -lt $(( v + fs )) ]; }
paddr_differs() { [ "$(load_field "$1" 1 3)" != "$(load_field "$1" 1 4)" ]; }
two_loads_same_va() { [ "$(load_field "$1" 1 3)" = "$(load_field "$1" 2 3)" ]; }
rodata_outside() { local a; a=0x$(sec_field "$1" .rodata 3); ! readelf -lW "$1" | awk -v a=$((a)) '$1 == "LOAD" && a >= strtonum($3) && a < strtonum($3) + strtonum($6) {f = 1} END {exit !f}'; }
# B-1 : correspondance réelle segment/section.
mk T41_poffset_not_congruent; putb "$W/T41_poffset_not_congruent.elf" $((0xff0)) 0f30ebfe
put64 "$W/T41_poffset_not_congruent.elf" "$(ph 0 8)" $((0xff0)); put64 "$W/T41_poffset_not_congruent.elf" "$(ph 0 32)" $((0x3d)); put64 "$W/T41_poffset_not_congruent.elf" "$(ph 0 40)" $((0x3d))
must_fail T41_poffset_not_congruent X7 "X7 X13" "$W/T41_poffset_not_congruent.elf" --production not_congruent "$W/T41_poffset_not_congruent.elf"
# Congru (0x2000 ≡ 0x100000 mod 0x1000) mais à la mauvaise place : octets de .rodata chargés comme code.
mk T42_poffset_congruent_misplaced; put64 "$W/T42_poffset_congruent_misplaced.elf" "$(ph 0 8)" $((0x2000))
must_fail T42_poffset_congruent_misplaced X13 "X7 X13" "$W/T42_poffset_congruent_misplaced.elf" --production congruent_but_misplaced "$W/T42_poffset_congruent_misplaced.elf"
mk T48_paddr_differs; put64 "$W/T48_paddr_differs.elf" "$(ph 0 24)" $((0x200000))
must_fail T48_paddr_differs X7 "X7" "$W/T48_paddr_differs.elf" --production paddr_differs "$W/T48_paddr_differs.elf"
# .bss ramenée au début du segment RW (0x102000), dans sa partie fichier ; X6
# (ordre strict) échoue aussi.
mk T47_bss_in_file_part; put64 "$W/T47_bss_in_file_part.elf" "$(sh 4 16)" $((0x102000))
must_fail T47_bss_in_file_part X13 "X6 X13" "$W/T47_bss_in_file_part.elf" --production bss_in_file_part "$W/T47_bss_in_file_part.elf"
mk T50_section_in_two_loads; put64 "$W/T50_section_in_two_loads.elf" "$(ph 1 16)" $((0x100000)); put64 "$W/T50_section_in_two_loads.elf" "$(ph 1 24)" $((0x100000))
must_fail T50_section_in_two_loads X13 "X7 X13" "$W/T50_section_in_two_loads.elf" --production two_loads_same_va "$W/T50_section_in_two_loads.elf"
mk T51_section_outside_loads; put64 "$W/T51_section_outside_loads.elf" "$(sh 2 16)" $((0x109000))
must_fail T51_section_outside_loads X13 "X6 X13" "$W/T51_section_outside_loads.elf" --production rodata_outside "$W/T51_section_outside_loads.elf"
# T49 : objdump falsifie UN octet brut (texte intact) : octets analysés != octets
# du fichier alors que la correspondance est exacte : incohérence d'outil (statut 2).
fake T49_objdump_byte_lie objdump <<'EOF'
echo "$*" >> "$MARK"; "$REAL" "$@" | sed '0,/^ *100000:\tfa /s//  100000:\tfb /'; exit 0
EOF
must_error T49_objdump_byte_lie "erreur interne d'analyse : octets de \.text : objdump et od divergent" "$FPROD" --production
# B-2 : couverture exécutable.
mk T43_exec_gap_code; putb "$W/T43_exec_gap_code.elf" $((0x1030)) 0f30ebfe; putb "$W/T43_exec_gap_code.elf" $((0x1000)) eb2e
put64 "$W/T43_exec_gap_code.elf" "$(ph 0 32)" $((0x40)); put64 "$W/T43_exec_gap_code.elf" "$(ph 0 40)" $((0x40))
must_fail T43_exec_gap_code X7 "X7" "$W/T43_exec_gap_code.elf" --production seg_beyond_text "$W/T43_exec_gap_code.elf"
mk T44_exec_trailing_zeros; put64 "$W/T44_exec_trailing_zeros.elf" "$(ph 0 32)" $((0x40)); put64 "$W/T44_exec_trailing_zeros.elf" "$(ph 0 40)" $((0x40))
must_fail T44_exec_trailing_zeros X7 "X7" "$W/T44_exec_trailing_zeros.elf" --production seg_beyond_text "$W/T44_exec_trailing_zeros.elf"
# Code dans la page du segment, HORS de son contenu (FileSiz inchangé) : _start y saute.
mk T45_exec_page_tail_code; putb "$W/T45_exec_page_tail_code.elf" $((0x1030)) 0f30ebfe; putb "$W/T45_exec_page_tail_code.elf" $((0x1000)) eb2e
must_fail T45_exec_page_tail_code X7 "X7" "$W/T45_exec_page_tail_code.elf" --production nonzero_at "$W/T45_exec_page_tail_code.elf" $((0x1030)) 4
mk T46_exec_memsz_gt_filesz; put64 "$W/T46_exec_memsz_gt_filesz.elf" "$(ph 0 40)" $((0x40))
must_fail T46_exec_memsz_gt_filesz X7 "X7" "$W/T46_exec_memsz_gt_filesz.elf" --production seg_memsz_gt_filesz "$W/T46_exec_memsz_gt_filesz.elf"
# M-1 : un test par mécanisme jusque-là non couvert (mutants N01 à N19 ; N20 est
# équivalent : le dépassement de fin est aussi refusé par la fermeture de section).
rfake T52_secS_offset_lie -SW 's/^\(There are [0-9]* section headers, starting at offset \)0x[0-9a-f]*:/\10x10:/'
must_error T52_secS_offset_lie "erreur interne d'analyse : readelf -SW : .* readelf -h annonce" "$FPROD" --production
mk T53_section_content_outside; put64 "$W/T53_section_content_outside.elf" "$(sh 2 24)" $((0x100000))
must_fail T53_section_content_outside X1 "X1" "$W/T53_section_content_outside.elf" --production test "$(( 0x$(sec_field "$W/T53_section_content_outside.elf" .rodata 4) ))" -gt "$(stat -c %s "$FPROD")"
mk T54_segment_content_outside; put64 "$W/T54_segment_content_outside.elf" "$(ph 1 8)" $((0x101000))
must_fail T54_segment_content_outside X1 "X1" "$W/T54_segment_content_outside.elf" --production test "$(( $(load_field "$W/T54_segment_content_outside.elf" 2 2) ))" -gt "$(stat -c %s "$FPROD")"
rfake T55_l_entry_lie -lW 's/^Entry point 0x100000$/Entry point 0x100001/'
must_error T55_l_entry_lie "erreur interne d'analyse : readelf -lW : point d'entree" "$FPROD" --production
rfake T56_secS_extra_line -SW '/^  \[16\]/p'
must_error T56_secS_extra_line "erreur interne d'analyse : readelf -SW : ligne inattendue apres la section" "$FPROD" --production
fake T57_od_word_garbage od <<'EOF'
case " $* " in *" -tx4 "*) echo "$*" >> "$MARK"; "$REAL" "$@" | sed '1s/ 464c457f/ zzzzzzzz/'; exit 0 ;; esac
exec "$REAL" "$@"
EOF
must_error T57_od_word_garbage "erreur interne d'analyse : od : mot « zzzzzzzz »" "$FPROD" --production
fake T58_nm_u_extra nm <<'EOF'
case " $* " in *" -u "*) echo "$*" >> "$MARK"; "$REAL" "$@"; echo "                 U fantome"; exit 0 ;; esac
exec "$REAL" "$@"
EOF
must_error T58_nm_u_extra "erreur interne d'analyse : nm -u et readelf -sW divergent" "$FPROD" --production
fake T59_objdump_extra_section objdump <<'EOF'
echo "$*" >> "$MARK"; "$REAL" "$@"; printf '\nDisassembly of section .rodata:\n'; exit 0
EOF
must_error T59_objdump_extra_section "erreur interne d'analyse : objdump : section \.rodata inattendue" "$FPROD" --production
fake T60_objdump_two_headers objdump <<'EOF'
echo "$*" >> "$MARK"; "$REAL" "$@" | sed '2p'; exit 0
EOF
must_error T60_objdump_two_headers "erreur interne d'analyse : objdump : en-tete « file format » vu 2 fois" "$FPROD" --production
fake T61_objdump_section_dropped objdump <<'EOF'
echo "$*" >> "$MARK"; "$REAL" "$@" | sed '/^Disassembly of section/,$d'; exit 0
EOF
must_error T61_objdump_section_dropped "erreur interne d'analyse : objdump : section executable \.text non desassemblee" "$FPROD" --production
mk T62_undecodable_byte; putb "$W/T62_undecodable_byte.elf" $((0x1000)) 06
must_fail T62_undecodable_byte X11 "X11" "$W/T62_undecodable_byte.elf" --production has_insn "$W/T62_undecodable_byte.elf" '\(bad\)'
fake T63_symtab_count_lie readelf <<'EOF'
case " $* " in *" -sW "*) echo "$*" >> "$MARK"; "$REAL" "$@" | sed -e 's/contains \([0-9]*\) entries/contains NM1 entries/' -e '$d' | awk -v n="$("$REAL" "$@" | sed -n 's/.*contains \([0-9]*\) entries.*/\1/p')" '{sub(/NM1/, n - 1); print}'; exit 0 ;; esac
exec "$REAL" "$@"
EOF
must_error T63_symtab_count_lie "erreur interne d'analyse : readelf -sW : \.symtab annonce [0-9]+ entrees, la section en contient" "$FPROD" --production
fake T64_symtab_absent readelf <<'EOF'
case " $* " in *" -sW "*) echo "$*" >> "$MARK"; exit 0 ;; esac
exec "$REAL" "$@"
EOF
must_error T64_symtab_absent "erreur interne d'analyse : readelf -sW : table \.symtab absente" "$FPROD" --production
rfake T65_l_offset_lie -lW 's/^\(There are [0-9]* program headers, starting at offset \)64$/\165/'
must_error T65_l_offset_lie "erreur interne d'analyse : readelf -lW : .* readelf -h annonce" "$FPROD" --production
rfake T66_mapping_swapped -lW 's/^   00 /   0X /; s/^   01 /   00 /; s/^   0X /   01 /'
must_error T66_mapping_swapped "erreur interne d'analyse : readelf -lW : correspondance numero" "$FPROD" --production
rfake T67_hdr_duplicate_field -hW '/^  Class:/p'
must_error T67_hdr_duplicate_field "erreur interne d'analyse : readelf -h : champ « Class » en double" "$FPROD" --production
rfake T68_hdr_beyond_16bit -hW 's/^\(  Number of section headers: *\)[0-9]*$/\170000/'
must_error T68_hdr_beyond_16bit "erreur interne d'analyse : readelf -h : e_shnum = 70000 \(champ ELF de 16 bits" "$FPROD" --production
rfake T69_flags_garbage -SW 's/\(\] \.text .*\)  AX /\1  AZ /'
must_error T69_flags_garbage "erreur interne d'analyse : section \.text : drapeaux « AZ »" "$FPROD" --production
fake T70_od_extra_word od <<'EOF'
case " $* " in *" -tx4 "*) echo "$*" >> "$MARK"; "$REAL" "$@" | sed '1s/$/ 00000000/'; exit 0 ;; esac
exec "$REAL" "$@"
EOF
must_error T70_od_extra_word "erreur interne d'analyse : od : ligne a 0 : 5 mots au lieu de 4" "$FPROD" --production
# M-2 : od avec décalages. T71 : la ligne du magic remplacée par une copie de la
# précédente (nombre de mots conservé ; faux PASS X5 avant R2) ; T72 : ligne
# substituée par une ligne d'ailleurs (son propre décalage).
fake T71_od_dup_line od <<'EOF'
case " $* " in *" -tx4 "*) echo "$*" >> "$MARK"; "$REAL" "$@" | awk '{ if ($0 ~ /1badb002/) print prev; else print; prev = $0 }'; exit 0 ;; esac
exec "$REAL" "$@"
EOF
must_error T71_od_dup_line "erreur interne d'analyse : od : decalage [0-9]+ au lieu de [0-9]+" "$W/N7_multiboot_magic.elf" --production
fake T72_od_line_substituted od <<'EOF'
case " $* " in *" -tx4 "*) echo "$*" >> "$MARK"; "$REAL" "$@" | awk 'NR == 2 {print l1; next} {if (NR == 1) l1 = $0; print}'; exit 0 ;; esac
exec "$REAL" "$@"
EOF
must_error T72_od_line_substituted "erreur interne d'analyse : od : decalage 0+ au lieu de 16" "$FPROD" --production
# M-3 : refus NOMMÉS pour des défauts de l'image (statut 1, plus « erreur interne »).
mk T74_phentsize_zero; put "$W/T74_phentsize_zero.elf" '\000\000' 54
must_fail T74_phentsize_zero X1 "X1" "$W/T74_phentsize_zero.elf" --production test "$(hval "$W/T74_phentsize_zero.elf" "Size of program headers")" = 0
mk T75_two_exec_text; n1=$(od -An -tu4 -j "$(sh 1 0)" -N 4 "$FPROD" | tr -d ' ')
put "$W/T75_two_exec_text.elf" "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((n1 & 255)) $(((n1 >> 8) & 255)) $(((n1 >> 16) & 255)) $(((n1 >> 24) & 255)))" "$(sh 2 0)"
put64 "$W/T75_two_exec_text.elf" "$(sh 2 8)" 6
must_fail T75_two_exec_text X6 "X6 X11 X13" "$W/T75_two_exec_text.elf" --production test "$(readelf -SW "$W/T75_two_exec_text.elf" | grep -c ' \.text .* AX ')" -eq 2
objcopy --add-symbol _start=.text:0x10,local "$FPROD" "$W/T76_two_start.elf" > "$W/T76.objcopy.log" 2>&1
must_fail T76_ambiguous_start X4 "X4" "$W/T76_two_start.elf" --production test "$(nm "$W/T76_two_start.elf" | awk '$3 == "_start"' | wc -l)" -eq 2
# M-4 : aucune PASS sans les données nécessaires. nopass CAS MOTIFS : aucune ligne
# « [PASS] elf64 MOTIF » (MOTIFS séparés par « | ») dans le journal du cas.
nopass() {
    local n="$1" r bad_="" IFS='|'; for r in $2; do grep -qF "[PASS] elf64 $r" "$W/$n.log" && bad_="$bad_ $r"; done
    if [ ! -s "$W/$n.log" ]; then ko "D/$n/nopass: journal absent"
    elif [ -n "$bad_" ]; then ko "D/$n/nopass: PASS vacant pour$bad_ (donnees absentes)"
    else ok "D/$n/nopass: aucune PASS pour $2 (donnees absentes)"; fi
}
nopass T39_no_section_headers "X6:|X8:|X10:|X12:"
strip -o "$W/T77_test_stripped.elf" "$FTEST"
must_fail T77_stripped_test_in_production X10 "X4 X8 X9 X10" "$W/T77_test_stripped.elf" --production test -z "$(nm "$W/T77_test_stripped.elf" 2>/dev/null)"
nopass T77_stripped_test_in_production "X8: aucun symbole indefini|X10:"
# must_msg CAS MOTIF : le journal du cas contient le diagnostic PRÉCIS du
# mécanisme (plusieurs contrôles de X7 ou X13 se recouvrent : sans ce test, la
# désactivation d'un seul resterait invisible, constaté par mutation en R2).
must_msg() {
    if [ ! -s "$W/$1.log" ]; then ko "D/$1/msg: journal absent"
    elif grep -qE "^\[FAIL\] elf64 $2" "$W/$1.log"; then ok "D/$1/msg: diagnostic « $2 »"
    else ko "D/$1/msg: diagnostic attendu absent (« $2 »)"; fi
}
must_msg T41_poffset_not_congruent "X7: segment 0x0+100000 : p_offset 0xff0 non congru"
must_msg T42_poffset_congruent_misplaced "X7: segments .* : contenus fichier chevauchants"
must_msg T42_poffset_congruent_misplaced "X13: section \.text : decalage fichier 0x1000 different du decalage charge 0x2000"
must_msg T47_bss_in_file_part "X13: section NOBITS \.bss dans la partie fichier"
must_msg T50_section_in_two_loads "X13: section \.text dans plusieurs PT_LOAD"
must_msg T51_section_outside_loads "X13: section \.rodata hors de tout PT_LOAD"
must_msg T43_exec_gap_code "X7: segment executable .* octet\(s\) non controles en fin de contenu"
must_msg T45_exec_page_tail_code "X7: segment executable .* octets non nuls dans ses pages"
# T78 : section NON exécutable dans le segment exécutable. Le PT_LOAD n°1 couvre
# .text ET .rodata (décalages exacts) ; le PT_LOAD n°2 devient PT_NULL.
mk T78_nonexec_in_exec_seg; put64 "$W/T78_nonexec_in_exec_seg.elf" "$(ph 0 32)" $((0x1037)); put64 "$W/T78_nonexec_in_exec_seg.elf" "$(ph 0 40)" $((0x1037)); put "$W/T78_nonexec_in_exec_seg.elf" '\000\000\000\000' "$(ph 1 0)"
rodata_in_exec() { readelf -lW "$1" | sed -n '/Section to Segment/,$p' | grep -qE '^ +00 +\.text \.rodata'; }
must_fail T78_nonexec_in_exec_seg X7 "X7 X13" "$W/T78_nonexec_in_exec_seg.elf" --production rodata_in_exec "$W/T78_nonexec_in_exec_seg.elf"
must_msg T78_nonexec_in_exec_seg "X7: segment executable .* : section non executable \.rodata"
# T79 : DEUX sections exécutables (.rodata marquée AX) dans le segment n°1, et un
# octet NON NUL dans le remplissage qui les sépare (0x1100).
cp "$W/T78_nonexec_in_exec_seg.elf" "$W/T79_exec_gap_nonzero.elf"; put64 "$W/T79_exec_gap_nonzero.elf" "$(sh 2 8)" 6; putb "$W/T79_exec_gap_nonzero.elf" $((0x1100)) cc
must_fail T79_exec_gap_nonzero X7 "X6 X7 X11 X13" "$W/T79_exec_gap_nonzero.elf" --production nonzero_at "$W/T79_exec_gap_nonzero.elf" $((0x1100)) 1
must_msg T79_exec_gap_nonzero "X7: segment executable .* : remplissage non nul entre sections"
# .rodata (symbole OBJECT samos64_build_id) devenue exécutable : objdump en
# affiche les octets en DONNÉES ; ils ne sont pas désassemblés, X11 échoue.
must_msg T79_exec_gap_nonzero "X11: 55 octet\(s\) de section executable affiches en donnees"
# T80 : octets non contrôlés en TÊTE du segment exécutable : PT_LOAD n°1 à
# p_offset 0, p_vaddr 0xff000 (correspondance de .text exacte), donc l'en-tête
# ELF est chargé exécutable devant .text.
mk T80_exec_head_uncontrolled; put64 "$W/T80_exec_head_uncontrolled.elf" "$(ph 0 8)" 0; put64 "$W/T80_exec_head_uncontrolled.elf" "$(ph 0 16)" $((0xff000)); put64 "$W/T80_exec_head_uncontrolled.elf" "$(ph 0 24)" $((0xff000))
put64 "$W/T80_exec_head_uncontrolled.elf" "$(ph 0 32)" $((0x102d)); put64 "$W/T80_exec_head_uncontrolled.elf" "$(ph 0 40)" $((0x102d))
head_before_text() { [ $(( $(load_field "$1" 1 3) )) -lt $(( 0x$(sec_field "$1" .text 3) )) ]; }
must_fail T80_exec_head_uncontrolled X7 "X7" "$W/T80_exec_head_uncontrolled.elf" --production head_before_text "$W/T80_exec_head_uncontrolled.elf"
must_msg T80_exec_head_uncontrolled "X7: segment executable .* octet\(s\) non controles en tete"
# T81 : correspondance CALCULÉE != correspondance de readelf : readelf -l factice
# qui permute les noms des deux premières lignes de correspondance (refus X13 ;
# la contradiction porte sur les données de l'image, aucune n'est retenue).
rfake T81_mapping_names_swapped -lW 's/^   00     \.text $/   00     .rodata /; t; s/^   01     \.rodata $/   01     .text /'
PATH="$FK/T81_mapping_names_swapped:$PATH" "$CHK" "$FPROD" --production > "$W/T81_mapping_names_swapped.log" 2>&1; rc=$?
if [ ! -s "$W/T81_mapping_names_swapped.marker" ]; then ko "D/T81: panne NON injectee"
elif [ "$rc" -ne 1 ]; then ko "D/T81: statut $rc au lieu de 1"
else must_msg T81_mapping_names_swapped "X13: segment 0x0+100000 : sections calculees « \.text », readelf « \.rodata »"; fi
# M5, M6 : MÉTA-TESTS de must_error, sans quoi le banc pourrait accepter une
# injection ratée ou une erreur acceptée sans le signaler.
#   M5 : outil factice dont le déclencheur n'est jamais atteint (aucun marqueur) ;
#   M6 : « vérificateur » de substitution qui appelle nm (l'outil factice
#        injecte, d'où le marqueur) puis sort avec le statut 0, comme un
#        vérificateur qui accepterait l'erreur d'outil.
# must_error doit conclure à l'échec, avec le diagnostic correspondant.
fake M5_no_injection nm <<'EOF'
case " $* " in *" --option-jamais-utilisee "*) echo "$*" >> "$MARK"; exit 1 ;; esac
exec "$REAL" "$@"
EOF
fake M6_checker_accepts nm <<'EOF'
echo "$*" >> "$MARK"; exit 1
EOF
printf '#!/bin/sh\nnm "$1" > /dev/null 2>&1\nexit 0\n' > "$W/M6_accepting_checker.sh"; chmod +x "$W/M6_accepting_checker.sh"
meta_err() {   # meta_err NOM MOTIF_ATTENDU CAS VERIFICATEUR
    local n="$1" want="$2" cas="$3" chk="$4"
    local out; out=$( fail=0; CHK="$chk"; must_error "$cas" "motif-sans-objet" "$FPROD" --production; exit "$fail" ); local r=$?
    if [ "$r" -ne 0 ] && grep -q "^\[FAIL\] x86_64-build D/$cas: .*$want" <<<"$out"; then ok "D/$n: must_error conclut a l'echec ($want)"
    else ko "D/$n: must_error n'a pas detecte le cas (statut $r : $out)"; fi
}
meta_err M5_meta_no_injection "NON injectee" M5_no_injection "$CHK"
meta_err M6_meta_checker_accepts "ACCEPTEE" M6_checker_accepts "$W/M6_accepting_checker.sh"

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
