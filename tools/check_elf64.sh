#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tools/check_elf64.sh                     (K1.1-M64-1)
# -----------------------------------------------------------------------------
# Contrôles STATIQUES du squelette ELF64 x86-64, NON AMORÇABLE. Aucun de ces
# contrôles n'exécute l'image : ils prouvent que la chaîne de construction
# produit ce qui est attendu, pas que du code 64 bits fonctionne.
#
# Usage : check_elf64.sh IMAGE --production | --test
# Sortie : 0 si toutes les règles passent ; 1 sinon (chaque échec est une
# ligne « [FAIL] elf64 Xn: ... » nommée) ; 2 si un outil ou l'image manque.
#
# Règles :
#   X1  classe ELF64, données little-endian
#   X2  machine x86-64
#   X3  type EXEC (exécutable statique, pas DYN ni REL)
#   X4  point d'entrée = symbole _start
#   X5  AUCUN magic Multiboot 1 (0x1BADB002) ni Multiboot 2 (0xE85250D6) aligné
#       sur 4 dans les 32 premiers KiB du fichier : l'image n'est pas présentée
#       comme amorçable (A-1 non tranchée)
#   X6  sections ALLOC = exactement .text .rodata .data .bss ; chacune
#       commence sur 4 KiB ; ordre .text < .rodata < .data < .bss ; .bss NOBITS
#       (règle R4 de la conception)
#   X7  segments : exactement 3 PT_LOAD, droits R E / R / RW dans cet ordre ;
#       aucun W+X ; vaddr = paddr >= 1 MiB ; vaddr aligné sur 4 KiB ;
#       alignement 0x1000 ; deux segments ne partagent aucune page ; ni
#       PT_INTERP ni PT_DYNAMIC ; PT_GNU_STACK éventuel non exécutable (R5)
#   X8  aucun symbole indéfini ; aucune section de relogement ni dynamique
#       (.rela*, .rel*, .dynamic, .got, .plt, .interp).
#       LIMITE : GNU ld résout un symbole indéfini FAIBLE (ou forcé par
#       --unresolved-symbols=ignore-all) à 0 et le retire de la table des
#       symboles d'un exécutable statique ; X8 ne peut pas le voir dans
#       l'image. Ce cas est contrôlé sur les OBJETS d'entrée et sur les
#       options de ld par tests/tools/x86_64_build_check.sh (B'/L0-L2, A/F7).
#   X9  __kernel_start = 0x100000 (hypothèse locale M64-1) et
#       __kernel_end <= 4 MiB
#   X10 séparation production/test : --production refuse tout symbole
#       « ktest_* » ou « test_* » ; --test exige ktest_m64_skeleton_marker
#   X11 périmètre M64-1, sur le code désassemblé (.text) : chaque MNÉMONIQUE
#       et chaque PRÉFIXE est comparé à une liste d'interdits, et chaque
#       opérande à une liste de registres interdits :
#         - transition ou configuration du processeur : RDMSR/WRMSR, LGDT,
#           LIDT, LLDT, LTR, LMSW et CLTS (modifient CR0), INVLPG, INVPCID,
#           INVD/WBINVD, IRET (toutes tailles), SYSCALL, SYSRET, SYSENTER,
#           SYSEXIT, SWAPGS, WRFSBASE/WRGSBASE, XSETBV ;
#         - x87 (toute mnémonique « f… », sauf le préfixe de segment « fs »),
#           MMX (EMMS, FEMMS), état SSE/AVX (LDMXCSR, STMXCSR, FXSAVE,
#           FXRSTOR, XSAVE*, XRSTOR*), toute mnémonique VEX/EVEX « v… » ;
#         - registres %crN, %drN/%dbN, %trN, %st, %mmN, %xmm, %ymm, %zmm.
#       Le désassemblage doit réussir et contenir au moins une instruction ;
#       un octet non décodable « (bad) » est refusé.
#       Révision M64-1-AUDIT : la première version ne cherchait que quelques
#       mnémoniques et noms de registres ; x87 sans opérande (FNINIT, FLDZ),
#       EMMS, LDMXCSR, FXSAVE, VZEROUPPER, LMSW, CLTS, INVLPG, IRETD et les
#       registres de débogage passaient (faux PASS constatés).
#   X12 aucune section ALLOC à adresse nulle ni au-delà de 2 GiB
#       (cohérence avec -mcmodel=small, hypothèse locale)
#   X13 correspondance sections → segments exacte : PT_LOAD n°1 = .text,
#       n°2 = .rodata, n°3 = .data .bss ; aucun autre segment ne contient de
#       section ; drapeaux de section exacts (.text AX, .rodata A, .data et
#       .bss WA). Sans X13, .rodata placée dans le segment RW (constantes
#       inscriptibles) n'était refusée qu'indirectement (ajout M64-1-AUDIT).
# =============================================================================
set -u
ELF="${1:?usage: check_elf64.sh IMAGE --production|--test}"
MODE="${2:?usage: check_elf64.sh IMAGE --production|--test}"
case "$MODE" in --production|--test) ;; *) echo "[FAIL] elf64: mode inconnu $MODE"; exit 2 ;; esac
for t in readelf nm objdump od; do
    command -v "$t" >/dev/null 2>&1 || { echo "[FAIL] elf64: outil absent ($t)"; exit 2; }
