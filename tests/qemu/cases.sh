# shellcheck shell=bash
# =============================================================================
# SAMOS / AtlasOS — tests/qemu/cases.sh
# Liste des cas de test QEMU, chargée par run.sh. Un cas par appel :
#   run_case NOM "LIGNE_DE_COMMANDE" STATUT_ATTENDU "motif 1" "motif 2" ...
# Motifs : voir l'en-tête de run.sh ("texte", "serial:", "re:", "!").
# IMG=prod : image de production ; par défaut : image de test.
# =============================================================================

# Démarrage normal de l'image de PRODUCTION : le noyau reste ensuite inactif,
# QEMU ne se termine pas seul : timeout (124) attendu, sans triple fault.
IMG=prod TIMEOUT=3 run_case boot_halt "" 124 \
    "SAMOS: kernel entry" \
    "SAMOS: multiboot magic ok" \
    "SAMOS: kernel image 0x00100000-" \
    "---G stack-guard" \
    "SAMOS: K0.6 boot complete, paging on, interrupts enabled" \
    "paging: enabled (CR0.PG|WP), identity map 0x00001000-0x07fe0000" \
    "guard page unmapped (MMU-enforced)" \
    "irq: 8259 PIC remapped to vectors 0x20-0x2f" \
    "timer: PIT channel 0, 100 Hz (divisor 11932)" \
    "keyboard: 8042 self-test ok, PS/2 keyboard reset ok, IRQ1 enabled" \
    "cpu: GDT loaded" \
    "cpu: IDT loaded (32 exception gates, #DF task gate)" \
    "SAMOS: page directory and 32 page tables sealed read-only (0 writable, 0 aliases)" \
    "!KERNEL PANIC"

# K0.3 — Le pilote série transmet réellement : motifs lus dans le seul journal COM1.
# K0.7 — Image de production : aucun octet sur le port debugcon 0xE9.
IMG=prod TIMEOUT=3 run_case serial_output "" 124 \
    "serial:SAMOS: kernel entry" \
    "serial:console: serial COM1 115200 8N1 VGA 80x25 (production image)" \
    "!console: debugcon"
[ -s "$OUT/serial_output.debugcon.log" ] && { echo "[FAIL] production: octets écrits sur debugcon"; fail=1; }

# K0.7 — L'image de production REFUSE les modes de test, y compris destructifs :
# elle démarre normalement, sans panique ni sortie QEMU.
IMG=prod TIMEOUT=3 run_case prod_refuses_test "test=wtext" 124 \
    "serial:test mode requested but not available in the production image" \
    "serial:SAMOS: K0.6 boot complete" \
    "!KERNEL PANIC" "![TEST]"
    "serial:SAMOS: K0.6 boot complete"

# Autotests noyau, toutes suites.
run_case selftest "test" 33 \
    "[TEST] suite K0.1/K0.2 boot" \
    "[TEST] suite K0.2 layout" \
    "[TEST] suite K0.3 console" \
    "[TEST] suite K0.4 cpu" \
    "[TEST] suite K0.5 irq" \
    "[TEST] suite K0.6 mm" \
    "re:suite K0.1/K0.2 boot done: [1-9][0-9]* checks, 0 failures" \
    "re:suite K0.2 layout done: [1-9][0-9]* checks, 0 failures" \
    "re:suite K0.3 console done: [1-9][0-9]* checks, 0 failures" \
    "re:suite K0.4 cpu done: [1-9][0-9]* checks, 0 failures" \
    "re:suite K0.5 irq done: [1-9][0-9]* checks, 0 failures" \
    "re:suite K0.6 mm done: [1-9][0-9]* checks, 0 failures" \
    "[TEST] ALL TESTS PASSED" \
    "![FAIL]"

# K0.3 — Chemin de panique : message, emplacement, trace de pile, statut 37.
run_case panic_path "test=panic" 37 \
    "serial:*** KERNEL PANIC: deliberate test panic (42)" \
    "serial:*** at kernel/tests/ktest.c:" \
    "serial:*** stack trace (" \
    "serial:***   #1 0x001" \
    "serial:*** system halted"

