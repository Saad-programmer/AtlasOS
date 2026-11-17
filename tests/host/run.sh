#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/host/run.sh
# Compile et exécute les tests unitaires HÉBERGÉS (sur la machine de
# développement) du code noyau indépendant du matériel, avec ASan + UBSan.
# Ces tests complètent les autotests QEMU : les sanitizers ne peuvent pas
# fonctionner dans un noyau freestanding.
# =============================================================================
set -u
cd "$(dirname "$0")/../.."
OUT=build/host
mkdir -p "$OUT"
CC="${HOST_CC:-gcc}"   # gcc : libasan/libubsan disponibles sur Ubuntu
CFLAGS="-std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
 -fno-omit-frame-pointer -fno-builtin -include tests/host/host_prefix.h -Ikernel/include"
fail=0
# test_NOM : sources noyau testées
build_and_run() {
    local name="$1"; shift
    if ! $CC $CFLAGS -o "$OUT/$name" "tests/host/$name.c" "$@"; then
        echo "[FAIL] $name (compilation)"; fail=1; return
    fi
    if ! "$OUT/$name"; then fail=1; fi
}
# shellcheck source=tests/host/cases.sh
source tests/host/cases.sh
exit $fail
