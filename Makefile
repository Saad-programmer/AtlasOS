# =============================================================================
# SAMOS / AtlasOS — Makefile (GNU Make)
# -----------------------------------------------------------------------------
# Cibles principales :
#   make            construit build/samos.elf
#   make run        démarre le noyau sous QEMU (sortie série sur le terminal)
#   make run-vga    idem avec la fenêtre d'écran VGA
#   make debug      démarre QEMU en attente de GDB sur localhost:1234
#   make check-elf  vérifications statiques de l'image ELF (tools/check_elf.sh)
#   make test       check-elf + tests hébergés + autotests QEMU + vérifications GDB
#   make clean      supprime build/
# =============================================================================

ARCH      := i386
BUILD     := build
# K0.7 : deux images distinctes construites à partir des mêmes sources.
#   build/samos.elf       PRODUCTION : sans autotests, sans modes de faute
#                         volontaire, sans écriture sur les ports QEMU 0xE9/0xF4
#   build/samos-test.elf  TEST : autotests + modes de faute (ligne de commande
#                         « test », « test=<mode> »), réservée aux bancs de test
KERNEL      := $(BUILD)/samos.elf
TEST_KERNEL := $(BUILD)/samos-test.elf

CC        := clang
AS        := nasm
LD        := ld
QEMU      := qemu-system-i386

TARGET    := i686-unknown-none-elf

# --- Options du compilateur C (voir Details.md, §« Chaîne de compilation ») ---
#   -ffreestanding      : pas de bibliothèque standard hébergée ; implique -fno-builtin
#   -nostdlibinc        : pas d'en-têtes système de l'hôte ; seuls les en-têtes
#                         freestanding de Clang (stdint.h, stddef.h...) restent visibles
#   -fno-pic -fno-pie   : code à adresse fixe (lié à 1 MiB)
#   -fno-stack-protector: pas de canari de pile, sauf test_canary.o (K0.7-R1b, voir plus
#                         bas) ; le support __stack_chk_* existe (entry.asm, stack_protector.c)
#   -fcf-protection=none: pas d'instructions CET (endbr32)
#   -mgeneral-regs-only : aucun registre x87/MMX/SSE dans le noyau
#   -mstack-alignment=16: maintenir la pile alignée sur 16 octets (ABI i386
#                         System V) ; sans cette option la cible i686-elf de
#                         Clang ne garantit que 4 octets (vérifié par désassemblage)
#   -fno-omit-frame-pointer : chaîne EBP conservée (traces de pile en panic)
#   -Wframe-larger-than=3072 (K0.7-R1b) : refuse tout cadre de pile > 3 KiB.
#                         Clang ignore -fstack-clash-protection pour cette cible :
#                         un cadre >= 4 KiB pourrait sauter la page de garde.
#                         Borne de COMPILATION seulement (pas de sondage de pile).
#   -ffile-prefix-map    : chemins relatifs dans DWARF et __FILE__ ; l'image ne
#                          dépend plus du répertoire de compilation (build reproductible)
CFLAGS := --target=$(TARGET) -march=i686 -std=c11 \
          -ffreestanding -nostdlibinc -fno-pic -fno-pie \
          -fno-stack-protector -fcf-protection=none \
          -mgeneral-regs-only -mstack-alignment=16 \
          -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -fno-common -O2 -g \
          -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
          -Wpointer-arith -Wundef -Wvla -Wimplicit-fallthrough -Wcast-qual -Wformat=2 \
          -Wframe-larger-than=3072 \
          -Ikernel/include -Ikernel/arch/$(ARCH)/include \
          -MMD -MP \
          -ffile-prefix-map=$(CURDIR)=.

# --- Options de l'assembleur NASM ---
#   -f elf32        : objets ELF 32 bits
#   -g -F dwarf     : informations de débogage DWARF pour GDB
#   -w+all -w+error : tous les avertissements, traités comme des erreurs
ASFLAGS := -f elf32 -g -F dwarf -w+all -w+error

# --- Options de l'éditeur de liens ---
#   -m elf_i386              : émulation i386
#   -nostdlib                : aucune bibliothèque implicite (ni libc, ni libgcc)
#   -z max-page-size=0x1000  : segments alignés sur 4 KiB dans le fichier
#   --build-id=none          : pas de section .note.gnu.build-id (avant l'en-tête)
#   --fatal-warnings         : avertissements (ex. segment RWX) = erreurs
LDFLAGS := -m elf_i386 -T kernel/arch/$(ARCH)/linker.ld -nostdlib \
           -z max-page-size=0x1000 -z noexecstack --build-id=none \
           --fatal-warnings

QEMU_FLAGS := -m 128M -no-reboot -no-shutdown

# Configuration par image (macros testées par #if dans le code).
PROD_DEFS := -DCONFIG_KTEST=0 -DCONFIG_QEMU_DEBUGCON=0
TEST_DEFS := -DCONFIG_KTEST=1 -DCONFIG_QEMU_DEBUGCON=1

# --- Sources ---
ASM_SRCS := kernel/arch/$(ARCH)/boot/entry.asm \
            kernel/arch/$(ARCH)/gdt_flush.asm \
            kernel/arch/$(ARCH)/isr_stubs.asm \
            kernel/arch/$(ARCH)/probe.asm