done
[ -f "$ELF" ] || { echo "[FAIL] elf64: image absente : $ELF"; exit 2; }

fail=0
pass() { echo "[PASS] elf64 $1"; }
bad()  { echo "[FAIL] elf64 $1"; fail=1; }

hdr="$(readelf -hW "$ELF" 2>/dev/null)" || { echo "[FAIL] elf64 X1: fichier non ELF ($ELF)"; exit 1; }

# ---- X1 à X4 : en-tête ----------------------------------------------------
grep -qE '^\s*Class:\s*ELF64' <<<"$hdr" && grep -qE "^\s*Data:.*little endian" <<<"$hdr" \
    && pass "X1: ELF64 little-endian" || bad "X1: classe ou boutisme incorrects"
grep -qE '^\s*Machine:\s*Advanced Micro Devices X86-64' <<<"$hdr" \
    && pass "X2: machine x86-64" || bad "X2: machine != x86-64"
grep -qE '^\s*Type:\s*EXEC' <<<"$hdr" && pass "X3: type EXEC" || bad "X3: type != EXEC"
entry=$(awk '/Entry point address:/ {print $4}' <<<"$hdr")
start=$(nm "$ELF" 2>/dev/null | awk '$3=="_start" {print "0x" $1}')
if [ -n "$start" ] && [ $((entry)) -eq $((start)) ]; then
    pass "X4: point d'entree = _start ($entry)"
else
    bad "X4: point d'entree ($entry) != _start (${start:-absent})"
fi

# ---- X5 : aucun en-tête Multiboot (image non revendiquée amorçable) -------
mb=$(od -An -v -tx4 -N 32768 "$ELF" | tr -s ' ' '\n' | grep -ciE '^(1badb002|e85250d6)$')
[ "$mb" -eq 0 ] && pass "X5: aucun magic Multiboot 1/2 (image non amorcable)" \
                || bad "X5: magic Multiboot present ($mb) : mecanisme de chargement avant A-1"

# ---- X6 : sections ALLOC --------------------------------------------------
# readelf -SW, après le numéro : Name Type Address Off Size ES Flg Lk Inf Al
# ($7 = Flg ; vide pour les sections sans drapeau, d'où un décalage sans
# effet : Lk est numérique et ne contient jamais « A »).
secs=$(readelf -SW "$ELF" | sed -n 's/^ *\[ *[0-9]*\] //p' | awk '$1 != "" && $7 ~ /A/ {print $1, $2, $3, $5, $7}')
names=$(awk '{print $1}' <<<"$secs" | tr '\n' ' ')
if [ "$names" = ".text .rodata .data .bss " ]; then
    pass "X6: sections ALLOC = .text .rodata .data .bss"
else
    bad "X6: sections ALLOC inattendues : $names"
fi
prev=-1; order_ok=1; align_ok=1
while read -r n typ addr size flg; do
    [ -n "$n" ] || continue
    a=$((0x$addr))
    [ $((a % 4096)) -eq 0 ] || { align_ok=0; bad "X6: $n non alignee sur 4 KiB (0x$addr)"; }
    [ "$a" -gt "$prev" ] || order_ok=0
    prev=$a
    if [ "$n" = ".bss" ] && [ "$typ" != NOBITS ]; then bad "X6: .bss n'est pas NOBITS"; fi
done <<<"$secs"
[ "$align_ok" -eq 1 ] && pass "X6: debut de chaque section ALLOC aligne sur 4 KiB"
[ "$order_ok" -eq 1 ] && pass "X6: ordre croissant des sections ALLOC" || bad "X6: ordre des sections incorrect"

# ---- X7 : segments ----------------------------------------------------------
ph=$(readelf -lW "$ELF")
# LOAD Offset VirtAddr PhysAddr FileSiz MemSiz Flg(1 ou 2 mots) Align
loads=$(awk '$1=="LOAD" {fl=""; for (i=7; i<NF; i++) fl = fl $i; print $3, $4, $6, fl, $NF}' <<<"$ph")
nload=$(grep -c . <<<"$loads")
flags_seq=$(awk '{print $4}' <<<"$loads" | tr '\n' ' ')
[ "$nload" -eq 3 ] && [ "$flags_seq" = "RE R RW " ] \
    && pass "X7: 3 PT_LOAD, droits R E / R / RW" || bad "X7: segments PT_LOAD inattendus ($nload : $flags_seq)"