# Mode de test inconnu : refus explicite (statut d'échec), pas de panique.
run_case unknown_mode "test=nope" 35 \
    "[TEST] unknown test mode 'nope'"

# ---------------------------------------------------------------------------
# K0.4 — Exceptions fatales : rapport complet puis panique (statut 37).
# ---------------------------------------------------------------------------
run_case exc_ud "test=ud" 37 \
    "serial:[TEST] critical structures sealed" \
    "serial:*** CPU exception 6: Invalid Opcode (#UD), error code 0x00000000" \
    "serial:EIP=0x001" "serial:CS=0008" "serial:CR0=" \
    "serial:KERNEL PANIC: unhandled Invalid Opcode (#UD)"

run_case exc_de "test=de" 37 \
    "serial:*** CPU exception 0: Divide Error (#DE)" \
    "serial:KERNEL PANIC: unhandled Divide Error (#DE)"

# MOV DS, 0x0F00 : index 480 au-delà de la limite de la GDT (5 entrées).
run_case exc_gp "test=gp" 37 \
    "serial:*** CPU exception 13: General Protection (#GP), error code 0x00000f00" \
    "serial:selector error: GDT index 480" \
    "serial:KERNEL PANIC: unhandled General Protection (#GP)"

# INT 0x2E (IRQ 14, inutilisée) : porte de type valide mais P = 0 => #NP,
# code = 0x2E*8 + 2 (bit IDT) = 0x172.
run_case exc_np "test=np" 37 \
    "serial:*** CPU exception 11: Segment Not Present (#NP), error code 0x00000172" \
    "serial:selector error: IDT index 46" \
    "serial:KERNEL PANIC: unhandled Segment Not Present (#NP)"

# INT 0x80 : entrée entièrement nulle (type de porte invalide). Le CPU vérifie
# le TYPE avant le bit P => #GP (et non #NP), code = 0x80*8 + 2. (Constaté en K0.4.)
run_case exc_int_invalid "test=intbad" 37 \
    "serial:*** CPU exception 13: General Protection (#GP), error code 0x00000402" \
    "serial:selector error: IDT index 128"

# Double faute : capturée par la porte de tâche, sur la pile dédiée, SANS reset.
run_case exc_df "test=df" 37 \
    "serial:*** DOUBLE FAULT (#DF) caught by task gate, running on dedicated stack" \
    "serial:interrupted task: EIP=0x001" \
    "serial:KERNEL PANIC: double fault" \
    "serial:double-fault stack sentinels: intact" \
    "!sentinels CORRUPTED" \
    "!CPU exception 11"

# Triple fault volontaire : le banc de test DOIT le détecter (« Triple fault »
# dans le journal QEMU). Avec -no-reboot, QEMU s'arrête (statut 0).
EXPECT_TRIPLE=1 run_case exc_triple "test=triple" 0 \
    "[TEST] fatal mode 'triple'" \
    "!KERNEL PANIC"

# ---------------------------------------------------------------------------
# K0.5 — Clavier de bout en bout : frappes réelles injectées par le moniteur
# QEMU -> contrôleur 8042 émulé -> IRQ 1 -> file circulaire -> décodeur.
# Majuscules par Shift, chiffres et ponctuation, Entrée termine la ligne.
# ---------------------------------------------------------------------------
TIMEOUT=20 run_case_keys keyboard_e2e "test=kbd" 33 \
    "shift-s a m o s spc shift-k 0 dot 5 spc shift-1 ret" \
    "serial:[TEST] critical structures sealed" \
    "serial:[TEST] kbd ready" \
    "serial:[TEST] kbd line: 'Samos K0.5 !'" \
    "serial:dropped=0"

# ---------------------------------------------------------------------------
# K0.6 — Protections mémoire : chaque violation est diagnostiquée précisément.
# ---------------------------------------------------------------------------
run_case pf_null "test=null" 37 \
    "serial:*** CPU exception 14: Page Fault (#PF), error code 0x00000000" \
    "serial:page not present, read access, supervisor mode, address CR2=0x00000000" \
    "serial:diagnosis: NULL pointer dereference"