C_SRCS := kernel/arch/$(ARCH)/gdt.c \
          kernel/arch/$(ARCH)/idt.c \
          kernel/arch/$(ARCH)/exceptions.c \
          kernel/arch/$(ARCH)/pic.c \
          kernel/arch/$(ARCH)/irq.c \
          kernel/arch/$(ARCH)/paging.c \
          kernel/core/kmain.c \
          kernel/core/bootinfo.c \
          kernel/core/layout.c \
          kernel/core/log.c \
          kernel/core/panic.c \
          kernel/core/stack_protector.c \
          kernel/drivers/kbd_decode.c \
          kernel/drivers/pit.c \
          kernel/drivers/ps2kbd.c \
          kernel/drivers/qemu_dbg.c \
          kernel/drivers/serial.c \
          kernel/drivers/vga.c \
          kernel/lib/format.c \
          kernel/lib/math64.c \
          kernel/lib/ringbuf.c \
          kernel/lib/string.c \
          kernel/mm/memmap.c \
          kernel/mm/mm.c \
          kernel/mm/pmm.c \
          kernel/mm/pmm_core.c

# Sources présentes UNIQUEMENT dans l'image de test.
TEST_SRCS := kernel/tests/ktest.c \
             kernel/tests/test_boot.c \
             kernel/tests/test_layout.c \
             kernel/tests/test_console.c \
             kernel/tests/test_cpu.c \
             kernel/tests/test_faults.c \
             kernel/tests/test_irq.c \
             kernel/tests/test_mm.c \
             kernel/tests/test_canary.c

objs = $(patsubst %.asm,$(BUILD)/$(1)/%.o,$(ASM_SRCS)) $(patsubst %.c,$(BUILD)/$(1)/%.o,$(2))
PROD_OBJS := $(call objs,prod,$(C_SRCS))
TEST_OBJS := $(call objs,test,$(C_SRCS) $(TEST_SRCS))

.PHONY: all run run-vga run-test debug check-elf test test-tools test-host test-qemu \
        test-gdb test-mutation test-repro clean

all: $(KERNEL) $(TEST_KERNEL)

$(KERNEL): $(PROD_OBJS) kernel/arch/$(ARCH)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -Map=$(BUILD)/samos.map -o $@ $(PROD_OBJS)

$(TEST_KERNEL): $(TEST_OBJS) kernel/arch/$(ARCH)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -Map=$(BUILD)/samos-test.map -o $@ $(TEST_OBJS)

$(BUILD)/prod/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PROD_DEFS) -c $< -o $@

$(BUILD)/test/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TEST_DEFS) -c $< -o $@

# K0.7-R1b — Instrumentation minimale : un SEUL fichier, dans l'image de test.
# Placée après -fno-stack-protector (CFLAGS), elle l'emporte (dernière option).
$(BUILD)/test/kernel/tests/test_canary.o: CFLAGS += -fstack-protector-strong

# Deux règles distinctes : une règle à motifs à PLUSIEURS cibles signifie,
# pour GNU Make, qu'une seule exécution produit toutes les cibles ; après
# « make clean », les objets de l'image de test n'étaient alors jamais
# assemblés (défaut constaté en K0.7, masqué par les compilations incrémentales).
$(BUILD)/prod/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/test/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# make run      : image de PRODUCTION, sans fenêtre ; port série COM1 sur le terminal
# make run-vga  : idem avec la fenêtre de l'écran VGA (affichage graphique requis)
# make run-test : image de TEST (debugcon dans build/debugcon.log)
run: $(KERNEL)
	$(QEMU) $(QEMU_FLAGS) -display none -kernel $(KERNEL) -serial stdio

run-vga: $(KERNEL)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL) -serial stdio

run-test: $(TEST_KERNEL)
	$(QEMU) $(QEMU_FLAGS) -display none -kernel $(TEST_KERNEL) -serial stdio -debugcon file:$(BUILD)/debugcon.log

debug: $(KERNEL)
	@echo "QEMU attend GDB : gdb $(KERNEL) -ex 'target remote :1234'"
	$(QEMU) $(QEMU_FLAGS) -display none -kernel $(KERNEL) -serial stdio -s -S

check-elf: $(KERNEL) $(TEST_KERNEL)
	tools/check_elf.sh $(KERNEL) --production
	tools/check_elf.sh $(TEST_KERNEL) --test

test-tools: $(KERNEL) $(TEST_KERNEL)
	tests/tools/check_elf_negative.sh $(TEST_KERNEL)
	tests/tools/harness_selftest.sh $(KERNEL) $(TEST_KERNEL)
	tests/tools/canary_check.sh $(KERNEL) $(TEST_KERNEL)
	tests/tools/frame_limit_check.sh $(CC) $(CFLAGS) $(PROD_DEFS)

test-host:
	tests/host/run.sh

test-qemu: $(KERNEL) $(TEST_KERNEL)
	tests/qemu/run.sh $(KERNEL) $(TEST_KERNEL)

test-gdb: $(KERNEL)
	tests/qemu/gdb_checks.sh $(KERNEL)

# Vérification du banc de tests lui-même (K0.7-R0) : défauts connus (E3, E5,
# M2.5c) injectés dans une copie isolée des sources (tests/mutation/cases.sh) ;
# chaque défaut doit être DÉTECTÉ par des assertions nommées. Environ 20 s.
# Auto-test du mécanisme : tests/mutation/selftest.sh.
test-mutation:
	tests/mutation/run.sh

# Reproductibilité (K0.7-R0) : deux compilations propres dans deux répertoires
# différents ; les images sans débogage doivent être identiques octet pour
# octet, et seules les sections .debug_* peuvent différer (chemin absolu écrit
# par NASM). REPRO_STRICT=1 exige des ELF complets identiques.
# Auto-test : tests/tools/repro_selftest.sh.
test-repro:
	tests/tools/repro_check.sh

test: check-elf test-tools test-host test-qemu test-gdb

clean:
	rm -rf $(BUILD)

-include $(PROD_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
