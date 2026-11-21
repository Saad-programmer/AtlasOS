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
# Révision K1.1-M64-2-L1-1-R1 (audit contradictoire de L1-1) :
#   - statut 2 aussi pour une sortie INCOMPLÈTE (complétude recoupée par des
#     sources indépendantes), un champ numérique HORS CONTRAT (préfixe, base,
#     longueur, selon l'origine du champ) ou hors du domaine analysable
#     (valeur >= 2^61), et pour toute terminaison imprévue du script ;
#   - statut 1, refus « X1: structure ELF invalide », pour une image dont une
#     table ou un contenu sort du fichier (image tronquée, e_shoff corrompu) ;
#   - plus aucun utilitaire de texte : l'analyse est faite en bash pur.
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
# Garde de terminaison (L1-1-R1) : toute sortie NON prévue du script (variable
# non définie sous set -u, erreur de bash) est convertie en statut 2 avec un
# diagnostic ; seules les sorties explicites via « out » gardent leur statut.
# Sans elle, set -u terminait le script avec le statut 1, confondu avec un
# refus de règle.
__final=0
out() { __final=1; exit "$1"; }
trap '__st=$?; if [ "$__final" -ne 1 ]; then echo "[FAIL] elf64: erreur interne du verificateur (terminaison imprevue, statut $__st)"; exit 2; fi' EXIT
# Locale C : messages des outils en anglais (format analysé ci-dessous) et
# classes de caractères ASCII dans les expressions rationnelles de bash.
export LC_ALL=C
ELF="${1:?usage: check_elf64.sh IMAGE --production|--test}"
MODE="${2:?usage: check_elf64.sh IMAGE --production|--test}"
case "$MODE" in --production|--test) ;; *) echo "[FAIL] elf64: mode inconnu $MODE"; out 2 ;; esac
# Outils externes : EXACTEMENT ceux qu'exécute le script (révision L1-1-R1).
# Après la collecte, l'analyse est faite en bash pur (read, mapfile, [[ =~ ]],
# développement de paramètres) : plus aucun awk, sed, grep, tr, paste ni head,
# donc plus aucun statut d'utilitaire de texte à perdre (constat B1 de l'audit).
# L'ordre conserve le diagnostic du test D/T1 (objdump masqué).
for t in readelf nm objdump od stat; do
    command -v "$t" >/dev/null 2>&1 || { echo "[FAIL] elf64: outil absent ($t)"; out 2; }
done
[ -f "$ELF" ] || { echo "[FAIL] elf64: image absente : $ELF"; out 2; }

fail=0
# Les verdicts des règles sont MIS EN TAMPON et n'apparaissent qu'à la fin
# normale du script : une erreur interne survenant pendant l'évaluation des
# règles (statut 2) n'est donc jamais précédée de lignes PASS (L1-1-R1).
OUTBUF=""
pass() { OUTBUF+="[PASS] elf64 $1"$'\n'; }
bad()  { OUTBUF+="[FAIL] elf64 $1"$'\n'; fail=1; }

hdr="$(readelf -hW "$ELF" 2>/dev/null)" || { echo "[FAIL] elf64 X1: fichier non ELF ($ELF)"; out 1; }

