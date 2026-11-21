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
# ligne « [FAIL] elf64 Xn: ... » nommée) ; 2 si un outil ou l'image manque,
# si un outil ÉCHOUE (statut non nul, ou sortie vide là où le contrat de
# l'outil en exige une), ou si une sortie d'outil est illisible (erreur
# interne d'analyse). Dans ces trois derniers cas, aucune règle n'est évaluée
# et aucun PASS n'est émis (révision K1.1-M64-2-L1-1, « échec fermé »).
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
# Outils d'analyse d'abord (l'ordre conserve le diagnostic du test D/T1),
# puis les utilitaires de texte, qui manquaient à la liste (L1-1).
for t in readelf nm objdump od awk sed grep tr paste head; do
    command -v "$t" >/dev/null 2>&1 || { echo "[FAIL] elf64: outil absent ($t)"; exit 2; }
done
[ -f "$ELF" ] || { echo "[FAIL] elf64: image absente : $ELF"; exit 2; }

fail=0
pass() { echo "[PASS] elf64 $1"; }
bad()  { echo "[FAIL] elf64 $1"; fail=1; }

hdr="$(readelf -hW "$ELF" 2>/dev/null)" || { echo "[FAIL] elf64 X1: fichier non ELF ($ELF)"; exit 1; }

# ---- Collecte des sorties d'outils : ÉCHEC FERMÉ (K1.1-M64-2-L1-1) ----------
# Défaut corrigé : « und=$(nm -u "$ELF" 2>/dev/null | grep -c .) » donnait 0,
# donc X8 PASS, quand nm -u ÉCHOUAIT. Le statut d'une substitution de commande
# contenant un pipeline est celui de la DERNIÈRE commande (ici grep, qui
# réussit) ; « 2>/dev/null » cachait en plus le message de l'outil. Les mêmes
# motifs rendaient PASS X5 (od), X7 (INTERP, GNU_STACK : readelf -l), X8
# (sections dynamiques : readelf -S), X10 en production (nm) et X12 (readelf -S)
# sur un outil en échec ; démontré par injection (rapport L1-1).
# Désormais chaque outil externe s'exécute UNE fois, ici, AVANT toute règle :
#   - statut non nul : erreur d'outil, statut 2, aucun PASS ;
#   - contrat « nonempty » : une sortie vide est aussi une erreur d'outil
#     (od, readelf -S et readelf -l produisent toujours du texte sur un ELF) ;
#   - contrat « any » : une sortie vide est LÉGITIME (nm -u d'une image sans
#     symbole indéfini, cas normal ; nm d'une image sans symboles, qui répond
#     « no symbols » sur stderr avec le statut 0 ; X4 et X9 échouent alors) ;
#   - stderr n'est plus jeté : il va au journal de l'appelant.
# readelf -h (ci-dessus) garde sa politique : un échec reste le refus X1
# (statut 1), qui couvre le fichier non ELF (test D/N29).
tool_err() { echo "[FAIL] elf64: erreur d'outil : $1"; exit 2; }
internal() { echo "[FAIL] elf64: erreur interne d'analyse : $1"; exit 2; }
collect() {   # collect VARIABLE CONTRAT COMMANDE ARGUMENTS... (CONTRAT : any | nonempty)
    local __out __rc
    __out=$("${@:3}"); __rc=$?
    [ "$__rc" -eq 0 ] || tool_err "${*:3} (statut $__rc)"
    [ "$2" = any ] || [ -n "$__out" ] || tool_err "${*:3} : sortie vide (contrat : sortie non vide)"
    printf -v "$1" '%s' "$__out"
}
collect nm_all  any      nm "$ELF"
collect nm_und  any      nm -u "$ELF"
collect od_out  nonempty od -An -v -tx4 -N 32768 "$ELF"
collect sec_raw nonempty readelf -SW "$ELF"
collect ph      nonempty readelf -lW "$ELF"
collect dis_raw any      objdump -d -M att --no-show-raw-insn "$ELF"   # vide : refus X11 (aucune instruction)

# ---- Analyse des sorties : champs numériques validés AVANT tout calcul -------
# Défaut corrigé : sur un champ illisible, « $((0x$addr)) » fait abandonner à
# bash la BOUCLE entière (puis le script continue) : les drapeaux align_ok,
# seg_ok, x12 restaient à 1 et la règle passait, statut final 0 (démontré :
# adresse .rodata remplacée par « zzzz… »). Toute sortie illisible est donc une
# erreur interne d'analyse (statut 2), jamais un PASS.
is_hex() { [[ "$1" =~ ^(0x)?[0-9a-fA-F]+$ ]]; }
sym() { awk -v s="$1" '$3==s {print "0x" $1}' <<<"$nm_all"; }
entry=$(awk '/Entry point address:/ {print $4}' <<<"$hdr")
is_hex "$entry" || internal "point d'entree illisible dans readelf -h (« $entry »)"
start=$(sym _start); ks=$(sym __kernel_start); ke=$(sym __kernel_end)
for v in "$start" "$ks" "$ke"; do [ -z "$v" ] || is_hex "$v" || internal "valeur de symbole illisible dans nm (« $v »)"; done
# readelf -SW, après le numéro : Name Type Address Off Size ES Flg Lk Inf Al
# ($7 = Flg ; vide pour les sections sans drapeau, d'où un décalage sans
# effet : Lk est numérique et ne contient jamais « A »).
secs=$(sed -n 's/^ *\[ *[0-9]*\] //p' <<<"$sec_raw" | awk '$1 != "" && $7 ~ /A/ {print $1, $2, $3, $5, $7}')
[ -n "$secs" ] || internal "aucune section ALLOC lue dans readelf -SW"
while read -r n typ addr size flg; do
    is_hex "$addr" && is_hex "$size" || internal "section $n : adresse ou taille illisible (« $addr », « $size »)"
