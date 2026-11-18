#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/frame_limit_check.sh                    (K0.7-R1b)
# -----------------------------------------------------------------------------
# Vérifie avec les OPTIONS RÉELLES du noyau (passées en arguments par le
# Makefile) que -Wframe-larger-than refuse un cadre de pile trop grand :
#   - un cadre d'environ 5000 octets doit faire ÉCHOUER la compilation, avec
#     le diagnostic « exceeds limit » ;
#   - un cadre d'environ 2000 octets doit compiler (contre-épreuve : l'échec
#     précédent n'est pas dû à une autre erreur).
# Ce n'est qu'une borne de COMPILATION : elle ne sonde pas la pile à
# l'exécution et ne remplace pas -fstack-clash-protection (non pris en charge
# par Clang pour cette cible).
# Le puits frame_limit_sink est seulement DÉCLARÉ : s'il était défini dans le
# même fichier, Clang l'intégrerait et réduirait le tableau (cadre réel d'un
# octet, constaté) ; la borne porte sur le cadre APRÈS optimisation.
# Usage : frame_limit_check.sh CC [CFLAGS...]
# =============================================================================
set -u
[ "$#" -ge 1 ] || { echo "[FAIL] frame-limit: usage : frame_limit_check.sh CC [CFLAGS...]"; exit 2; }
command -v "$1" >/dev/null 2>&1 || { echo "[FAIL] frame-limit: compilateur absent : $1"; exit 2; }
printf '%s\n' "$@" | grep -q '^-Wframe-larger-than=' \
    || { echo "[FAIL] frame-limit: -Wframe-larger-than absent des options du noyau"; exit 1; }
T="$(mktemp -d "${TMPDIR:-/tmp}/samos-frame.XXXXXX")" || exit 2
trap 'rm -rf "$T"' EXIT
fail=0
gen() {   # gen FICHIER TAILLE
    cat > "$1" <<C
void frame_limit_sink(volatile char *p);   /* externe : le tableau doit exister */
void frame_limit_probe(void);
void frame_limit_probe(void) { volatile char b[$2]; b[0] = 1; frame_limit_sink(b); }
C
}
gen "$T/big.c" 5000
gen "$T/ok.c" 2000
if "$@" -c "$T/big.c" -o "$T/big.o" >"$T/big.log" 2>&1; then
    echo "[FAIL] frame-limit: un cadre de 5000 octets a été ACCEPTÉ"; fail=1
elif grep -q 'exceeds limit' "$T/big.log"; then
    echo "[PASS] frame-limit: cadre de 5000 octets refusé ($(grep -o 'stack frame size ([0-9]*) exceeds limit ([0-9]*)' "$T/big.log" | head -1))"
else
    echo "[FAIL] frame-limit: échec de compilation sans le diagnostic attendu"; sed 's/^/      /' "$T/big.log" | head -3; fail=1
fi
if "$@" -c "$T/ok.c" -o "$T/ok.o" >"$T/ok.log" 2>&1; then
    echo "[PASS] frame-limit: cadre de 2000 octets accepté (contre-épreuve)"
else
    echo "[FAIL] frame-limit: un cadre de 2000 octets est refusé"; sed 's/^/      /' "$T/ok.log" | head -3; fail=1
fi
exit $fail
