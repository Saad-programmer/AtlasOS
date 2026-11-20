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
#   D. Tests négatifs N1 à N16 du vérificateur : chaque image falsifiée doit
#      échouer sur la RÈGLE NOMMÉE attendue (« [FAIL] elf64 Xn »), pas sur
#      n'importe quel échec ; une liaison refusée par une assertion R4 compte
#      seulement si le message attendu apparaît.
#   E. Reproductibilité : seconde construction dans un autre répertoire de
#      sortie ; images identiques octet pour octet.
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

# ---- A. Drapeaux ------------------------------------------------------------
REQ_C="--target=x86_64-unknown-none-elf -march=x86-64 -ffreestanding -nostdlibinc -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small -fstack-protector-strong -fcf-protection=none -Werror"
FORBID_C="-fpic -fPIC -fpie -fPIE -mred-zone -m32 -march=i686 --target=i686-unknown-none-elf -mstack-alignment=4 -msse -mavx"
# flags_ok [SURCHARGES make...] : 0 si toutes les commandes x86-64 sont conformes.
flags_ok() {
    local log; log="$(make -n -B x86_64 "$@" 2>&1)" || { echo "    make -n en echec"; return 1; }
    local cc as ld r=0 line
    cc=$(grep -E '^clang .* -o build/x86_64/' <<<"$log"); as=$(grep -E '^nasm .* -o build/x86_64/' <<<"$log")
    ld=$(grep -E '^ld .* -o build/x86_64/' <<<"$log")
    [ "$(grep -c . <<<"$cc")" -ge 2 ] && [ "$(grep -c . <<<"$as")" -ge 2 ] && [ "$(grep -c . <<<"$ld")" -eq 2 ] \
        || { echo "    commandes x86-64 manquantes"; return 1; }
    while IFS= read -r line; do
        for f in $REQ_C; do grep -qF -- " $f " <<<" $line " || { echo "    option requise absente : $f"; r=1; }; done
        for f in $FORBID_C; do grep -qF -- " $f " <<<" $line " && { echo "    option interdite : $f"; r=1; }; done
    done <<<"$cc"
    while IFS= read -r line; do grep -qF -- " -f elf64 " <<<" $line " || { echo "    NASM sans -f elf64"; r=1; }; done <<<"$as"
    while IFS= read -r line; do
        for f in "-m elf_x86_64" "--fatal-warnings" "-T kernel/arch/x86_64/linker.ld" "-nostdlib" "-z noexecstack"; do
            grep -qF -- " $f " <<<" $line " || { echo "    ld sans $f"; r=1; }
        done
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
if flags_ok > "$W/F0.log"; then ok "A/F0: drapeaux x86-64 conformes (C, NASM, ld)"; else ko "A/F0: drapeaux non conformes"; cat "$W/F0.log"; fi
neg_flags() {   # neg_flags NOM MOTIF_ATTENDU SURCHARGE...
    local n="$1" want="$2"; shift 2
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
    done < <(find "$x" -name '*.o' -not -path "$X64/repro/*")
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
if sep_ok "$X64" build/prod build/test > "$W/S0.log"; then ok "B/S0: separation des objets ($(grep -o 'objets.*' "$W/S0.log"))"; else ko "B/S0: contamination croisee"; cat "$W/S0.log"; fi
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
mkdir -p "$W"
P_OBJS=$(find "$X64/prod" -name '*.o' | sort); T_OBJS=$(find "$X64/test" -name '*.o' | sort)
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
if "$CHK" "$PROD" --production > "$W/C0.log"; then ok "C/C0: image de production conforme ($(grep -c '^\[PASS\]' "$W/C0.log") regles)"; else ko "C/C0: production non conforme"; grep FAIL "$W/C0.log"; fi
if "$CHK" "$TEST" --test > "$W/C1.log"; then ok "C/C1: image de test conforme ($(grep -c '^\[PASS\]' "$W/C1.log") regles)"; else ko "C/C1: test non conforme"; grep FAIL "$W/C1.log"; fi

# ---- D. Tests négatifs du vérificateur ---------------------------------------
# must_fail NOM REGLE IMAGE MODE : le vérificateur doit échouer SUR la règle.
must_fail() {
    local n="$1" rule="$2" img="$3" mode="$4"
    if [ ! -f "$img" ]; then ko "D/$n: image falsifiee non produite (voir $W/$n.link.log)"; return; fi
    if "$CHK" "$img" "$mode" > "$W/$n.log" 2>&1; then ko "D/$n: anomalie ACCEPTEE par check_elf64"
    elif grep -q "^\[FAIL\] elf64 $rule" "$W/$n.log"; then ok "D/$n: refuse par $rule"
    else ko "D/$n: refuse sans la regle $rule (voir $W/$n.log)"; fi
}
LDS=kernel/arch/x86_64/linker.ld
POBJ="$(find "$X64/prod" -name '*.o' | sort | tr '\n' ' ')"
LDBASE="-m elf_x86_64 -nostdlib -z max-page-size=0x1000 -z noexecstack --build-id=none"
asm() { printf '%s\n' "$2" > "$W/$1.asm" && nasm -f elf64 -w+all "$W/$1.asm" -o "$W/$1.o"; }
link() { local n="$1" s="$2"; shift 2; ld $LDBASE -T "$s" -o "$W/$n.elf" "$@" > "$W/$n.link.log" 2>&1; }
noassert() { sed '/^ASSERT/d' "$LDS" > "$W/$1.ld"; }

must_fail N1_i386_image X1 build/samos.elf --production
must_fail N2_test_as_production X10 "$TEST" --production
must_fail N3_production_as_test X10 "$PROD" --test
# N4 : segment RWX (PHDRS text FLAGS(7)), lié sans --fatal-warnings.
sed 's/text   PT_LOAD FLAGS(5)/text   PT_LOAD FLAGS(7)/' "$LDS" > "$W/N4.ld"; link N4_rwx_segment "$W/N4.ld" $POBJ
must_fail N4_rwx_segment X7 "$W/N4_rwx_segment.elf" --production
# N5a : .rodata non alignée, script original : l'assertion R4 refuse la liaison.
sed 's/^    \.rodata ALIGN(PAGE_SIZE) :/    .rodata :/' "$LDS" > "$W/N5a.ld"
if link N5a_rodata_unaligned "$W/N5a.ld" $POBJ; then ko "D/N5a: liaison ACCEPTEE malgre .rodata non alignee"
elif grep -q "R4 : .rodata non alignee" "$W/N5a_rodata_unaligned.link.log"; then ok "D/N5a: liaison refusee par l'assertion R4 (.rodata)"
else ko "D/N5a: liaison refusee pour une autre raison (voir $W/N5a_rodata_unaligned.link.log)"; fi
# N5b : même défaut sans assertions : check_elf64 doit le refuser seul.
sed -e 's/^    \.rodata ALIGN(PAGE_SIZE) :/    .rodata :/' -e '/^ASSERT/d' "$LDS" > "$W/N5b.ld"; link N5b_rodata_unaligned "$W/N5b.ld" $POBJ
must_fail N5b_rodata_unaligned X6 "$W/N5b_rodata_unaligned.elf" --production
# N6 : (supprimé de l'image) un symbole indéfini n'est pas visible dans un
# exécutable statique lié par GNU ld : il est contrôlé sur les objets (B'/L1,
# B'/L2) et par l'interdiction des options de ld qui le toléreraient (A/F7).
# N7 : magic Multiboot 1 dans .rodata (mécanisme de chargement avant A-1).
asm mb1 "bits 64
section .rodata
dd 0x1BADB002, 0, 0"
link N7_multiboot_magic "$LDS" $POBJ "$W/mb1.o"
must_fail N7_multiboot_magic X5 "$W/N7_multiboot_magic.elf" --production
# N7b : magic Multiboot 2.
asm mb2 "bits 64
section .rodata
dd 0xE85250D6, 0, 0, 0"
link N7b_multiboot2_magic "$LDS" $POBJ "$W/mb2.o"
must_fail N7b_multiboot2_magic X5 "$W/N7b_multiboot2_magic.elf" --production
# N8 à N12 : instructions hors périmètre M64-1.
i=0
for spec in "N8_write_cr4|mov cr4, rax" "N9_wrmsr|wrmsr" "N10_lgdt|lgdt [rax]" "N11_sse|movaps xmm0, xmm1" "N12_read_cr3|mov rax, cr3"; do
    n="${spec%%|*}" ins="${spec#*|}"; i=$((i + 1))
    asm "ins$i" "bits 64
section .text
global forbidden_$i
forbidden_$i: $ins
    ret"
    link "$n" "$LDS" $POBJ "$W/ins$i.o"
    must_fail "$n" X11 "$W/$n.elf" --production
done
# N13 : section ALLOC inattendue.
asm extra "bits 64
section .samos_extra progbits alloc noexec nowrite
dd 1"
noassert N13; link N13_extra_section "$W/N13.ld" $POBJ "$W/extra.o"
must_fail N13_extra_section X6 "$W/N13_extra_section.elf" --production
# N14 : point d'entrée autre que _start.
link N14_entry "$LDS" $POBJ -e samos64_skeleton_probe
must_fail N14_entry X4 "$W/N14_entry.elf" --production
# N15 : image liée sous 1 MiB (assertions retirées).
sed -e 's/^KERNEL_LOAD_ADDR = 0x00100000;/KERNEL_LOAD_ADDR = 0x00010000;/' -e '/^ASSERT/d' "$LDS" > "$W/N15.ld"; link N15_below_1mib "$W/N15.ld" $POBJ
must_fail N15_below_1mib X7 "$W/N15_below_1mib.elf" --production
# N16 : image partageant une page entre .rodata et .data (ALIGN de .data retiré).
sed -e 's/^    \.data ALIGN(PAGE_SIZE) :/    .data :/' -e '/^ASSERT/d' "$LDS" > "$W/N16.ld"; link N16_shared_page "$W/N16.ld" $POBJ
must_fail N16_shared_page X6 "$W/N16_shared_page.elf" --production

# ---- E. Reproductibilité -------------------------------------------------------
R="$X64/repro"
rm -rf "$R"
if make -s --no-print-directory X64_BUILD="$R" "$R/samos64.elf" "$R/samos64-test.elf" > "$W/E.log" 2>&1 &&
   cmp -s "$PROD" "$R/samos64.elf" && cmp -s "$TEST" "$R/samos64-test.elf"; then
    ok "E: seconde construction ($R) identique octet pour octet ($(sha256sum "$PROD" | cut -c1-16)..., $(sha256sum "$TEST" | cut -c1-16)...)"
else
    ko "E: seconde construction differente ou en echec (voir $W/E.log)"
fi
rm -rf "$R"

exit $fail
