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
ELF="${1:?usage: check_elf.sh build/samos.elf [--production|--test]}"
MODE="${2:-}"
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
for want in .text .rodata .data .bss .stacks; do
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
order=$(awk '$1 ~ /^\.(text|rodata|data|bss|stacks)$/ {print $3}' <<<"$secs" | tr '\n' ' ')
prev=0; ok=1
for a in $order; do [ $((0x$a)) -gt "$prev" ] || ok=0; prev=$((0x$a)); done
[ "$ok" -eq 1 ] && pass "ordre .text < .rodata < .data < .bss < .stacks" || bad "ordre des sections incorrect"
# .bss et .stacks doivent être NOBITS (aucun octet dans le fichier)
for nb in .bss .stacks; do
    awk -v n="$nb" '$1==n && $2=="NOBITS" {f=1} END {exit !f}' <<<"$secs" && pass "$nb NOBITS" || bad "$nb n'est pas NOBITS"
done

# --- 5b. Disposition des piles (K0.7-R1f-1) ------------------------------------
# Section .stacks (linker.ld), de bas en haut : garde S2 (4 KiB, réservée),
# S2 (4 KiB), garde basse S1 (4 KiB, non présente), S1 (16 KiB), garde haute
# S1 (4 KiB, réservée) ; __kernel_end juste après. Vérifié sur les symboles de
# l'image (indépendamment des assertions du linker), puis : aucun AUTRE symbole
# ne doit désigner une adresse dans une page de garde ou réservée.
symv() { nm "$ELF" | awk -v s="$1" '$3 == s { print "0x" $1; exit }'; }
lay_ok=1
expect_chain="__stacks_start=__df_guard_start __df_guard_end=__df_stack_start __df_stack_start=df_stack_area __df_stack_end=__stack_guard_start __stack_guard_end=boot_stack_bottom boot_stack_top=__stack_top_guard_start __stack_top_guard_end=__stacks_end __stacks_end=__kernel_end"
for pair in $expect_chain; do
    a="$(symv "${pair%%=*}")"; b="$(symv "${pair##*=}")"
    if [ -z "$a" ] || [ -z "$b" ] || [ $((a)) -ne $((b)) ]; then lay_ok=0; bad "piles: ${pair%%=*} ($a) != ${pair##*=} ($b)"; fi
done
size_of() { echo $(( $(symv "$2") - $(symv "$1") )); }
for spec in "__df_guard_start __df_guard_end 4096" "__df_stack_start __df_stack_end 4096" \
            "__stack_guard_start __stack_guard_end 4096" "boot_stack_bottom boot_stack_top 16384" \
            "__stack_top_guard_start __stack_top_guard_end 4096"; do
    set -- $spec
    [ "$(size_of "$1" "$2")" -eq "$3" ] || { lay_ok=0; bad "piles: [$1, $2) = $(size_of "$1" "$2") octets, attendu $3"; }
    [ $(( $(symv "$1") % 4096 )) -eq 0 ] || { lay_ok=0; bad "piles: $1 non aligne sur 4 KiB"; }
done
[ "$lay_ok" -eq 1 ] && pass "piles: .stacks = garde S2 | S2 4 KiB | garde S1 | S1 16 KiB | garde haute, alignees 4 KiB, contigues"
# K0.7-R1h (F6) — Liste EXACTE des symboles autorisés dans une page de garde,
# chacun uniquement à l'adresse de DÉBUT de cette garde (ce sont des bornes de
# la disposition, égales au début de la page). Auparavant, des PRÉFIXES
# (boot_stack_, __stack_guard_...) étaient exclus : un symbole intrus nommé
# boot_stack_intruder dans la garde haute passait inaperçu (audit R1g, F6 ;
# démontré en R1h). Les bornes de FIN (__df_guard_end, __stack_guard_end,
# __stack_top_guard_end...) valent la fin exclue et ne sont pas dans la plage.
#   garde S2       : __stacks_start, __df_guard_start
#   garde basse S1 : __df_stack_end, __stack_guard_start
#   garde haute S1 : boot_stack_top, __stack_top_guard_start
intruders=""
for spec in "__df_guard_start __df_guard_end __stacks_start,__df_guard_start" \
            "__stack_guard_start __stack_guard_end __df_stack_end,__stack_guard_start" \
            "__stack_top_guard_start __stack_top_guard_end boot_stack_top,__stack_top_guard_start"; do
    set -- $spec; lo=$(( $(symv "$1") )); hi=$(( $(symv "$2") )); allow=",$3,"
    intruders="$intruders$(nm "$ELF" | awk -v lo="$lo" -v hi="$hi" -v allow="$allow" '
        {
            a = 0; h = tolower($1)
            for (i = 1; i <= length(h); i++) a = a * 16 + index("0123456789abcdef", substr(h, i, 1)) - 1
            if (a >= lo && a < hi && !(a == lo && index(allow, "," $3 ",") > 0)) printf " %s", $3
        }')"
done
[ -z "$intruders" ] && pass "piles: aucun objet dans les pages de garde (liste exacte de bornes autorisees)" \
                    || bad "piles: objets dans une page de garde ou reservee :$intruders"

# --- 5c. Pages des structures critiques (K0.7-P4b) -------------------------------
# Chaque page fait exactement 4 KiB, alignée, et ne contient QUE les symboles
# listés (plus ses bornes, autorisées à l'adresse de début). Une page candidate
# à la lecture seule (kro_*) ne doit partager sa page avec AUCUN objet qui doit
# rester inscriptible. Liste exacte, sans préfixe (leçon de R1h, F6).
p4_ok=1
for spec in "__kcpu_start __kcpu_end g_gdt,g_tss_main,g_tss_df __bss_start,__kcpu_start" \
            "__kro_idt_start __kro_idt_end g_idt __kcpu_end,__kro_idt_start" \
            "__kro_handlers_start __kro_handlers_end g_handlers,g_irq_handlers __kro_idt_end,__kro_handlers_start" \
            "__kro_vmm_start __kro_vmm_end g_idmap_top,g_pd __kro_handlers_end,__kro_vmm_start"; do
    set -- $spec
    lo=$(symv "$1"); hi=$(symv "$2")
    if [ -z "$lo" ] || [ -z "$hi" ]; then p4_ok=0; bad "P4b: bornes absentes ($1, $2)"; continue; fi
    lo=$((lo)); hi=$((hi))
    if [ $((hi - lo)) -ne 4096 ] || [ $((lo % 4096)) -ne 0 ]; then p4_ok=0; bad "P4b: [$1, $2) n'est pas une page alignee"; fi
    found=$(nm "$ELF" | awk -v lo="$lo" -v hi="$hi" -v objs=",$3," -v bounds=",$4," '
        {
            a = 0; h = tolower($1)
            for (i = 1; i <= length(h); i++) a = a * 16 + index("0123456789abcdef", substr(h, i, 1)) - 1
            if (a < lo || a >= hi) next
            if (index(objs, "," $3 ",") > 0) { seen[$3] = 1; next }
            if (a == lo && index(bounds, "," $3 ",") > 0) next
            printf " intrus:%s", $3
        }
        END { n = split(substr(objs, 2, length(objs) - 2), want, ","); for (k = 1; k <= n; k++) if (!(want[k] in seen)) printf " absent:%s", want[k] }')
    if [ -n "$found" ]; then p4_ok=0; bad "P4b: page [$1, $2) :$found"; fi
done
[ "$p4_ok" -eq 1 ] && pass "P4b: pages GDT/TSS, IDT, gestionnaires, racines VMM : contenu exact, une page chacune"

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

# --- 7. Séparation production / test (K0.7) --------------------------------------
# Symboles qui ne doivent exister QUE dans l'image de test : autotests, modes de
# faute volontaire, sortie QEMU. Leur présence dans l'image de production
# signifierait qu'un mode destructif y est accessible.
TEST_ONLY_SYMS="ktest_run_all ktest_fatal_mode qemu_exit ktest_check ktest_s2_prefill ptgate_write_entry ptgate_set_pte paging_gate_test_write paging_gate_test_set_active ktest_suite_pmm_trust"
# K0.7-P4c, P4e-1, P4e-2 : écritures de CR0 autorisées, par LOCALISATION et
# par FONCTION (pas seulement par nombre). Argument : prod | test.
#   paging_init   : 1 écriture, précédée de « or $0x80010000 » (PG|WP) ;
#   pt_gate_write : 2 écritures (fenêtre du passage des PDE/PTE) :
#     - la 1re suit immédiatement « and $0xfffeffff,%R » sur le même
#       registre (retrait de WP seul) ;
#     - entre les deux : EXACTEMENT une instruction, un stockage
#       « mov %r,(%r) » (l'unique écriture de la fenêtre) ;
#     - un « cli » précède la 1re ; aucun « sti » entre les deux.
#   image de TEST seulement : ptgate_write_entry (prototype P4c, 2 écritures,
#   WP seul retiré) ; ktest_check / ktest_run_all (rétablissement de WP par
#   l'invariant global, « or $0x10000 » sur le même registre).
# Toute autre écriture de CR0 est un échec.
cr0_report() {
    objdump -d --no-show-raw-insn "$ELF" | awk -v mode="$1" '
            /^[0-9a-f]+ <[^>]+>:$/ { fn = $2; gsub(/[<>:]/, "", fn); n = 0; next }
            { sub(/^ *[0-9a-f]+:\t/, ""); ins[fn, ++n] = $0; cnt[fn] = n }
            /mov +%e[a-z]+,%cr0/ { w[fn] = w[fn] " " n; total++ }
            END {
                for (f in w) {
                    k = split(w[f], at, " ")
                    if (f == "paging_init") {
                        if (k != 1) { print "paging_init:" k; continue }
                        ok = 0; for (d = 1; d <= 3; d++) if (ins[f, at[1] - d] ~ /or +\$0x80010000,/) ok = 1
                        if (!ok) print "paging_init:sans-or-PG-WP"
                    } else if (f == "pt_gate_write") {
                        if (k != 2) { print "pt_gate_write:" k; continue }
                        a = at[1]; b = at[2]
                        split(ins[f, a], wa, ","); reg = wa[1]; sub(/^mov +/, "", reg)
                        if (ins[f, a - 1] !~ ("and +\\$0xfffeffff," reg "$")) print "pt_gate_write:sans-and-WP"
                        if (b != a + 2 || ins[f, a + 1] !~ /^mov +%e[a-z]+,\(%e[a-z]+\)$/) print "pt_gate_write:fenetre-non-unitaire"
                        cli = 0; for (x = 1; x < a; x++) if (ins[f, x] ~ /^cli/) cli = 1
                        if (!cli) print "pt_gate_write:sans-cli"
                        for (x = a; x <= b; x++) if (ins[f, x] ~ /^sti/) print "pt_gate_write:sti-dans-fenetre"
                    } else if (mode == "test" && f == "ptgate_write_entry") {
                        # Prototype P4c (image de test) : 2 écritures, WP seul retiré.
                        if (k != 2) { print f ":" k; continue }
                        split(ins[f, at[1]], wa, ","); reg = wa[1]; sub(/^mov +/, "", reg)
                        if (ins[f, at[1] - 1] !~ ("and +\\$0xfffeffff," reg "$")) print f ":sans-and-WP"
                    } else if (mode == "test" && (f == "ktest_check" || f == "ktest_run_all")) {
                        # Invariant global de ktest (P4e-1) : rétablit WP seulement.
                        if (k != 1) { print f ":" k; continue }
                        split(ins[f, at[1]], wa, ","); reg = wa[1]; sub(/^mov +/, "", reg)
                        ok = 0; for (d = 1; d <= 3; d++) if (ins[f, at[1] - d] ~ ("or +\\$0x10000," reg "$")) ok = 1
                        if (!ok) print f ":sans-or-WP"
                    } else {
                        print f ":" k
                    }
                }
            }'
}
# K0.7-P4e-2 — seal_table_frame (deux images) : l'écriture par le passage est
# SUIVIE d'un INVLPG. Sous QEMU, tout changement de CR0.WP vide le TLB : un
# INVLPG oublié resterait invisible aux autotests, mais pas sur un processeur
# réel (MOV vers CR0 n'invalide le TLB que si PG passe de 1 à 0, SDM vol. 3A
# §4.10.4.1). Le contrôle porte donc sur le code machine.
sealrep=$(objdump -d --no-show-raw-insn "$ELF" | awk '
    /^[0-9a-f]+ <[^>]+>:$/ { infn = ($2 == "<seal_table_frame>:"); if (infn) seen = 1; next }
    infn && /call +[0-9a-f]+ <pt_gate_write>/ { gate = 1 }
    infn && gate && /invlpg/ { inv = 1 }
    END { if (!seen) print "absente"; else if (!gate) print "sans-appel-pt_gate_write"; else if (!inv) print "sans-invlpg" }')
if [ -z "$sealrep" ]; then
    pass "seal_table_frame : INVLPG apres l'ecriture par pt_gate_write"
else
    bad "seal_table_frame:$sealrep"
fi
syms=$(nm "$ELF" | awk '{print $3}')
case "$MODE" in
    --production)
        for s in $TEST_ONLY_SYMS; do
            if grep -qx "$s" <<<"$syms"; then bad "production: symbole de test present ($s)"; fi
        done
        cr0rep=$(cr0_report prod)
        if [ -z "$cr0rep" ]; then
            pass "production: CR0 ecrit seulement par paging_init (PG|WP) et la fenetre unitaire de pt_gate_write"
        else
            bad "production: ecritures de CR0 non conformes : $(echo $cr0rep)"
        fi
        # Aucune instruction écrivant sur les ports QEMU 0xE9 / 0xF4.
        if objdump -d --no-show-raw-insn "$ELF" | grep -qE 'out +%(al|ax|eax),\$0x(e9|f4)'; then
            bad "production: ecriture sur un port QEMU (0xE9/0xF4)"
        else
            pass "production: aucun symbole de test, aucun acces aux ports QEMU"
        fi
        ;;
    --test)
        for s in $TEST_ONLY_SYMS; do
            grep -qx "$s" <<<"$syms" || bad "test: symbole attendu absent ($s)"
        done
        pass "test: autotests et modes de faute presents"
        cr0rep=$(cr0_report test)
        if [ -z "$cr0rep" ]; then
            pass "test: CR0 ecrit seulement par paging_init, pt_gate_write, le prototype P4c et l'invariant WP de ktest"
        else
            bad "test: ecritures de CR0 non conformes : $(echo $cr0rep)"
        fi
        ;;
    "") ;;
    *) bad "mode inconnu: $MODE" ;;
esac

exit $fail