run_case pf_write_text "test=wtext" 37 \
    "serial:error code 0x00000003" \
    "serial:protection violation, write access" \
    "serial:diagnosis: write to read-only kernel code (.text)"

run_case pf_write_rodata "test=wrodata" 37 \
    "serial:diagnosis: write to read-only kernel data (.rodata)"

run_case pf_unmapped "test=unmapped" 37 \
    "serial:address CR2=0xe0000000" \
    "serial:diagnosis: access to an unmapped address (outside identity-mapped RAM)"

# Débordement de pile : page de garde (K0.2) + pagination (K0.6) => #PF non
# livrable => #DF attrapée par la porte de tâche (K0.4), SANS triple fault.
run_case stack_overflow "test=stack" 37 \
    "serial:[TEST] critical structures sealed" \
    "serial:*** DOUBLE FAULT (#DF) caught by task gate, running on dedicated stack" \
    "serial:double-fault stack sentinels: intact" \
    "!sentinels CORRUPTED" \
    "serial:kernel stack overflow (guard page below the boot stack)" \
    "serial:KERNEL PANIC: double fault"

# K0.7-R1f-2 — Garde haute de S1 : écriture DIRECTE (non une sonde) sur le
# premier puis le dernier mot de [__stack_top_guard_start, +4 KiB), depuis un
# cadre sur S1 valide. Attendu : #PF code 0x2 (non présente, écriture), panic
# identifiable, statut 37, ni triple fault ni panique récursive. L'adresse
# n'est pas codée en dur : cr2_matches compare le CR2 affiché à l'adresse
# calculée depuis la table des symboles de l'image (nm).
cr2_matches() {   # cr2_matches CAS SYMBOLE DÉCALAGE
    local slog="$OUT/$1.serial.log" base want got
    total=$((total + 1))
    base=$(nm "$TEST_KERNEL" | awk -v s="$2" '$3 == s { print $1 }')
    want=$(printf '0x%08x' $(( 0x${base:-0} + $3 )))
    got=$(tr -d '\r' < "$slog" 2>/dev/null | sed -n 's/.*page fault: .*address CR2=\(0x[0-9a-f]*\).*/\1/p' | head -1)
    if [ -n "$base" ] && [ "$got" = "$want" ]; then
        echo "[PASS] $1_cr2 (CR2=$got = $2 + $3)"
        passed=$((passed + 1))
    else
        echo "[FAIL] $1_cr2 (CR2=${got:-absent}, attendu $want)"
        fail=1
    fi
}
run_case topguard "test=topguard" 37 \
    "serial:[TEST] writing to the S1 top guard at 0x" \
    "serial:*** CPU exception 14: Page Fault (#PF), error code 0x00000002" \
    "serial:page not present, write access, supervisor mode" \
    "serial:KERNEL PANIC: page fault at 0x" \
    "serial:access above the boot stack top (S1 top guard page)" \
    "!top guard write NOT trapped" \
    "!RECURSIVE PANIC" \
    "!DOUBLE FAULT"
cr2_matches topguard __stack_top_guard_start 0
run_case topguard_end "test=topguard_end" 37 \
    "serial:*** CPU exception 14: Page Fault (#PF), error code 0x00000002" \
    "serial:access above the boot stack top (S1 top guard page)" \
    "!top guard write NOT trapped" \
    "!RECURSIVE PANIC" \
    "!DOUBLE FAULT"
cr2_matches topguard_end __stack_top_guard_start 4092

# K0.7-R1f-3 — Garde basse de S2 : même méthode, écriture directe DEPUIS S1
# (la #PF est livrée sur S1). Aucun débordement réel de S2 : il finirait en
# triple fault (R1e §5.2), ce que ces cas ne mesurent pas.
run_case dfguard "test=dfguard" 37 \
    "serial:[TEST] writing to the S2 guard at 0x" \
    "serial:*** CPU exception 14: Page Fault (#PF), error code 0x00000002" \
    "serial:page not present, write access, supervisor mode" \
    "serial:access to the double-fault stack guard page (below S2)" \
    "!S2 guard write NOT trapped" \
    "!RECURSIVE PANIC" \
    "!DOUBLE FAULT"