# ---- Statuts d'échec fermé (K1.1-M64-2-L1-1, révisés L1-1-R1) ---------------
#   tool_err : un outil a échoué (statut non nul, sortie vide hors contrat) ;
#   internal : sortie d'outil illisible, incomplète ou hors contrat numérique,
#              ou valeur hors du domaine analysable ; dans les deux cas statut 2,
#              AUCUNE règle n'est évaluée, aucun PASS n'est émis ;
#   sfail    : l'IMAGE est structurellement invalide (table ou contenu hors du
#              fichier) : refus nommé X1, statut 1, comme un fichier non ELF.
tool_err() { echo "[FAIL] elf64: erreur d'outil : $1"; out 2; }
internal() { echo "[FAIL] elf64: erreur interne d'analyse : $1"; out 2; }
sfail()    { echo "[FAIL] elf64 X1: structure ELF invalide : $1"; out 1; }
collect() {   # collect VARIABLE CONTRAT COMMANDE ARGUMENTS... (CONTRAT : any | nonempty)
    local __out __rc
    __out=$("${@:3}"); __rc=$?
    [ "$__rc" -eq 0 ] || tool_err "${*:3} (statut $__rc)"
    [ "$2" = any ] || [ -n "$__out" ] || tool_err "${*:3} : sortie vide (contrat : sortie non vide)"
    printf -v "$1" '%s' "$__out"
}
# rx CHAINE REGEX : 0 correspondance, 1 absence LÉGITIME de correspondance ;
# tout autre statut de [[ =~ ]] (expression invalide) est une erreur interne,
# jamais confondue avec une absence de correspondance.
rx() { local __r; [[ $1 =~ $2 ]]; __r=$?; [ "$__r" -le 1 ] || internal "expression rationnelle invalide ($2)"; return "$__r"; }
# ---- Contrat numérique (constat B2) ------------------------------------------
# num VARIABLE CONTRAT CHAINE QUOI, CONTRAT selon l'ORIGINE du champ :
#   hex   : 1 à 16 chiffres hexadécimaux minuscules SANS préfixe
#           (readelf -S : Address, Off, Size, ES ; nm ; objdump : adresses) ;
#   hex0x : « 0x » OBLIGATOIRE puis 1 à 16 chiffres hexadécimaux minuscules
#           (readelf -l : Offset … Align ; point d'entrée de readelf -h et -l) ;
#   dec   : décimal, sans zéro initial (sauf « 0 »), au plus 19 chiffres
#           (readelf -h : décalages, tailles, nombres ; readelf -S : Lk, Inf,
#           Al ; stat : taille du fichier).
# Le préfixe manquant ou en trop, un chiffre hors base (8 ou 9 en octal : la
# conversion est explicite, 16#… ou 10#…, jamais l'interprétation implicite de
# bash), une valeur vide, signée, majuscule ou trop longue sont des erreurs
# internes (statut 2). Statut 3 : valeur bien formée mais >= 2^61. Toutes les
# valeurs acceptées étant < 2^61, aucune somme ni aucun arrondi calculé plus
# bas (au plus a + s + 4095, ou décalage + n × taille d'entrée avec n et la
# taille <= 65535) ne peut dépasser 2^63 - 1 : l'arithmétique signée 64 bits de
# bash ne déborde pas.
num() {
    local __s="$3" __d
    case "$2" in
        hex0x) rx "$__s" '^0x([0-9a-f]{1,16})$' || internal "$4 : « $__s » hors contrat (0x puis 1 a 16 chiffres hexadecimaux minuscules)"
               __d=${BASH_REMATCH[1]} ;;
        hex)   rx "$__s" '^[0-9a-f]{1,16}$' || internal "$4 : « $__s » hors contrat (1 a 16 chiffres hexadecimaux minuscules, sans prefixe)"
               __d=$__s ;;
        dec)   rx "$__s" '^(0|[1-9][0-9]{0,18})$' || internal "$4 : « $__s » hors contrat (entier decimal sans zero initial, 19 chiffres au plus)"
               if [ "${#__s}" -eq 19 ] && [[ ! "$__s" < 2305843009213693952 ]]; then return 3; fi
               printf -v "$1" '%s' "$((10#$__s))"; return 0 ;;
        *)     internal "contrat numerique inconnu ($2)" ;;
    esac
    while [[ $__d == 0?* ]]; do __d=${__d#0}; done
    if [ "${#__d}" -eq 16 ] && [[ $__d == [2-9a-f]* ]]; then return 3; fi
    printf -v "$1" '%s' "$((16#$__d))"
}
big() { internal "$1 : valeur >= 2^61, hors du domaine analysable"; }
# num_s : comme num, mais une valeur hors domaine est une incohérence
# STRUCTURELLE (décalage ou taille de contenu dans le fichier : aucun fichier
# analysé n'atteint 2^61 octets).
num_s() { num "$@"; [ $? -ne 3 ] || sfail "$4 = $3, au-dela de toute taille de fichier"; }
# num_a : une valeur hors domaine (adresse, taille mémoire) est une erreur
# d'analyse, statut 2 (limite : une image en moitié haute n'est pas analysée).
num_a() { num "$@"; [ $? -ne 3 ] || big "$4 = $3"; }
split_lines() { if [ -n "$2" ]; then mapfile -t "$1" <<<"$2"; else eval "$1=()"; fi; }

