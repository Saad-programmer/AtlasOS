# =============================================================================
# SAMOS / AtlasOS — tests/qemu/emu.sh                         (K1.1-M64-0)
# -----------------------------------------------------------------------------
# Bibliothèque (chargée par « source ») : identification de l'émulateur QEMU
# utilisé par les bancs. Un seul endroit décide si un émulateur est accepté,
# pour que run.sh et gdb_checks.sh ne puissent pas diverger.
#
# emu_probe QEMU
#   Démarre l'émulateur SANS noyau, arrêté (-S), et l'interroge par QMP
#   (protocole JSON de QEMU) :
#     qom-get /machine/unattached/device[0] type      -> modèle de CPU réel
#     qom-get /machine/unattached/device[0] model-id  -> chaîne CPUID affichée
#     qom-get /machine type                           -> type de machine
#   On mesure donc la configuration EFFECTIVE par défaut, au lieu de la
#   supposer (qemu-system-i386 => qemu32, qemu-system-x86_64 => qemu64 sur
#   QEMU 8.2.2 ; les deux modèles ne sont PAS équivalents : qemu64 annonce
#   le mode long, et la machine x86_64 publie une région E820 réservée de
#   plus au-dessus de 1 TiB, voir docs/review/K1.1-M64-0-report.md).
#   Positionne : EMU_VERSION, EMU_TARGET (i386 | x86_64), EMU_CPU, EMU_MODEL,
#   EMU_MACHINE, EMU_GDB_ARCH. Retour 0 si accepté, 2 sinon (diagnostic
#   « [FAIL] » sur la sortie standard) : un émulateur absent, invalide ou
#   d'une autre architecture n'est JAMAIS utilisé en silence.
#
# EMU_GDB_ARCH : architecture à imposer à GDB AVANT « target remote ».
#   i386   -> vide : GDB déduit i386 de l'ELF32, description de registres
#             32 bits fournie par le stub de qemu-system-i386.
#   x86_64 -> « i386:x86-64 » : le stub de qemu-system-x86_64 décrit TOUJOURS
#             les registres 64 bits (paquet 'g' de 608 octets), même quand le
#             CPU est en mode protégé 32 bits ; GDB en architecture i386 refuse
#             cette description (« Remote 'g' packet reply is too long »).
#             Mesuré en K1.1-M64-0 : $esp, $eflags, $pc, la mémoire et
#             « break *adresse » donnent alors les mêmes valeurs que sous
#             qemu-system-i386 ; $eip n'existe pas, le désassemblage et
#             l'affichage symbolique des arguments C sont FAUX (code 32 bits
#             décodé en 64 bits). Les bancs n'utilisent donc que des
#             registres, des adresses et la mémoire brute.
# =============================================================================

emu_probe() {
    local q="$1" out
    EMU_VERSION="" EMU_TARGET="" EMU_CPU="" EMU_MODEL="" EMU_MACHINE="" EMU_GDB_ARCH=""
    if ! command -v "$q" >/dev/null 2>&1; then
        echo "[FAIL] QEMU introuvable ($q)"
        return 2
    fi
    EMU_VERSION="$("$q" --version 2>/dev/null | head -1)"
    out="$(printf '%s\n' '{"execute":"qmp_capabilities"}' \
        '{"execute":"qom-get","arguments":{"path":"/machine/unattached/device[0]","property":"type"}}' \
        '{"execute":"qom-get","arguments":{"path":"/machine/unattached/device[0]","property":"model-id"}}' \
        '{"execute":"qom-get","arguments":{"path":"/machine","property":"type"}}' \
        '{"execute":"quit"}' |
        timeout 10 "$q" -m 128M -display none -qmp stdio -S 2>/dev/null |
        sed -n 's/^{"return": "\([^"]*\)"}.*$/\1/p')"
    local type model machine
    type="$(printf '%s\n' "$out" | sed -n 1p)"
    model="$(printf '%s\n' "$out" | sed -n 2p)"
    machine="$(printf '%s\n' "$out" | sed -n 3p)"
    case "$type" in
        *-i386-cpu)   EMU_TARGET=i386;   EMU_GDB_ARCH="" ;;
        *-x86_64-cpu) EMU_TARGET=x86_64; EMU_GDB_ARCH="i386:x86-64" ;;
        "")
            echo "[FAIL] emulateur invalide ($q) : aucune reponse QMP exploitable"
            return 2 ;;
        *)
            echo "[FAIL] emulateur non pris en charge ($q) : CPU $type (i386 ou x86_64 requis)"
            return 2 ;;
    esac
    EMU_CPU="${type%-"$EMU_TARGET"-cpu}"
    EMU_MODEL="$model"
    EMU_MACHINE="${machine%-machine}"
    return 0
}

# Ligne d'identification reprise dans chaque rapport de banc.
emu_print() {   # emu_print QEMU
    echo "[EMU] qemu=$1 target=$EMU_TARGET cpu=$EMU_CPU (\"$EMU_MODEL\") machine=$EMU_MACHINE"
    echo "[EMU] version: $EMU_VERSION"
}