cr2_matches dfguard __df_guard_start 0
run_case dfguard_end "test=dfguard_end" 37 \
    "serial:*** CPU exception 14: Page Fault (#PF), error code 0x00000002" \
    "serial:access to the double-fault stack guard page (below S2)" \
    "!S2 guard write NOT trapped" \
    "!RECURSIVE PANIC" \
    "!DOUBLE FAULT"
cr2_matches dfguard_end __df_guard_start 4092

# K0.7-R1h (F4) — Panique récursive : la première panique formate une chaîne
# située à une adresse non projetée ; la #PF appelle panic depuis son
# gestionnaire. panic_at doit s'arrêter après UN message « RECURSIVE PANIC »
# (sortie minimale, debugcon et série), statut 37 : ni boucle (délai), ni
# débordement de S1 (double faute), ni triple fault.
count_is() {   # count_is CAS TEXTE NOMBRE (dans la sortie série)
    local slog="$OUT/$1.serial.log" n
    total=$((total + 1))
    n=$(tr -d '\r' < "$slog" 2>/dev/null | grep -cF -- "$2")
    if [ "$n" -eq "$3" ]; then
        echo "[PASS] $1_count (« $2 » : $n)"
        passed=$((passed + 1))
    else
        echo "[FAIL] $1_count (« $2 » : $n, attendu $3)"
        fail=1
    fi
}
run_case recursive_panic "test=recursive_panic" 37 \
    "serial:[TEST] fatal mode 'recursive_panic'" \
    "serial:*** KERNEL PANIC: " \
    "serial:*** CPU exception 14: Page Fault (#PF), error code 0x00000000" \
    "serial:address CR2=0xe0000000" \
    "serial:SAMOS: RECURSIVE PANIC" \
    "RECURSIVE PANIC" \
    "!DOUBLE FAULT" \
    "!NESTED CPU exception"
count_is recursive_panic "RECURSIVE PANIC" 1
count_is recursive_panic "KERNEL PANIC" 1

# K0.7-P4d — Écriture DIRECTE dans chacune des trois pages scellées, après le
# scellement de production (ktest_run_all). Attendu : #PF code 0x3 (page
# présente, écriture, superviseur), diagnostic dédié, statut 37, ni triple
# fault ni panique récursive ; CR2 = début de la page, lu par nm.
for sp in "sealidt __kro_idt_start" "sealhandlers __kro_handlers_start" "sealvmm __kro_vmm_start"; do
    set -- $sp
    run_case "$1" "test=$1" 37 \
        "serial:[TEST] critical structures sealed" \
        "serial:[TEST] writing to sealed page ($1)" \
        "serial:*** CPU exception 14: Page Fault (#PF), error code 0x00000003" \
        "serial:protection violation, write access, supervisor mode" \
        "serial:write to a sealed kernel structure (IDT, handler tables or VMM roots)" \
        "!sealed write NOT trapped" \
        "!RECURSIVE PANIC" \
        "!DOUBLE FAULT"
    cr2_matches "$1" "$2" 0
done

