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
KERNEL    := $(BUILD)/samos.elf

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
#   -fno-stack-protector: pas de canari de pile (nécessiterait un runtime __stack_chk_*)
#   -fcf-protection=none: pas d'instructions CET (endbr32)
#   -mgeneral-regs-only : aucun registre x87/MMX/SSE dans le noyau
#   -mstack-alignment=16: maintenir la pile alignée sur 16 octets (ABI i386
#                         System V) ; sans cette option la cible i686-elf de
#                         Clang ne garantit que 4 octets (vérifié par désassemblage)
#   -fno-omit-frame-pointer : chaîne EBP conservée (traces de pile en panic)
CFLAGS := --target=$(TARGET) -march=i686 -std=c11 \
          -ffreestanding -nostdlibinc -fno-pic -fno-pie \
          -fno-stack-protector -fcf-protection=none \
          -mgeneral-regs-only -mstack-alignment=16 \
          -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -fno-common -O2 -g \
          -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
          -Wpointer-arith -Wundef -Wvla -Wimplicit-fallthrough -Wcast-qual -Wformat=2 \
          -Ikernel/include -Ikernel/arch/$(ARCH)/include \
          -MMD -MP

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
           --fatal-warnings -Map=$(BUILD)/samos.map

QEMU_FLAGS := -m 128M -no-reboot -no-shutdown

# --- Sources ---
ASM_SRCS := kernel/arch/$(ARCH)/boot/entry.asm \
            kernel/arch/$(ARCH)/gdt_flush.asm \
            kernel/arch/$(ARCH)/isr_stubs.asm

C_SRCS := kernel/arch/$(ARCH)/gdt.c \
          kernel/arch/$(ARCH)/idt.c \
          kernel/arch/$(ARCH)/exceptions.c \
          kernel/core/kmain.c \
          kernel/core/bootinfo.c \
          kernel/core/layout.c \
          kernel/core/log.c \
          kernel/core/panic.c \
          kernel/drivers/qemu_dbg.c \
          kernel/drivers/serial.c \
          kernel/drivers/vga.c \
          kernel/lib/format.c \
          kernel/lib/math64.c \
          kernel/lib/string.c \
          kernel/tests/ktest.c \
          kernel/tests/test_boot.c \
          kernel/tests/test_layout.c \
          kernel/tests/test_console.c \
          kernel/tests/test_cpu.c \
          kernel/tests/test_faults.c

OBJS := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS)) \
        $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))

.PHONY: all run run-vga debug check-elf test test-tools test-host test-qemu test-gdb clean

all: $(KERNEL)

$(KERNEL): $(OBJS) kernel/arch/$(ARCH)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# make run     : sans fenêtre ; port série COM1 sur le terminal, debugcon dans build/debugcon.log
# make run-vga : fenêtre QEMU affichant l'écran VGA (nécessite un affichage graphique)
run: $(KERNEL)
	$(QEMU) $(QEMU_FLAGS) -display none -kernel $(KERNEL) -serial stdio -debugcon file:$(BUILD)/debugcon.log

run-vga: $(KERNEL)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL) -serial stdio -debugcon file:$(BUILD)/debugcon.log

debug: $(KERNEL)
	@echo "QEMU attend GDB : gdb $(KERNEL) -ex 'target remote :1234'"
	$(QEMU) $(QEMU_FLAGS) -display none -kernel $(KERNEL) -serial stdio -debugcon file:$(BUILD)/debugcon.log -s -S

check-elf: $(KERNEL)
	tools/check_elf.sh $(KERNEL)

test-tools: $(KERNEL)
	tests/tools/check_elf_negative.sh $(KERNEL)

test-host:
	tests/host/run.sh

test-qemu: $(KERNEL)
	tests/qemu/run.sh $(KERNEL)

test-gdb: $(KERNEL)
	tests/qemu/gdb_checks.sh $(KERNEL)

test: check-elf test-tools test-host test-qemu test-gdb

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
