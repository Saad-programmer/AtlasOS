# =============================================================================
# SAMOS / AtlasOS — mk/x86_64.mk                              (K1.1-M64-1)
# -----------------------------------------------------------------------------
# Construction double : un SQUELETTE ELF64 x86-64, NON AMORÇABLE, construit à
# côté du noyau i386, qui reste la seule image exécutable.
#
# Cibles :
#   make x86_64              build/x86_64/samos64.elf et samos64-test.elf
#   make check-elf64         contrôles statiques ELF64 (tools/check_elf64.sh)
#   make test-x86_64-build   drapeaux, séparation des objets, contrôles ELF64,
#                            tests négatifs, reproductibilité ; inclus dans
#                            « make test »
#
# Séparation (aucune contamination croisée) :
#   - variables propres (préfixe X64_) : CFLAGS, ASFLAGS, LDFLAGS et ARCH de
#     l'i386 ne sont ni lus ni modifiés ;
#   - répertoire propre : build/x86_64/{prod,test}/ ; les règles à motifs de
#     l'i386 (build/prod/%.o, build/test/%.o) ne peuvent pas désigner ces
#     chemins, et réciproquement ;
#   - « make all » n'est pas modifié : il ne construit que l'i386 (banc de
#     mutations, reproductibilité, images de référence inchangés).
#   tests/tools/x86_64_build_check.sh vérifie que chaque objet de
#   build/x86_64 est ELF64 x86-64 et chaque objet i386 ELF32 i386.
#
# Ce fragment est inclus à la fin du Makefile ; il ne change aucune commande
# de l'i386 (vérifié par comparaison de « make -n -B all », rapport M64-1).
# =============================================================================

X64_TARGET      := x86_64-unknown-none-elf
X64_BUILD       := $(BUILD)/x86_64
X64_KERNEL      := $(X64_BUILD)/samos64.elf
X64_TEST_KERNEL := $(X64_BUILD)/samos64-test.elf

# --- Options du compilateur C x86-64 ----------------------------------------
# Reprises de l'i386 (mêmes raisons, voir le Makefile) : freestanding, sans
# en-têtes de l'hôte, code à adresse fixe, canari de pile, sans CET, cadre de
# pile borné, avertissements traités comme des erreurs, DWARF à chemins
# relatifs (-ffile-prefix-map).
# Propres au x86-64 :
#   --target=x86_64-unknown-none-elf -march=x86-64 : jeu d'instructions de
#                         base x86-64 (SSE2 inclus dans -march, mais interdit
#                         par -mgeneral-regs-only)
#   -mno-red-zone       : OBLIGATOIRE en noyau. L'ABI System V AMD64 autorise
#                         une fonction à utiliser 128 octets SOUS RSP sans les
#                         réserver ; une interruption, qui empile sur la pile
#                         courante, les écraserait (conception §6.1).
#   -mgeneral-regs-only : aucun registre x87, MMX, SSE ou AVX (aucune
#                         sauvegarde d'état FPU n'existe dans le noyau).
#   -mcmodel=small      : HYPOTHÈSE LOCALE DE M64-1, PAS la décision A-6.
#                         Le code et les données sont supposés tenir sous
#                         2 GiB, ce qui est vrai pour le squelette lié à 1 MiB
#                         (kernel/arch/x86_64/linker.ld). A-6 sera tranchée
#                         avec D-LAYOUT (-mcmodel=kernel pour un noyau
#                         « higher-half »).
#   -fno-pic -fno-pie   : comme l'i386 ; sans table GOT/PLT ni relogement
#                         dynamique (vérifié par check_elf64.sh).
# Absent volontairement : -mstack-alignment=16, inutile en x86-64 (l'ABI
# impose déjà 16 octets) ; -march=i686 et -m32, propres à l'i386.
X64_CFLAGS := --target=$(X64_TARGET) -march=x86-64 -std=c11 \
              -ffreestanding -nostdlibinc -fno-pic -fno-pie \
              -mno-red-zone -mgeneral-regs-only -mcmodel=small \
              -fstack-protector-strong -fcf-protection=none \
              -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -fno-common -O2 -g \
              -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
              -Wpointer-arith -Wundef -Wvla -Wimplicit-fallthrough -Wcast-qual -Wformat=2 \
              -Wframe-larger-than=3072 \
              -Ikernel/include \
              -MMD -MP \
              -ffile-prefix-map=$(CURDIR)=.

# --- NASM : objets ELF64 ------------------------------------------------------
X64_ASFLAGS := -f elf64 -g -F dwarf -w+all -w+error

# --- Éditeur de liens : émulation x86-64, mêmes garde-fous que l'i386 --------
X64_LDFLAGS := -m elf_x86_64 -T kernel/arch/x86_64/linker.ld -nostdlib \
               -z max-page-size=0x1000 -z noexecstack --build-id=none \
               --fatal-warnings

# --- Sources du squelette ------------------------------------------------------
X64_ASM_SRCS  := kernel/arch/x86_64/boot/entry.asm
X64_C_SRCS    := kernel/arch/x86_64/skeleton.c
# Image de TEST seulement (séparation production/test, règle X10).
X64_TEST_SRCS := kernel/arch/x86_64/skeleton_test.c

x64_objs = $(patsubst %.asm,$(X64_BUILD)/$(1)/%.o,$(X64_ASM_SRCS)) $(patsubst %.c,$(X64_BUILD)/$(1)/%.o,$(2))
X64_PROD_OBJS := $(call x64_objs,prod,$(X64_C_SRCS))
X64_TEST_OBJS := $(call x64_objs,test,$(X64_C_SRCS) $(X64_TEST_SRCS))

.PHONY: x86_64 check-elf64 test-x86_64-build

x86_64: $(X64_KERNEL) $(X64_TEST_KERNEL)

$(X64_KERNEL): $(X64_PROD_OBJS) kernel/arch/x86_64/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(X64_LDFLAGS) -Map=$(X64_BUILD)/samos64.map -o $@ $(X64_PROD_OBJS)

$(X64_TEST_KERNEL): $(X64_TEST_OBJS) kernel/arch/x86_64/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(X64_LDFLAGS) -Map=$(X64_BUILD)/samos64-test.map -o $@ $(X64_TEST_OBJS)

$(X64_BUILD)/prod/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(X64_CFLAGS) $(PROD_DEFS) -c $< -o $@

$(X64_BUILD)/test/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(X64_CFLAGS) $(TEST_DEFS) -c $< -o $@

# Deux règles distinctes pour l'assembleur (même raison que l'i386 : une règle
# à motifs à plusieurs cibles serait exécutée une seule fois).
$(X64_BUILD)/prod/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(X64_ASFLAGS) $< -o $@

$(X64_BUILD)/test/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(X64_ASFLAGS) $< -o $@

check-elf64: $(X64_KERNEL) $(X64_TEST_KERNEL)
	tools/check_elf64.sh $(X64_KERNEL) --production
	tools/check_elf64.sh $(X64_TEST_KERNEL) --test

test-x86_64-build: $(X64_KERNEL) $(X64_TEST_KERNEL) $(KERNEL) $(TEST_KERNEL)
	tests/tools/x86_64_build_check.sh

# Le squelette fait partie du banc complet : prérequis ajouté à « test »
# (règle sans recette : GNU Make fusionne les prérequis).
test: test-x86_64-build

-include $(X64_PROD_OBJS:.o=.d) $(X64_TEST_OBJS:.o=.d)