# K0.7-P4e-2 — Écriture DIRECTE dans le répertoire scellé, dans une table
# scellée au démarrage, et dans une table créée APRÈS le scellement. La valeur
# écrite est la valeur actuelle de l'entrée (rien n'est détruit avant la
# faute). Attendu : #PF code 0x3, diagnostic dédié, statut 37 ; CR2 égal à
# l'adresse affichée par le mode (dynamique : allouée par le PMM).
cr2_printed() {   # cr2_printed CAS
    local slog="$OUT/$1.serial.log" want got
    total=$((total + 1))
    want=$(tr -d '\r' < "$slog" 2>/dev/null | sed -n 's/.*writing to sealed page table ([a-z]*) at \(0x[0-9a-f]*\).*/\1/p' | head -1)
    got=$(tr -d '\r' < "$slog" 2>/dev/null | sed -n 's/.*page fault: .*address CR2=\(0x[0-9a-f]*\).*/\1/p' | head -1)
    if [ -n "$want" ] && [ "$got" = "$want" ]; then
        echo "[PASS] $1_cr2 (CR2=$got = adresse visée)"
        passed=$((passed + 1))
    else
        echo "[FAIL] $1_cr2 (CR2=${got:-absent}, attendu ${want:-absent})"
        fail=1
    fi
}
for m in sealpd sealpt sealnewpt; do
    run_case "$m" "test=$m" 37 \
        "serial:[TEST] critical structures sealed" \
        "serial:[TEST] writing to sealed page table ($m) at 0x" \
        "serial:*** CPU exception 14: Page Fault (#PF), error code 0x00000003" \
        "serial:protection violation, write access, supervisor mode" \
        "serial:write to a sealed page table or page directory" \
        "!sealed page-table write NOT trapped" \
        "!cannot create a page table" \
        "!RECURSIVE PANIC" \
        "!DOUBLE FAULT"
    cr2_printed "$m"
done

# K0.7-P4f-B1 — Mode ISOLÉ : falsification de descripteurs PMM (D1). Le VMM
# doit refuser tout alias d'une structure de pagination ou de l'image du
# noyau, quel que soit le type affiché par le PMM.
run_case pmmtrust "test=pmmtrust" 33 \
    "serial:[TEST] suite K0.7 P4f-B1 PMM trust boundary (isolated) done:" \
    "serial:[TEST] D1 first table frame=" \
    "serial:[TEST] D1 page directory frame=" \
    "serial:[TEST] ALL TESTS PASSED" \
    "![FAIL]" "!KERNEL PANIC"

# K0.7-P4f-B2 — Mode ISOLÉ : intégrité du répertoire (D3). Substitution,
# duplication, table non scellée, table déjà aliasée, falsification du PMM :
# tout est refusé sans effet ; la transition absente -> table neuve scellée
# reste possible ; l'audit compte une PDE en double écrite hors du passage.
run_case pdeint "test=pdeint" 33 \
    "serial:[TEST] suite K0.7 P4f-B2 page-directory integrity (isolated) done:" \
    "serial:[TEST] D3 audit after a duplicate PDE written outside pt_gate_write: dup_pdes=1" \
    "serial:[TEST] ALL TESTS PASSED" \
    "![FAIL]" "!KERNEL PANIC"
# K0.7-P4f-B2 — Table publiée marquée FREE par falsification : la remise à
# zéro par le PMM faute sur l'identité scellée, avant toute publication.
run_case pdesteal "test=pdesteal" 37 \
    "serial:[TEST] pdesteal: published table 0x" \
    "serial:*** CPU exception 14: Page Fault (#PF), error code 0x00000003" \
    "serial:write to a sealed page table or page directory" \
    "!active table reused NOT trapped" \
    "!preconditions not met" \
    "!RECURSIVE PANIC" \
    "!DOUBLE FAULT"
total=$((total + 1))
want=$(tr -d '\r' < "$OUT/pdesteal.serial.log" 2>/dev/null | sed -n 's/.*pdesteal: published table \(0x[0-9a-f]*\) marked FREE.*/\1/p' | head -1)
got=$(tr -d '\r' < "$OUT/pdesteal.serial.log" 2>/dev/null | sed -n 's/.*page fault: .*address CR2=\(0x[0-9a-f]*\).*/\1/p' | head -1)
if [ -n "$want" ] && [ "$got" = "$want" ]; then
    echo "[PASS] pdesteal_cr2 (CR2=$got = table falsifiée)"; passed=$((passed + 1))
else
    echo "[FAIL] pdesteal_cr2 (CR2=${got:-absent}, attendu ${want:-absent})"; fail=1
fi