seg_ok=1; prev_end=0
while read -r va pa msz fl al; do
    [ -n "$va" ] || continue
    v=$((va)); p=$((pa)); m=$((msz))
    case "$fl" in *W*E*|*E*W*) seg_ok=0; bad "X7: segment $va W+X ($fl)" ;; esac
    [ "$v" -eq "$p" ] || { seg_ok=0; bad "X7: segment $va : vaddr != paddr ($pa)"; }
    [ "$v" -ge $((0x100000)) ] || { seg_ok=0; bad "X7: segment $va sous 1 MiB"; }
    [ $((v % 4096)) -eq 0 ] || { seg_ok=0; bad "X7: segment $va non aligne sur 4 KiB"; }
    [ "$al" = "0x1000" ] || { seg_ok=0; bad "X7: segment $va alignement $al != 0x1000"; }
    # Page partagée : le segment commence dans la dernière page du précédent.
    [ "$v" -ge "$prev_end" ] || { seg_ok=0; bad "X7: segment $va partage une page avec le precedent (fin arrondie 0x$(printf %x "$prev_end"))"; }
    prev_end=$(( (v + m + 4095) / 4096 * 4096 ))
done <<<"$loads"
[ "$seg_ok" -eq 1 ] && pass "X7: aucun W+X, vaddr=paddr >= 1 MiB, alignes, aucune page partagee"
if grep -qE '^\s*(INTERP|DYNAMIC)\s' <<<"$ph"; then bad "X7: segment INTERP ou DYNAMIC present"; else pass "X7: ni INTERP ni DYNAMIC"; fi
if awk '$1=="GNU_STACK"' <<<"$ph" | grep -q 'E'; then bad "X7: pile GNU_STACK executable"; else pass "X7: pile non executable"; fi

# ---- X8 : symboles indéfinis, relogements, dynamique ----------------------
und=$(nm -u "$ELF" 2>/dev/null | grep -c .)
[ "$und" -eq 0 ] && pass "X8: aucun symbole indefini" || bad "X8: $und symbole(s) indefini(s) : $(nm -u "$ELF" | tr '\n' ' ')"
dyn=$(readelf -SW "$ELF" | sed -n 's/^ *\[ *[0-9]*\] //p' | awk '{print $1}' | grep -cE '^\.(rela?\b|rela?\.|dynamic|dynsym|dynstr|got|plt|interp)')
[ "$dyn" -eq 0 ] && pass "X8: aucune section de relogement ni dynamique" || bad "X8: $dyn section(s) de relogement ou dynamique(s)"

# ---- X9 : bornes de l'hypothèse locale ------------------------------------
sym() { nm "$ELF" 2>/dev/null | awk -v s="$1" '$3==s {print "0x" $1}'; }
ks=$(sym __kernel_start); ke=$(sym __kernel_end)
if [ -n "$ks" ] && [ -n "$ke" ] && [ $((ks)) -eq $((0x100000)) ] && [ $((ke)) -le $((0x400000)) ]; then
    pass "X9: __kernel_start = 1 MiB, __kernel_end <= 4 MiB (hypothese locale M64-1)"
else
    bad "X9: bornes de l'image incorrectes (start=${ks:-absent}, end=${ke:-absent})"
fi

# ---- X10 : séparation production/test --------------------------------------
tsyms=$(nm "$ELF" 2>/dev/null | awk '{print $NF}' | grep -E '^(ktest_|test_)' | tr '\n' ' ')
if [ "$MODE" = --production ]; then
    [ -z "$tsyms" ] && pass "X10: aucun symbole de test (production)" || bad "X10: symboles de test en production : $tsyms"
else
    grep -qw ktest_m64_skeleton_marker <<<"$tsyms" && pass "X10: temoin de l'image de test present" \
        || bad "X10: temoin ktest_m64_skeleton_marker absent (image de test ?)"
fi

# ---- X11 : périmètre M64-1 (aucune instruction de transition, aucun SIMD) --
dis_raw=$(objdump -d -M att --no-show-raw-insn "$ELF" 2>&1); drc=$?
# Lignes d'instruction : « adresse:<TAB>texte » ; $2 = texte de l'instruction.
ins=$(awk -F'\t' 'NF >= 2 && $1 ~ /^ *[0-9a-f]+:$/ {print $2}' <<<"$dis_raw")
nins=$(grep -c . <<<"$ins")
if [ "$drc" -ne 0 ] || [ "$nins" -eq 0 ]; then
    bad "X11: desassemblage impossible ou vide (objdump statut $drc, $nins instructions)"
