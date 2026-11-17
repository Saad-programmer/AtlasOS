#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tools/check_elf.sh
# Vérifications statiques de l'image noyau (aucune exécution) :
#   1. En-tête ELF : ELF32, i386, exécutable, point d'entrée = _start
#   2. En-tête Multiboot 1 : présent dans les 8192 premiers octets, aligné sur
#      4 octets, somme magic+flags+checksum = 0 (mod 2^32)
#   3. Aucun symbole indéfini (pas de dépendance cachée à libgcc/libc)
#   4. Segments PT_LOAD : aucun segment W+X, chargés à >= 1 MiB, vaddr == paddr
#   5. memset/memcpy ne s'appellent pas eux-mêmes (piège de l'optimiseur)
# Code de sortie : 0 si tout passe, 1 sinon.
# =============================================================================
set -u
ELF="${1:?usage: check_elf.sh build/samos.elf}"
fail=0
pass() { printf '[PASS] %s\n' "$1"; }
bad()  { printf '[FAIL] %s\n' "$1"; fail=1; }

[ -f "$ELF" ] || { echo "[FAIL] fichier absent: $ELF"; exit 1; }

# --- 1. En-tête ELF ----------------------------------------------------------
hdr=$(readelf -h "$ELF")
grep -q 'Class:[[:space:]]*ELF32' <<<"$hdr" && pass "ELF32" || bad "classe ELF != ELF32"
grep -q 'Machine:[[:space:]]*Intel 80386' <<<"$hdr" && pass "machine i386" || bad "machine != i386"
grep -q 'Type:[[:space:]]*EXEC' <<<"$hdr" && pass "type EXEC" || bad "type != EXEC"
entry=$(awk '/Entry point address/ {print $4}' <<<"$hdr")
start=$(nm "$ELF" | awk '$3=="_start" {print "0x" $1}')
if [ -n "$start" ] && [ $((entry)) -eq $((start)) ]; then
    pass "point d'entree = _start ($entry)"
else
    bad "point d'entree ($entry) != _start ($start)"
fi

# --- 2. En-tête Multiboot ----------------------------------------------------
# od affiche des mots de 32 bits (petit-boutiste, comme le CPU) avec leur offset décimal.
# Le calcul de la somme est fait par bash (awk de Debian/Ubuntu = mawk, sans strtonum).
mb=$(od -A d -t x4 -v -N 8192 "$ELF" | awk '
    NF > 1 {
        off = $1
        for (i = 2; i <= NF; i++) { w[off + (i - 2) * 4] = $i }
        if (off + (NF - 2) * 4 > max) max = off + (NF - 2) * 4
    }
    END {
        for (o = 0; o <= max; o += 4) {
            if (w[o] == "1badb002") { print o, w[o+4], w[o+8]; exit }
        }
    }')
if [ -n "$mb" ]; then
    read -r mb_off mb_flags mb_ck <<<"$mb"
    mb_sum=$(( (0x1badb002 + 0x$mb_flags + 0x$mb_ck) & 0xFFFFFFFF ))
fi
if [ -z "$mb" ]; then
    bad "en-tete Multiboot introuvable dans les 8192 premiers octets"
else
    pass "en-tete Multiboot a l'offset $mb_off (flags=0x$mb_flags checksum=0x$mb_ck)"
    [ $((mb_off % 4)) -eq 0 ] && pass "en-tete aligne sur 4" || bad "en-tete non aligne"
    [ "$mb_sum" -eq 0 ] && pass "somme magic+flags+checksum = 0" || bad "checksum invalide (somme=$mb_sum)"
    # bit 16 (a.out kludge) ne doit pas être positionné : nous utilisons ELF
    [ $(( 0x$mb_flags & 0x10000 )) -eq 0 ] && pass "pas de a.out kludge (ELF)" || bad "bit 16 positionne"
fi

# --- 3. Symboles indéfinis ---------------------------------------------------
undef=$(nm -u "$ELF")
if [ -z "$undef" ]; then pass "aucun symbole indefini"; else bad "symboles indefinis: $undef"; fi

# --- 4. Segments -------------------------------------------------------------
# Format readelf -lW : Type Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align
seg=$(readelf -lW "$ELF" | awk '$1=="LOAD" {
        flg = ""; for (i = 8; i < NF; i++) flg = flg $i
        print $3, $4, flg }')
nload=0
while read -r va pa flg; do
    [ -z "$va" ] && continue
    nload=$((nload + 1))
    if [[ "$flg" == *W* && "$flg" == *E* ]]; then bad "segment $va W+X ($flg)"; fi
    [ $((va)) -ge $((0x100000)) ] || bad "segment $va sous 1 MiB"
    [ $((va)) -eq $((pa)) ] || bad "segment $va : vaddr != paddr ($pa)"
done <<<"$seg"
[ "$nload" -ge 2 ] && pass "$nload segments PT_LOAD, aucun W+X, >= 1 MiB, identite" || bad "segments PT_LOAD inattendus"

# --- 5. Sections (K0.2) --------------------------------------------------------
# Format readelf -SW : [Nr] Name Type Addr Off Size ES Flg Lk Inf Al
secs=$(readelf -SW "$ELF" | sed 's/^ *\[ *[0-9]*\] *//' | awk 'NF >= 9 && $1 ~ /^\./ {print $1, $2, $3, $NF}')
for want in .text .rodata .data .bss .bootstack; do
    line=$(awk -v n="$want" '$1==n' <<<"$secs")
    if [ -z "$line" ]; then bad "section $want absente"; continue; fi
    read -r _ _ addr al <<<"$line"
    if [ $((0x$addr % 4096)) -eq 0 ]; then pass "section $want a 0x$addr (alignee 4 KiB)"; else bad "section $want non alignee (0x$addr)"; fi
done
# Sections interdites dans un noyau statique non PIC : leur présence révélerait
# du code dynamique/PIC ou des données de déroulement inattendues.
forbidden=$(awk '$1 ~ /^\.(got|plt|dynamic|interp|dynsym|dynstr|rel|rela|eh_frame|note|multiboot$)/ {print $1}' <<<"$secs")
if [ -z "$forbidden" ]; then pass "aucune section dynamique/PIC/.eh_frame/.note"; else bad "sections inattendues: $forbidden"; fi
# Ordre croissant des adresses des sections chargées
order=$(awk '$1 ~ /^\.(text|rodata|data|bss|bootstack)$/ {print $3}' <<<"$secs" | tr '\n' ' ')
prev=0; ok=1
for a in $order; do [ $((0x$a)) -gt "$prev" ] || ok=0; prev=$((0x$a)); done
[ "$ok" -eq 1 ] && pass "ordre .text < .rodata < .data < .bss < .bootstack" || bad "ordre des sections incorrect"
# .bss et .bootstack doivent être NOBITS (aucun octet dans le fichier)
for nb in .bss .bootstack; do
    awk -v n="$nb" '$1==n && $2=="NOBITS" {f=1} END {exit !f}' <<<"$secs" && pass "$nb NOBITS" || bad "$nb n'est pas NOBITS"
done

# --- 6. Récursion involontaire de memset/memcpy ---------------------------------
for fn in memset memcpy memmove; do
    if objdump -d "$ELF" --no-show-raw-insn | awk -v f="$fn" '
        $0 ~ "<" f ">:$" { inside = 1; next }
        inside && /^$/ { inside = 0 }
        inside && $0 ~ "call.*<" f ">" { found = 1 }
        END { exit !found }'; then
        bad "$fn s'appelle lui-meme"
    else
        pass "$fn sans auto-appel"
    fi
done

exit $fail