# K0.7-P4f-B3 — Création des tables de pages (D4). Mode ISOLÉ tablelife :
# erreurs ordinaires (mémoire épuisée réellement, paramètres, projections
# interdites, région occupée, références saturées) sans aucun effet.
run_case tablelife "test=tablelife" 33 \
    "serial:[TEST] suite K0.7 P4f-B3 page-table creation lifecycle (isolated) done:" \
    "serial:[TEST] D4 OOM: vmm_map=-2" \
    "serial:[TEST] ALL TESTS PASSED" \
    "![FAIL]" "!KERNEL PANIC"
# Violations d'invariant APRÈS l'allocation : arrêt déterministe, diagnostic
# explicite, jamais de retour au VMM, ni triple fault ni panique récursive.
for tc in "tcseal:sealing refused (error -9)" "tcpde:PDE publication refused (error -10)" \
          "tablealias:frame already mapped elsewhere (error -10)"; do
    run_case "${tc%%:*}" "test=${tc%%:*}" 37 \
        "serial:KERNEL PANIC: page-table creation: ${tc#*:}" \
        "serial:kernel invariant violated, frame not returned" \
        "!internal violation NOT fatal" \
        "!preconditions not met" \
        "!RECURSIVE PANIC" \
        "!DOUBLE FAULT"
done
# tablealias : le cadre du diagnostic est exactement le cadre aliasé.
total=$((total + 1))
want=$(tr -d '\r' < "$OUT/tablealias.serial.log" 2>/dev/null | sed -n 's/.*tablealias: frame \(0x[0-9a-f]*\) mapped.*/\1/p' | head -1)
got=$(tr -d '\r' < "$OUT/tablealias.serial.log" 2>/dev/null | sed -n 's/.*frame already mapped elsewhere.*: frame \(0x[0-9a-f]*\) type.*/\1/p' | head -1)
if [ -n "$want" ] && [ "$got" = "$want" ]; then
    echo "[PASS] tablealias_frame (cadre $got)"; passed=$((passed + 1))
else
    echo "[FAIL] tablealias_frame (diagnostic ${got:-absent}, attendu ${want:-absent})"; fail=1
fi

# K0.7-R1d — Sentinelles de S2 : un mot sentinelle (le mot 2) est altéré, puis
# une vraie #DF est provoquée. La tâche #DF doit le signaler (masque 0x4) et
# paniquer avec un message identifiable ; aucune panique récursive.
run_case dfsentinel "test=dfsentinel" 37 \
    "serial:[TEST] corrupting double-fault stack sentinel word 2" \
    "serial:*** DOUBLE FAULT (#DF) caught by task gate, running on dedicated stack" \
    "serial:double-fault stack sentinels: CORRUPTED (mask 0x4)" \
    "serial:KERNEL PANIC: double fault; double-fault stack sentinels corrupted (mask 0x4)" \
    "!RECURSIVE PANIC"

# K0.7-R1b — Canari de pile : la copie locale du canari (et elle seule) est
# modifiée dans une fonction instrumentée ; son épilogue doit appeler
# __stack_chk_fail, donc panic. Le message « NOT detected » signalerait un
# retour normal (statut 35 : échec du cas).
run_case canary_smash "test=canary" 37 \
    "serial:[TEST] critical structures sealed" \
    "serial:[TEST] corrupting the local canary copy of canary_frame_probe" \
    "serial:KERNEL PANIC: stack smashing detected: canary corrupted in the function containing 0x" \
    "serial:at kernel/core/stack_protector.c:" \
    "!canary corruption NOT detected" \
    "!RECURSIVE PANIC"

# Tailles de RAM : petite, moyenne, au-delà de la limite d'identité (1 GiB),
# et 4 GiB (région au-dessus de 4 GiB dans la carte, ignorée).
QEMU_MEM=16M TIMEOUT=4 run_case mem_16M "" 124 \
    "pmm: descriptor table consistent" "!inconsistent" \
    "paging: enabled" "SAMOS: K0.6 boot complete" "!KERNEL PANIC" "!mm: RAM above" \
    "SAMOS: page directory and 4 page tables sealed read-only (0 writable, 0 aliases)"