else
    # Mots d'instruction = préfixes et mnémonique : jetons initiaux commençant
    # par une lettre, après retrait des commentaires (#...) et des noms de
    # symboles (<...>). Le premier opérande (%, $, chiffre, parenthèse) arrête
    # la lecture.
    words=$(awk '{ sub(/#.*/, ""); gsub(/<[^>]*>/, ""); for (i = 1; i <= NF; i++) { if ($i ~ /^[a-z]/) print NR ":" $i; else break } }' <<<"$ins")
    fmn=$(grep -E ':(rdmsr|wrmsr|lgdt[lqw]?|lidt[lqw]?|lldt|ltr|lmsw|clts|invlpg[a-z]*|invpcid|invd|wbinvd|iret[lqw]?|syscall|sysret[lq]?|sysenter|sysexit[lq]?|swapgs|wr[fg]sbase|xsetbv|emms|femms|ldmxcsr|stmxcsr|fxsave(64)?|fxrstor(64)?|xsave[a-z0-9]*|xrstor[a-z0-9]*|f[a-z0-9]+|v[a-z0-9]+)$' <<<"$words" | grep -vE ':fs$')
    freg=$(grep -nE '%(cr|dr|db|tr)[0-9]|%(st|mm[0-7]|xmm|ymm|zmm)' <<<"$ins")
    fbad=$(grep -nF '(bad)' <<<"$ins")
    if [ -z "$fmn$freg$fbad" ]; then
        pass "X11: aucune instruction de transition, x87, MMX, SSE ou AVX ($nins instructions)"
    else
        bad "X11: instruction hors perimetre M64-1 : $(printf '%s\n%s\n%s\n' "$fmn" "$freg" "$fbad" | grep . | head -3 | tr '\n' ';')"
    fi
fi

# ---- X12 : adresses compatibles avec -mcmodel=small ------------------------
x12=1
while read -r n typ addr size flg; do
    [ -n "$n" ] || continue
    a=$((0x$addr)); s=$((0x$size))
    { [ "$a" -gt 0 ] && [ $((a + s)) -le $((0x80000000)) ]; } || { x12=0; bad "X12: $n hors de ]0, 2 GiB] (0x$addr+0x$size)"; }
done <<<"$secs"
[ "$x12" -eq 1 ] && pass "X12: sections ALLOC dans ]0, 2 GiB] (-mcmodel=small, hypothese locale)"

# ---- X13 : correspondance sections -> segments, drapeaux de section -------
types=$(awk '/^Program Headers:/ {f = 1; next} f && /^ *$/ {f = 0} f && $1 ~ /^[A-Z_]+$/ && $1 != "Type" {print $1}' <<<"$ph")
mapping=$(awk '/Section to Segment mapping/ {f = 1; next} f && /^ +[0-9][0-9]/ {l = $0; sub(/^ +[0-9][0-9] */, "", l); sub(/ +$/, "", l); print (l == "" ? "-" : l)}' <<<"$ph")
x13=1
if [ "$(grep -c . <<<"$types")" -ne "$(grep -c . <<<"$mapping")" ]; then
    x13=0; bad "X13: table de correspondance illisible ($(grep -c . <<<"$types") segments, $(grep -c . <<<"$mapping") lignes)"
else
    got=$(paste -d'|' <(printf '%s\n' "$types") <(printf '%s\n' "$mapping"))
    loadmap=$(awk -F'|' '$1 == "LOAD" {print $2}' <<<"$got" | tr '\n' '/')
    [ "$loadmap" = ".text/.rodata/.data .bss/" ] || { x13=0; bad "X13: sections des PT_LOAD = $loadmap (attendu .text/.rodata/.data .bss/)"; }
    other=$(awk -F'|' '$1 != "LOAD" && $2 != "-" {print $1 ":" $2}' <<<"$got")
    [ -z "$other" ] || { x13=0; bad "X13: section(s) hors PT_LOAD : $other"; }
fi
while read -r n typ addr size flg; do
    [ -n "$n" ] || continue
    case "$n" in .text) want=AX ;; .rodata) want=A ;; .data|.bss) want=WA ;; *) want="?" ;; esac
    [ "$flg" = "$want" ] || { x13=0; bad "X13: drapeaux de $n = $flg (attendu $want)"; }
done <<<"$secs"
[ "$x13" -eq 1 ] && pass "X13: .text, .rodata, .data .bss dans leurs PT_LOAD ; drapeaux AX / A / WA"

exit $fail
