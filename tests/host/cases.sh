# shellcheck shell=bash
# Tests hébergés : build_and_run NOM_DU_TEST SOURCES_NOYAU...
build_and_run test_string kernel/lib/string.c
build_and_run test_format kernel/lib/format.c kernel/lib/math64.c kernel/lib/string.c