# ---- readelf -h : champs requis, uniques, au format attendu -----------------
declare -A H=()
split_lines HL "$hdr"
[ "${HL[0]-}" = "ELF Header:" ] || internal "readelf -h : premiere ligne inattendue (« ${HL[0]-} »)"
for ((i = 1; i < ${#HL[@]}; i++)); do
    rx "${HL[i]}" '^  ([^:]+):[[:space:]]+(.*[^[:space:]])[[:space:]]*$' || internal "readelf -h : ligne illisible (« ${HL[i]} »)"
    k=${BASH_REMATCH[1]}; v=${BASH_REMATCH[2]}
    case "$k" in
        Class|Data|Type|Machine|"Entry point address"|"Start of program headers"|"Start of section headers"|\
        "Size of program headers"|"Number of program headers"|"Size of section headers"|"Number of section headers")
            [ -z "${H[$k]+x}" ] || internal "readelf -h : champ « $k » en double"
            H[$k]=$v ;;
    esac
done
for k in Class Data Type Machine "Entry point address" "Start of program headers" "Start of section headers" \
         "Size of program headers" "Number of program headers" "Size of section headers" "Number of section headers"; do
    [ -n "${H[$k]+x}" ] || internal "readelf -h : champ « $k » absent (sortie incomplete)"
done
entry=${H[Entry point address]}
num_a entry_v hex0x "$entry" "point d'entree (readelf -h)"
hfield() {   # hfield VARIABLE CHAMP SUFFIXE : « N SUFFIXE » décimal
    rx "${H[$2]}" "^([^ ]+)$3\$" || internal "readelf -h : « $2 » illisible (« ${H[$2]} »)"
    num_s "$1" dec "${BASH_REMATCH[1]}" "readelf -h : $2"
}
hfield e_phoff "Start of program headers" ' \(bytes into file\)'
hfield e_shoff "Start of section headers" ' \(bytes into file\)'
hfield e_phentsize "Size of program headers" ' \(bytes\)'
hfield e_shentsize "Size of section headers" ' \(bytes\)'
hfield e_phnum "Number of program headers" ''
hfield e_shnum "Number of section headers" ''
for v in e_phentsize e_shentsize e_phnum e_shnum; do
    [ "${!v}" -le 65535 ] || internal "readelf -h : $v = ${!v} (champ ELF de 16 bits ; numerotation etendue non prise en charge)"
done

# ---- Structure : chaque table et chaque contenu doit tenir dans le fichier ---
# (constat M2) : une image tronquée ou un e_shoff hors du fichier est un ELF
# INVALIDE, refusé par X1 (statut 1), et non une erreur d'outil (statut 2).
collect fsize_s nonempty stat -L -c %s -- "$ELF"
num fsize dec "$fsize_s" "stat : taille du fichier" || big "stat : taille du fichier = $fsize_s"
[ "$e_phnum" -eq 0 ] || [ $((e_phoff + e_phnum * e_phentsize)) -le "$fsize" ] \
    || sfail "table des segments hors du fichier (e_phoff=$e_phoff, e_phnum=$e_phnum, e_phentsize=$e_phentsize, taille=$fsize)"
[ "$e_shnum" -eq 0 ] || [ $((e_shoff + e_shnum * e_shentsize)) -le "$fsize" ] \
    || sfail "table des sections hors du fichier (e_shoff=$e_shoff, e_shnum=$e_shnum, e_shentsize=$e_shentsize, taille=$fsize)"

# ---- readelf -SW : COMPLET (e_shnum lignes, numérotées 0..N-1) et TOKENISÉ ---
# Ligne : [Nr] Name Type Address Off Size ES Flg Lk Inf Al. Name et Flg peuvent
# être vides, d'où 8, 9 ou 10 champs : 10 = nom et drapeaux ; 8 = ni l'un ni
# l'autre ; 9 = l'un des deux : sans nom si le 2e champ est une adresse
# hexadécimale (un Type n'est jamais en minuscules hexadécimales), sinon sans
# drapeaux. Tout autre nombre de champs est une erreur (constat M3 : un champ
# manquant décalait les colonnes sans être vu).
collect sec_raw nonempty readelf -SW "$ELF"
split_lines SL "$sec_raw"
S_name=(); S_type=(); S_addr=(); S_addr_s=(); S_size=(); S_size_s=(); S_flg=(); S_es=(); S_es_s=()
if [ "$e_shnum" -eq 0 ]; then
    [ "$sec_raw" = $'\nThere are no sections in this file.' ] || internal "readelf -SW : sortie inattendue pour e_shnum = 0"
else
    rx "${SL[0]-}" '^There (are|is) ([0-9]+) section headers?, starting at offset (0x[0-9a-f]+):$' \
        || internal "readelf -SW : en-tete illisible (« ${SL[0]-} »)"
    c2=${BASH_REMATCH[2]}; c3=${BASH_REMATCH[3]}   # rx (dans num) écrase BASH_REMATCH
    num n dec "$c2" "readelf -SW : nombre de sections"; num_s o hex0x "$c3" "readelf -SW : decalage"
    [ "$n" -eq "$e_shnum" ] && [ "$o" -eq "$e_shoff" ] || internal "readelf -SW : $n sections a $o, readelf -h annonce $e_shnum a $e_shoff"
    [ "${SL[1]-x}" = "" ] && [ "${SL[2]-}" = "Section Headers:" ] || internal "readelf -SW : sortie incomplete (titre absent)"
    rx "${SL[3]-}" '^  \[Nr\] Name +Type +Addr(ess)? +Off +Size +ES +Flg +Lk +Inf +Al$' || internal "readelf -SW : ligne de colonnes inattendue (« ${SL[3]-} »)"
    for ((i = 0; i < e_shnum; i++)); do
        l=${SL[i + 4]-}
        rx "$l" '^  \[ *([0-9]+)\] (.*)$' || internal "readelf -SW : section $i absente ou illisible (sortie incomplete : « $l »)"
        [ "${BASH_REMATCH[1]}" = "$i" ] || internal "readelf -SW : section numero ${BASH_REMATCH[1]} a la place de $i"
        read -ra T <<<"${BASH_REMATCH[2]}"
        case ${#T[@]} in
            10) nm_=${T[0]}; F=("${T[@]:1:5}"); fl_=${T[6]}; R=("${T[@]:7:3}") ;;
            9)  if rx "${T[1]}" '^[0-9a-f]+$'; then nm_=""; F=("${T[@]:0:5}"); fl_=${T[5]}; R=("${T[@]:6:3}")
                else nm_=${T[0]}; F=("${T[@]:1:5}"); fl_=""; R=("${T[@]:6:3}"); fi ;;
            8)  nm_=""; F=("${T[@]:0:5}"); fl_=""; R=("${T[@]:5:3}") ;;
            *)  internal "section ${T[0]-?} (numero $i) : ${#T[@]} champs dans readelf -SW (8 a 10 attendus)" ;;
        esac
        q="section ${nm_:-numero $i}"
        rx "${F[0]}" '^[A-Z][A-Za-z0-9_+]*$' || internal "$q : type « ${F[0]} » illisible"
        [ -z "$fl_" ] || rx "$fl_" '^[WAXMSILOGTCxoEpDlyR]+$' || internal "$q : drapeaux « $fl_ » illisibles"
        num_a a_ hex "${F[1]}" "$q : adresse"
        num_s o_ hex "${F[2]}" "$q : decalage"
        num_s s_ hex "${F[3]}" "$q : taille"
        num_a e_ hex "${F[4]}" "$q : ES"
        for r in "${R[@]}"; do num_a r_ dec "$r" "$q : Lk/Inf/Al"; done
        # Contenu de la section dans le fichier (une section NOBITS n'en a pas).
        [ "${F[0]}" = NOBITS ] || [ "$s_" -eq 0 ] || [ $((o_ + s_)) -le "$fsize" ] \
            || sfail "$q hors du fichier (decalage 0x${F[2]}, taille 0x${F[3]}, fichier $fsize octets)"
        S_name+=("$nm_"); S_type+=("${F[0]}"); S_addr+=("$a_"); S_addr_s+=("${F[1]}")
        S_size+=("$s_"); S_size_s+=("${F[3]}"); S_flg+=("$fl_"); S_es+=("$e_"); S_es_s+=("${F[4]}")
    done
    [ "${SL[e_shnum + 4]-}" = "Key to Flags:" ] || internal "readelf -SW : ligne inattendue apres la section $((e_shnum - 1)) (« ${SL[e_shnum + 4]-} »)"
fi

# ---- readelf -lW : COMPLET (e_phnum entrées ; correspondance 00..N-1) --------
# Entrée : Type Offset VirtAddr PhysAddr FileSiz MemSiz Flg(0 à 2 mots) Align.
collect ph nonempty readelf -lW "$ELF"
split_lines PL "$ph"
P_type=(); P_va=(); P_va_s=(); P_pa=(); P_pa_s=(); P_msz=(); P_fl=(); P_al_s=(); MAP=()
if [ "$e_phnum" -eq 0 ]; then
    [ "$ph" = $'\nThere are no program headers in this file.' ] || internal "readelf -lW : sortie inattendue pour e_phnum = 0"
