#!/usr/bin/env bash
# =============================================================================
# SAMOS / AtlasOS — tests/tools/check_elf_negative.sh
# Tests NÉGATIFS de l'outil tools/check_elf.sh : un vérificateur qui ne sait
# jamais échouer ne prouve rien. On altère des copies de l'image noyau et on
# exige que l'outil les rejette.
# =============================================================================
set -u
ELF="${1:?usage: check_elf_negative.sh build/samos.elf}"
TMP="$(dirname "$ELF")/tests/neg"; mkdir -p "$TMP"
fail=0
expect_reject() {   # NOM FICHIER MOTIF_ATTENDU
    if out=$(tools/check_elf.sh "$2" 2>&1); then
        echo "[FAIL] check_elf accepte une image alteree : $1"; fail=1
    elif grep -qF "$3" <<<"$out"; then
        echo "[PASS] check_elf rejette : $1"
    else
        echo "[FAIL] check_elf rejette $1 sans le bon diagnostic ($3)"; fail=1
    fi
}
# Offset fichier de l'en-tête : premier segment PT_LOAD (0x1000, cf. linker.ld)
MB_OFF=$(readelf -lW "$ELF" | awk '$1=="LOAD" {print $2; exit}')
# 1. Somme de contrôle corrompue
cp "$ELF" "$TMP/bad_checksum.elf"
printf '\x00' | dd of="$TMP/bad_checksum.elf" bs=1 seek=$((MB_OFF + 8)) conv=notrunc status=none
expect_reject "somme Multiboot corrompue" "$TMP/bad_checksum.elf" "checksum invalide"
# 2. Magic effacé
cp "$ELF" "$TMP/no_magic.elf"
printf '\x00\x00\x00\x00' | dd of="$TMP/no_magic.elf" bs=1 seek=$((MB_OFF)) conv=notrunc status=none
expect_reject "en-tete Multiboot absent" "$TMP/no_magic.elf" "introuvable"
exit $fail