QEMU_MEM=512M TIMEOUT=4 run_case mem_512M "" 124 \
    "pmm: descriptor table consistent" "!inconsistent" \
    "128 page tables" "SAMOS: K0.6 boot complete" "!KERNEL PANIC" \
    "SAMOS: page directory and 128 page tables sealed read-only (0 writable, 0 aliases)"

# K0.7-P4e-2 — Scellement des structures de pagination aux tailles de RAM
# extrêmes, image de PRODUCTION : 1 792 KiB (minimum démarrable, une table),
# 2 MiB, 1 GiB (256 tables, limite de l'identité). 1 728 KiB : refus ENOMEM
# inchangé (le scellement ne consomme aucun cadre).
IMG=prod QEMU_MEM=1792K TIMEOUT=4 run_case mem_1792K "" 124 \
    "serial:SAMOS: page directory and 1 page tables sealed read-only (0 writable, 0 aliases)" \
    "serial:SAMOS: K0.6 boot complete" "!KERNEL PANIC"
IMG=prod QEMU_MEM=2M TIMEOUT=4 run_case mem_2M "" 124 \
    "serial:SAMOS: page directory and 1 page tables sealed read-only (0 writable, 0 aliases)" \
    "serial:SAMOS: K0.6 boot complete" "!KERNEL PANIC"
IMG=prod QEMU_MEM=1G TIMEOUT=6 run_case mem_1G "" 124 \
    "serial:SAMOS: page directory and 256 page tables sealed read-only (0 writable, 0 aliases)" \
    "serial:SAMOS: K0.6 boot complete" "!KERNEL PANIC"
IMG=prod QEMU_MEM=1728K TIMEOUT=4 run_case mem_1728K "" 124 \
    "serial:KERNEL PANIC: memory management initialisation failed (-2)" \
    "!page tables sealed"
QEMU_MEM=1500M TIMEOUT=6 run_case mem_1500M "" 124 \
    "pmm: descriptor table consistent" "!inconsistent" \
    "pmm: 262144 frames managed" "mm: RAM above 1024 MiB is not managed in K0" \
    "SAMOS: K0.6 boot complete" "!KERNEL PANIC"
QEMU_MEM=4G TIMEOUT=6 run_case mem_4G "" 124 \
    "pmm: descriptor table consistent" "!inconsistent" \
    "mm: 0x100000000-0x13fffffff available" "pmm: 262144 frames managed" \
    "SAMOS: K0.6 boot complete" "!KERNEL PANIC"

# Régression : les autotests passent aussi avec 512 MiB.
# TIMEOUT=30 (K0.7-R1c) : à 512 MiB, la durée mesurée des autotests est de
# 10 à 11 s, AUSSI avec l'image de R1b (antérieure à l'instrumentation
# globale) ; le délai par défaut de 10 s faisait échouer ce cas de façon
# aléatoire. À 128 MiB : 4,6 s. Cause précise de la durée non mesurée
# (la durée croît avec la RAM : parcours des cadres et des tables).
QEMU_MEM=512M TIMEOUT=30 run_case selftest_512M "test" 33 "[TEST] ALL TESTS PASSED" "![FAIL]"

# K0.7 — Faute imbriquée : #UD dans un gestionnaire de #BP. La garde d'imbrication
# arrête le noyau dès le second niveau, avec un rapport explicite.
run_case nested_exception "test=nested" 37 \
    "serial:*** NESTED CPU exception 6 (Invalid Opcode (#UD))" \
    "serial:while handling exception 3 (Breakpoint (#BP))" \
    "serial:KERNEL PANIC: nested exception" \
    "!RECURSIVE PANIC"

# K0.7-M2.2 — Cohérence de la table des descripteurs vérifiée au démarrage
# (pmm_core_check dans mm_init) aussi en PRODUCTION, et à la RAM minimale testée.
IMG=prod TIMEOUT=3 run_case prod_descriptor_check "" 124 \
    "serial:pmm: descriptor table consistent" "!inconsistent" "!KERNEL PANIC"