else
    [ "${PL[0]-x}" = "" ] && rx "${PL[1]-}" '^Elf file type is [A-Z]+ \(.*\)$' || internal "readelf -lW : en-tete illisible"
    [ "${PL[2]-}" = "Entry point $entry" ] || internal "readelf -lW : point d'entree « ${PL[2]-} » different de readelf -h ($entry)"
    rx "${PL[3]-}" '^There (are|is) ([0-9]+) program headers?, starting at offset ([0-9]+)$' || internal "readelf -lW : nombre de segments illisible"
    c2=${BASH_REMATCH[2]}; c3=${BASH_REMATCH[3]}
    num n dec "$c2" "readelf -lW : nombre de segments"; num_s o dec "$c3" "readelf -lW : decalage"
    [ "$n" -eq "$e_phnum" ] && [ "$o" -eq "$e_phoff" ] || internal "readelf -lW : $n segments a $o, readelf -h annonce $e_phnum a $e_phoff"
    [ "${PL[4]-x}" = "" ] && [ "${PL[5]-}" = "Program Headers:" ] || internal "readelf -lW : sortie incomplete (titre absent)"
    rx "${PL[6]-}" '^  Type +Offset +VirtAddr +PhysAddr +FileSiz +MemSiz +Flg +Align$' || internal "readelf -lW : ligne de colonnes inattendue"
    j=7
    for ((i = 0; i < e_phnum; i++, j++)); do
        l=${PL[j]-}
        rx "$l" '^  [^ ]' || internal "segment numero $i absent ou illisible dans readelf -lW (sortie incomplete : « $l »)"
        read -ra T <<<"$l"
        [ "${#T[@]}" -ge 7 ] && [ "${#T[@]}" -le 9 ] || internal "segment PT_LOAD ou autre (numero $i) : ${#T[@]} champs dans readelf -lW (7 a 9 attendus)"
        q="segment PT_LOAD ou autre (numero $i, ${T[0]})"
        rx "${T[0]}" '^[A-Za-z][A-Za-z0-9_+]*$' || internal "$q : type illisible"
        fl_=""; for ((k = 6; k < ${#T[@]} - 1; k++)); do rx "${T[k]}" '^[RWE]+$' || internal "$q : droits « ${T[k]} » illisibles"; fl_+=${T[k]}; done
        num_s o_ hex0x "${T[1]}" "$q : Offset"
        num_a va_ hex0x "${T[2]}" "$q : VirtAddr"
        num_a pa_ hex0x "${T[3]}" "$q : PhysAddr"
        num_s fs_ hex0x "${T[4]}" "$q : FileSiz"
        num_a ms_ hex0x "${T[5]}" "$q : MemSiz"
        num_a al_ hex0x "${T[${#T[@]} - 1]}" "$q : Align"
        [ $((o_ + fs_)) -le "$fsize" ] || sfail "segment numero $i hors du fichier (Offset ${T[1]}, FileSiz ${T[4]}, fichier $fsize octets)"
        P_type+=("${T[0]}"); P_va+=("$va_"); P_va_s+=("${T[2]}"); P_pa+=("$pa_"); P_pa_s+=("${T[3]}")
        P_msz+=("$ms_"); P_fl+=("$fl_"); P_al_s+=("${T[${#T[@]} - 1]}")
        if rx "${PL[j + 1]-}" '^      \[Requesting program interpreter: .*\]$'; then j=$((j + 1)); fi
    done
    if [ "$e_shnum" -gt 0 ]; then
        [ "${PL[j]-x}" = "" ] && [ "${PL[j + 1]-}" = " Section to Segment mapping:" ] && [ "${PL[j + 2]-}" = "  Segment Sections..." ] \
            || internal "readelf -lW : correspondance sections -> segments absente (sortie incomplete)"
        j=$((j + 3))
        for ((i = 0; i < e_phnum; i++, j++)); do
            rx "${PL[j]-}" '^   ([0-9]{2,})(.*)$' || internal "readelf -lW : correspondance du segment $i absente (sortie incomplete)"
            [ "$((10#${BASH_REMATCH[1]}))" -eq "$i" ] || internal "readelf -lW : correspondance numero ${BASH_REMATCH[1]} a la place de $i"
            m_=${BASH_REMATCH[2]}
            while [[ $m_ == [[:space:]]* ]]; do m_=${m_#?}; done
            while [[ $m_ == *[[:space:]] ]]; do m_=${m_%?}; done
            MAP+=("${m_:--}")
        done
        while [ "$j" -lt "${#PL[@]}" ]; do
            rx "${PL[j]}" '^   None( .*)?$' || internal "readelf -lW : ligne inattendue (« ${PL[j]} »)"; j=$((j + 1))
        done
    else
        [ "$j" -eq "${#PL[@]}" ] || internal "readelf -lW : ligne inattendue apres les segments (« ${PL[j]} »)"
    fi
fi

# ---- nm, nm -u et readelf -sW : cohérence COMPLÈTE (constat M1) --------------
# nm peut réussir (statut 0) avec une sortie tronquée ; seule une source
# indépendante le révèle. La table .symtab vue par readelf -sW (dont le nombre
# d'entrées est aussi fixé par la taille de la section SYMTAB de readelf -SW)
# doit donner EXACTEMENT les mêmes symboles que nm (hors entrée 0 et symboles
# FILE et SECTION, que nm n'affiche pas), et ses symboles UND exactement ceux de
# nm -u. Sinon : statut 2. Une table illisible est aussi une erreur.
collect nm_all any nm "$ELF"
collect nm_und any nm -u "$ELF"
collect sym_raw any readelf -sW "$ELF"
declare -A EXPT=() SEEN=() SC=() UC=() SYMV=()
key_add() { local -n __m=$1; __m["$2"]=$(( ${__m["$2"]:-0} + $3 )); }
for ((i = 0; i < ${#S_name[@]}; i++)); do
    case "${S_type[i]}" in SYMTAB|DYNSYM) ;; *) continue ;; esac
    es_=${S_es[i]}
    [ "$es_" -gt 0 ] && [ $((S_size[i] % es_)) -eq 0 ] || internal "section ${S_name[i]} : taille 0x${S_size_s[i]} non multiple de ES 0x${S_es_s[i]}"
    [ -z "${EXPT[${S_name[i]}]+x}" ] || internal "deux tables de symboles nommees ${S_name[i]}"
    EXPT[${S_name[i]}]=$((S_size[i] / es_))
done
split_lines YL "$sym_raw"
symtab_name=""; for ((i = 0; i < ${#S_name[@]}; i++)); do [ "${S_type[i]}" != SYMTAB ] || { [ -z "$symtab_name" ] || internal "plusieurs sections SYMTAB"; symtab_name=${S_name[i]}; }; done
j=0
while [ "$j" -lt "${#YL[@]}" ]; do
    l=${YL[j]}; j=$((j + 1))
    if [ -z "$l" ] || [ "$l" = "Dynamic symbol information is not available for displaying symbols." ]; then continue; fi
    rx "$l" "^Symbol table '(.+)' contains ([0-9]+) entr(y|ies):\$" || internal "readelf -sW : ligne illisible (« $l »)"
    t_=${BASH_REMATCH[1]}; num n dec "${BASH_REMATCH[2]}" "readelf -sW : nombre d'entrees de $t_"
    [ -n "${EXPT[$t_]+x}" ] && [ -z "${SEEN[$t_]+x}" ] || internal "readelf -sW : table $t_ inattendue ou repetee"
    [ "$n" -eq "${EXPT[$t_]}" ] || internal "readelf -sW : $t_ annonce $n entrees, la section en contient ${EXPT[$t_]}"
    SEEN[$t_]=1
    rx "${YL[j]-}" '^ +Num: +Value +Size +Type +Bind +Vis +Ndx +Name$' || internal "readelf -sW : colonnes de $t_ absentes"
    j=$((j + 1))
    for ((k = 0; k < n; k++, j++)); do
        rx "${YL[j]-}" '^ *([0-9]+): (.*)$' || internal "readelf -sW : entree $k de $t_ absente (sortie incomplete)"
        [ "${BASH_REMATCH[1]}" = "$k" ] || internal "readelf -sW : entree ${BASH_REMATCH[1]} de $t_ a la place de $k"
        [ "$t_" = "$symtab_name" ] && [ "$k" -gt 0 ] || continue
        read -ra T <<<"${BASH_REMATCH[2]}"
        [ "${#T[@]}" -eq 6 ] || [ "${#T[@]}" -eq 7 ] || internal "readelf -sW : entree $k de $t_ : ${#T[@]} champs (6 ou 7 attendus)"
        num_a v_ hex "${T[0]}" "readelf -sW : valeur de l'entree $k"
        case "${T[2]}" in FILE|SECTION) continue ;; esac
        if [ "${T[5]}" = UND ]; then key_add SC "U|${T[6]-}" 1; key_add UC "${T[6]-}" 1
        else key_add SC "D|${T[0]}|${T[6]-}" 1; fi
    done
done
for t_ in "${!EXPT[@]}"; do [ -n "${SEEN[$t_]+x}" ] || internal "readelf -sW : table $t_ absente (sortie incomplete)"; done
# nm : « VALEUR TYPE NOM » (défini) ou « espaces TYPE NOM » (indéfini).
split_lines NL "$nm_all"
NM_names=()
for l in "${NL[@]}"; do
    if rx "$l" '^([^ ]+) ([A-Za-z?-]) ([^[:space:]]+)$'; then
        v_=${BASH_REMATCH[1]}; n_=${BASH_REMATCH[3]}
        num_a x_ hex "$v_" "valeur de symbole $n_ (nm)"
        key_add SC "D|$v_|$n_" -1
        case "$n_" in _start|__kernel_start|__kernel_end)
            [ -z "${SYMV[$n_]+x}" ] || internal "valeur de symbole $n_ : symbole defini plusieurs fois dans nm"
            SYMV[$n_]=$v_ ;;
        esac
    elif rx "$l" '^ +([A-Za-z]) ([^[:space:]]+)$'; then
        n_=${BASH_REMATCH[2]}; key_add SC "U|$n_" -1
    else
        internal "valeur de symbole ou ligne illisible dans nm (« $l »)"
    fi
    NM_names+=("$n_")
done
split_lines UL "$nm_und"
for l in "${UL[@]}"; do
    rx "$l" '^ +([A-Za-z]) ([^[:space:]]+)$' || internal "nm -u : ligne illisible (« $l »)"
    key_add UC "${BASH_REMATCH[2]}" -1
done
for k in "${!SC[@]}"; do [ "${SC["$k"]}" -eq 0 ] || internal "nm et readelf -sW divergent sur le symbole « $k » (sortie incomplete ?)"; done
for k in "${!UC[@]}"; do [ "${UC["$k"]}" -eq 0 ] || internal "nm -u et readelf -sW divergent sur le symbole indefini « $k »"; done
start=${SYMV[_start]:+0x${SYMV[_start]}}; ks=${SYMV[__kernel_start]:+0x${SYMV[__kernel_start]}}; ke=${SYMV[__kernel_end]:+0x${SYMV[__kernel_end]}}
[ -z "$start" ] || num_a start_v hex0x "$start" "valeur de symbole _start"
[ -z "$ks" ] || num_a ks_v hex0x "$ks" "valeur de symbole __kernel_start"
[ -z "$ke" ] || num_a ke_v hex0x "$ke" "valeur de symbole __kernel_end"

# ---- od : 32 premiers KiB, COMPLETS (constat M1) ----------------------------
# od -tx4 produit ceil(min(taille, 32768) / 4) mots de 8 chiffres (le dernier
# mot partiel est complété par des zéros) : un nombre de mots différent est une
# sortie incomplète ou excédentaire, statut 2.
collect od_out nonempty od -An -v -tx4 -N 32768 "$ELF"
lim=$fsize; [ "$lim" -le 32768 ] || lim=32768
want=$(( (lim + 3) / 4 )); nw=0; mb=0
split_lines OL "$od_out"
for l in "${OL[@]}"; do
    read -ra T <<<"$l"
    [ "${#T[@]}" -ge 1 ] && [ "${#T[@]}" -le 4 ] || internal "od : ligne de ${#T[@]} mots"
    for w in "${T[@]}"; do
        rx "$w" '^[0-9a-f]{8}$' || internal "od : mot « $w » hors contrat (8 chiffres hexadecimaux minuscules)"
        nw=$((nw + 1)); case "$w" in 1badb002|e85250d6) mb=$((mb + 1)) ;; esac
    done
done
[ "$nw" -eq "$want" ] || internal "od : $nw mots lus, $want attendus pour $lim octets (sortie incomplete ou excedentaire)"

# ---- objdump : UNE collection, avec octets bruts, COUVERTURE vérifiée --------
# -w : une ligne par instruction ; -z : aucun bloc de zéros sauté (« ... »).
# Chaque section exécutable avec contenu doit être désassemblée exactement une
# fois, de son adresse à adresse + taille, instruction après instruction, sans
# trou ni recouvrement (longueur = nombre d'octets affichés) ; sinon statut 2.
# X11 analyse le texte de cette MÊME sortie (plus de second désassemblage).
collect dis_raw any objdump -d -w -z -M att "$ELF"
declare -A XS=() XD=()
for ((i = 0; i < ${#S_name[@]}; i++)); do
    [[ ${S_flg[i]} == *X* ]] && [ "${S_type[i]}" != NOBITS ] || continue
    [ -z "${XS[${S_name[i]}]+x}" ] || internal "deux sections executables nommees ${S_name[i]}"
    XS[${S_name[i]}]=$i
done
split_lines DL "$dis_raw"
INS=(); cur=""; cpos=0; cend=0; nfmt=0
close_sec() { [ -z "$cur" ] || [ "$cpos" -eq "$cend" ] || internal "objdump : section $cur couverte jusqu'a 0x$(printf %x "$cpos") au lieu de 0x$(printf %x "$cend") (sortie incomplete)"; }
TAB=$'\t'
for l in "${DL[@]}"; do
    [ -n "$l" ] || continue
    if rx "$l" "^ *([0-9a-f]+):$TAB([0-9a-f]{2}( [0-9a-f]{2})*) *$TAB(.*)\$"; then
        [ -n "$cur" ] || internal "objdump : instruction hors section"
        c1=${BASH_REMATCH[1]}; c2=${BASH_REMATCH[2]}; t_=${BASH_REMATCH[4]}
        num_a ad_ hex "$c1" "objdump : adresse"
        [ "$ad_" -eq "$cpos" ] || internal "objdump : section $cur : instruction a 0x$c1, attendue a 0x$(printf %x "$cpos") (trou ou recouvrement)"
        read -ra B <<<"$c2"; cpos=$((cpos + ${#B[@]}))
        [ "$cpos" -le "$cend" ] || internal "objdump : section $cur : instruction au-dela de la fin"
        INS+=("${t_%%"$TAB"*}")
    elif rx "$l" '^Disassembly of section (.+):$'; then
        close_sec; cur=${BASH_REMATCH[1]}
        [ -n "${XS[$cur]+x}" ] && [ -z "${XD[$cur]+x}" ] || internal "objdump : section $cur inattendue ou repetee"
        XD[$cur]=1; i=${XS[$cur]}; cpos=${S_addr[i]}; cend=$((S_addr[i] + S_size[i]))
    elif rx "$l" '^[0-9a-f]+ <.*>:$'; then
        [ -n "$cur" ] || internal "objdump : etiquette hors section"
    elif rx "$l" '^.+:     file format [^ ]+$'; then
        nfmt=$((nfmt + 1))
    else
        internal "objdump : ligne illisible (« $l »)"
    fi
done
close_sec
[ "$nfmt" -eq 1 ] || internal "objdump : en-tete « file format » vu $nfmt fois"
for s_ in "${!XS[@]}"; do
    [ -n "${XD[$s_]+x}" ] || [ "${S_size[${XS[$s_]}]}" -eq 0 ] || internal "objdump : section executable $s_ non desassemblee (sortie incomplete)"
done

# ============================================================================
# RÈGLES. Les critères X1 à X13 sont INCHANGÉS ; seules leurs données viennent
# désormais des tableaux validés ci-dessus (aucune commande externe ici).
# ============================================================================
# ---- X1 à X4 : en-tête ----------------------------------------------------
[[ ${H[Class]} == ELF64* ]] && [[ ${H[Data]} == *"little endian"* ]] \
    && pass "X1: ELF64 little-endian" || bad "X1: classe ou boutisme incorrects"
[[ ${H[Machine]} == "Advanced Micro Devices X86-64"* ]] \
    && pass "X2: machine x86-64" || bad "X2: machine != x86-64"
[[ ${H[Type]} == EXEC* ]] && pass "X3: type EXEC" || bad "X3: type != EXEC"
if [ -n "$start" ] && [ "$entry_v" -eq "$start_v" ]; then
    pass "X4: point d'entree = _start ($entry)"
else
    bad "X4: point d'entree ($entry) != _start (${start:-absent})"
fi

# ---- X5 : aucun en-tête Multiboot (image non revendiquée amorçable) -------
[ "$mb" -eq 0 ] && pass "X5: aucun magic Multiboot 1/2 (image non amorcable)" \
                || bad "X5: magic Multiboot present ($mb) : mecanisme de chargement avant A-1"

# ---- X6 : sections ALLOC --------------------------------------------------
A_idx=(); names=""
for ((i = 0; i < ${#S_name[@]}; i++)); do [[ ${S_flg[i]} == *A* ]] && { A_idx+=("$i"); names+="${S_name[i]} "; }; done
if [ "$names" = ".text .rodata .data .bss " ]; then
    pass "X6: sections ALLOC = .text .rodata .data .bss"
else
    bad "X6: sections ALLOC inattendues : $names"
fi
prev=-1; order_ok=1; align_ok=1
for i in "${A_idx[@]}"; do
    a=${S_addr[i]}; n=${S_name[i]}
    [ $((a % 4096)) -eq 0 ] || { align_ok=0; bad "X6: $n non alignee sur 4 KiB (0x${S_addr_s[i]})"; }
    [ "$a" -gt "$prev" ] || order_ok=0
    prev=$a
    if [ "$n" = ".bss" ] && [ "${S_type[i]}" != NOBITS ]; then bad "X6: .bss n'est pas NOBITS"; fi
done
[ "$align_ok" -eq 1 ] && pass "X6: debut de chaque section ALLOC aligne sur 4 KiB"
[ "$order_ok" -eq 1 ] && pass "X6: ordre croissant des sections ALLOC" || bad "X6: ordre des sections incorrect"

# ---- X7 : segments ----------------------------------------------------------
L_idx=(); flags_seq=""
for ((i = 0; i < ${#P_type[@]}; i++)); do [ "${P_type[i]}" = LOAD ] && { L_idx+=("$i"); flags_seq+="${P_fl[i]} "; }; done
nload=${#L_idx[@]}
[ "$nload" -eq 3 ] && [ "$flags_seq" = "RE R RW " ] \
    && pass "X7: 3 PT_LOAD, droits R E / R / RW" || bad "X7: segments PT_LOAD inattendus ($nload : $flags_seq)"
seg_ok=1; prev_end=0
for i in "${L_idx[@]}"; do
    v=${P_va[i]}; p=${P_pa[i]}; m=${P_msz[i]}; va=${P_va_s[i]}; fl=${P_fl[i]}
    case "$fl" in *W*E*|*E*W*) seg_ok=0; bad "X7: segment $va W+X ($fl)" ;; esac
    [ "$v" -eq "$p" ] || { seg_ok=0; bad "X7: segment $va : vaddr != paddr (${P_pa_s[i]})"; }
    [ "$v" -ge $((0x100000)) ] || { seg_ok=0; bad "X7: segment $va sous 1 MiB"; }
    [ $((v % 4096)) -eq 0 ] || { seg_ok=0; bad "X7: segment $va non aligne sur 4 KiB"; }
    [ "${P_al_s[i]}" = "0x1000" ] || { seg_ok=0; bad "X7: segment $va alignement ${P_al_s[i]} != 0x1000"; }
    # Page partagée : le segment commence dans la dernière page du précédent.
    [ "$v" -ge "$prev_end" ] || { seg_ok=0; bad "X7: segment $va partage une page avec le precedent (fin arrondie 0x$(printf %x "$prev_end"))"; }
    prev_end=$(( (v + m + 4095) / 4096 * 4096 ))
done
[ "$seg_ok" -eq 1 ] && pass "X7: aucun W+X, vaddr=paddr >= 1 MiB, alignes, aucune page partagee"
idyn=0; xstack=0
for ((i = 0; i < ${#P_type[@]}; i++)); do
    case "${P_type[i]}" in INTERP|DYNAMIC) idyn=1 ;; GNU_STACK) [[ ${P_fl[i]} != *E* ]] || xstack=1 ;; esac
done
if [ "$idyn" -eq 1 ]; then bad "X7: segment INTERP ou DYNAMIC present"; else pass "X7: ni INTERP ni DYNAMIC"; fi
if [ "$xstack" -eq 1 ]; then bad "X7: pile GNU_STACK executable"; else pass "X7: pile non executable"; fi

# ---- X8 : symboles indéfinis, relogements, dynamique ----------------------
und=0; und_list=""
for l in "${UL[@]}"; do und=$((und + 1)); und_list+="$l "; done
[ "$und" -eq 0 ] && pass "X8: aucun symbole indefini" || bad "X8: $und symbole(s) indefini(s) : $und_list"
# « \b » de l'ancienne expression grep : fin du nom ou caractère hors [A-Za-z0-9_].
dyn=0
for n in "${S_name[@]}"; do
    if rx "$n" '^\.(rela?($|[^A-Za-z0-9_])|rela?\.|dynamic|dynsym|dynstr|got|plt|interp)'; then dyn=$((dyn + 1)); fi
done
[ "$dyn" -eq 0 ] && pass "X8: aucune section de relogement ni dynamique" || bad "X8: $dyn section(s) de relogement ou dynamique(s)"

# ---- X9 : bornes de l'hypothèse locale ------------------------------------
if [ -n "$ks" ] && [ -n "$ke" ] && [ "$ks_v" -eq $((0x100000)) ] && [ "$ke_v" -le $((0x400000)) ]; then
    pass "X9: __kernel_start = 1 MiB, __kernel_end <= 4 MiB (hypothese locale M64-1)"
else
    bad "X9: bornes de l'image incorrectes (start=${ks:-absent}, end=${ke:-absent})"
fi

# ---- X10 : séparation production/test --------------------------------------
tsyms=""
for n in "${NM_names[@]}"; do case "$n" in ktest_*|test_*) tsyms+="$n " ;; esac; done
if [ "$MODE" = --production ]; then
    [ -z "$tsyms" ] && pass "X10: aucun symbole de test (production)" || bad "X10: symboles de test en production : $tsyms"
else
    # Équivalent de « grep -w » : le nom entouré de caractères hors [A-Za-z0-9_].
    rx "$tsyms" '(^|[^A-Za-z0-9_])ktest_m64_skeleton_marker($|[^A-Za-z0-9_])' && pass "X10: temoin de l'image de test present" \
        || bad "X10: temoin ktest_m64_skeleton_marker absent (image de test ?)"
fi

# ---- X11 : périmètre M64-1 (aucune instruction de transition, aucun SIMD) --
# INS : texte de chaque instruction (après l'adresse et les octets bruts).
nins=0; for t_ in "${INS[@]}"; do [ -z "$t_" ] || nins=$((nins + 1)); done
if [ "$nins" -eq 0 ]; then
    bad "X11: desassemblage impossible ou vide (objdump statut 0, $nins instructions)"
else
    # Mots d'instruction = préfixes et mnémonique : jetons initiaux commençant
    # par une lettre, après retrait des commentaires (#...) et des noms de
    # symboles (<...>). Le premier opérande (%, $, chiffre, parenthèse) arrête
    # la lecture. Chaque mot est repéré « NUMÉRO:mot » (numéro de ligne).
    FMN_RE=':(rdmsr|wrmsr|lgdt[lqw]?|lidt[lqw]?|lldt|ltr|lmsw|clts|invlpg[a-z]*|invpcid|invd|wbinvd|iret[lqw]?|syscall|sysret[lq]?|sysenter|sysexit[lq]?|swapgs|wr[fg]sbase|xsetbv|emms|femms|ldmxcsr|stmxcsr|fxsave(64)?|fxrstor(64)?|xsave[a-z0-9]*|xrstor[a-z0-9]*|f[a-z0-9]+|v[a-z0-9]+)$'
    REG_RE='%(cr|dr|db|tr)[0-9]|%(st|mm[0-7]|xmm|ymm|zmm)'
    fmn=(); freg=(); fbad=()
    for ((i = 0; i < ${#INS[@]}; i++)); do
        l=${INS[i]}; nr=$((i + 1))
        w=${l%%#*}
        while rx "$w" '^([^<]*)<[^>]*>(.*)$'; do w="${BASH_REMATCH[1]}${BASH_REMATCH[2]}"; done
        read -ra T <<<"$w"
        for t_ in "${T[@]}"; do
            rx "$t_" '^[a-z]' || break
            if rx "$nr:$t_" "$FMN_RE" && ! rx "$nr:$t_" ':fs$'; then fmn+=("$nr:$t_"); fi
        done
        if rx "$l" "$REG_RE"; then freg+=("$nr:$l"); fi
        [[ $l != *'(bad)'* ]] || fbad+=("$nr:$l")
    done
    if [ $(( ${#fmn[@]} + ${#freg[@]} + ${#fbad[@]} )) -eq 0 ]; then
        pass "X11: aucune instruction de transition, x87, MMX, SSE ou AVX ($nins instructions)"
    else
        all=("${fmn[@]}" "${freg[@]}" "${fbad[@]}"); msg=""
        for t_ in "${all[@]:0:3}"; do msg+="$t_;"; done
        bad "X11: instruction hors perimetre M64-1 : $msg"
    fi
fi

# ---- X12 : adresses compatibles avec -mcmodel=small ------------------------
x12=1
for i in "${A_idx[@]}"; do
    a=${S_addr[i]}; s=${S_size[i]}
    { [ "$a" -gt 0 ] && [ $((a + s)) -le $((0x80000000)) ]; } || { x12=0; bad "X12: ${S_name[i]} hors de ]0, 2 GiB] (0x${S_addr_s[i]}+0x${S_size_s[i]})"; }
done
[ "$x12" -eq 1 ] && pass "X12: sections ALLOC dans ]0, 2 GiB] (-mcmodel=small, hypothese locale)"

# ---- X13 : correspondance sections -> segments, drapeaux de section -------
TY=(); for t_ in "${P_type[@]}"; do if rx "$t_" '^[A-Z_]+$'; then TY+=("$t_"); fi; done
x13=1
if [ "${#TY[@]}" -ne "${#MAP[@]}" ]; then
    x13=0; bad "X13: table de correspondance illisible (${#TY[@]} segments, ${#MAP[@]} lignes)"
else
    loadmap=""; other=""
    for ((i = 0; i < ${#TY[@]}; i++)); do
        if [ "${TY[i]}" = LOAD ]; then loadmap+="${MAP[i]}/"
        elif [ "${MAP[i]}" != "-" ]; then other+="${other:+$'\n'}${TY[i]}:${MAP[i]}"; fi
    done
    [ "$loadmap" = ".text/.rodata/.data .bss/" ] || { x13=0; bad "X13: sections des PT_LOAD = $loadmap (attendu .text/.rodata/.data .bss/)"; }
    [ -z "$other" ] || { x13=0; bad "X13: section(s) hors PT_LOAD : $other"; }
fi
for i in "${A_idx[@]}"; do
    n=${S_name[i]}; flg=${S_flg[i]}
    case "$n" in .text) want=AX ;; .rodata) want=A ;; .data|.bss) want=WA ;; *) want="?" ;; esac
    [ "$flg" = "$want" ] || { x13=0; bad "X13: drapeaux de $n = $flg (attendu $want)"; }
done
[ "$x13" -eq 1 ] && pass "X13: .text, .rodata, .data .bss dans leurs PT_LOAD ; drapeaux AX / A / WA"

printf '%s' "$OUTBUF"
out "$fail"