done <<<"$secs"
# LOAD Offset VirtAddr PhysAddr FileSiz MemSiz Flg(1 ou 2 mots) Align
loads=$(awk '$1=="LOAD" {fl=""; for (i=7; i<NF; i++) fl = fl $i; print $3, $4, $6, fl, $NF}' <<<"$ph")
while read -r va pa msz fl al; do
    [ -n "$va" ] || continue
    is_hex "$va" && is_hex "$pa" && is_hex "$msz" || internal "segment PT_LOAD illisible (« $va », « $pa », « $msz »)"
done <<<"$loads"

# ---- X1 à X4 : en-tête ----------------------------------------------------
grep -qE '^\s*Class:\s*ELF64' <<<"$hdr" && grep -qE "^\s*Data:.*little endian" <<<"$hdr" \
    && pass "X1: ELF64 little-endian" || bad "X1: classe ou boutisme incorrects"
grep -qE '^\s*Machine:\s*Advanced Micro Devices X86-64' <<<"$hdr" \
    && pass "X2: machine x86-64" || bad "X2: machine != x86-64"
grep -qE '^\s*Type:\s*EXEC' <<<"$hdr" && pass "X3: type EXEC" || bad "X3: type != EXEC"
# entry et start : collectés et validés ci-dessus (L1-1).
if [ -n "$start" ] && [ $((entry)) -eq $((start)) ]; then
    pass "X4: point d'entree = _start ($entry)"
else
    bad "X4: point d'entree ($entry) != _start (${start:-absent})"
fi

# ---- X5 : aucun en-tête Multiboot (image non revendiquée amorçable) -------
mb=$(tr -s ' ' '\n' <<<"$od_out" | grep -ciE '^(1badb002|e85250d6)$')
[ "$mb" -eq 0 ] && pass "X5: aucun magic Multiboot 1/2 (image non amorcable)" \
                || bad "X5: magic Multiboot present ($mb) : mecanisme de chargement avant A-1"

# ---- X6 : sections ALLOC --------------------------------------------------
# secs : collecté et validé ci-dessus (L1-1).
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
# ph et loads : collectés et validés ci-dessus (L1-1).
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
# nm_und et sec_raw : collectés ci-dessus ; un nm -u en échec ne peut plus
# donner « 0 symbole » (défaut corrigé par L1-1).
und=$(grep -c . <<<"$nm_und")
[ "$und" -eq 0 ] && pass "X8: aucun symbole indefini" || bad "X8: $und symbole(s) indefini(s) : $(tr '\n' ' ' <<<"$nm_und")"
dyn=$(sed -n 's/^ *\[ *[0-9]*\] //p' <<<"$sec_raw" | awk '{print $1}' | grep -cE '^\.(rela?\b|rela?\.|dynamic|dynsym|dynstr|got|plt|interp)')
[ "$dyn" -eq 0 ] && pass "X8: aucune section de relogement ni dynamique" || bad "X8: $dyn section(s) de relogement ou dynamique(s)"

# ---- X9 : bornes de l'hypothèse locale ------------------------------------
# ks et ke : collectés et validés ci-dessus (L1-1).
if [ -n "$ks" ] && [ -n "$ke" ] && [ $((ks)) -eq $((0x100000)) ] && [ $((ke)) -le $((0x400000)) ]; then
    pass "X9: __kernel_start = 1 MiB, __kernel_end <= 4 MiB (hypothese locale M64-1)"
else
    bad "X9: bornes de l'image incorrectes (start=${ks:-absent}, end=${ke:-absent})"
fi

# ---- X10 : séparation production/test --------------------------------------
tsyms=$(awk '{print $NF}' <<<"$nm_all" | grep -E '^(ktest_|test_)' | tr '\n' ' ')
if [ "$MODE" = --production ]; then
    [ -z "$tsyms" ] && pass "X10: aucun symbole de test (production)" || bad "X10: symboles de test en production : $tsyms"
else
    grep -qw ktest_m64_skeleton_marker <<<"$tsyms" && pass "X10: temoin de l'image de test present" \
        || bad "X10: temoin ktest_m64_skeleton_marker absent (image de test ?)"
fi

# ---- X11 : périmètre M64-1 (aucune instruction de transition, aucun SIMD) --
# dis_raw : collecté ci-dessus ; un objdump en échec est déjà une erreur d'outil
# (statut 2). drc reste à 0 : le test ci-dessous ne garde que le cas « vide ».
drc=0
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
