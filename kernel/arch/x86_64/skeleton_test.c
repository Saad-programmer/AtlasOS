/*
 * SAMOS / AtlasOS — kernel/arch/x86_64/skeleton_test.c     (K1.1-M64-1)
 *
 * Source présente UNIQUEMENT dans l'image de TEST du squelette x86-64
 * (X64_TEST_SRCS, mk/x86_64.mk), comme kernel/tests/ pour l'i386.
 *
 * Le symbole ktest_m64_skeleton_marker permet de vérifier la séparation
 * production/test : tools/check_elf64.sh --test exige sa présence et
 * --production refuse tout symbole « ktest_* » ou « test_* » (règle X10).
 * Il n'est pas utilisé par du code : c'est un témoin de construction.
 */
#include <stdint.h>

extern const uint32_t ktest_m64_skeleton_marker;
const uint32_t ktest_m64_skeleton_marker = 0x4B544553u;  /* « KTES » */
