/*
 * SAMOS / AtlasOS — kernel/core/kmain.c
 * ---------------------------------------------------------------------------
 * Premier code C exécuté. Appelé par _start (entry.asm) avec :
 *   magic    : valeur d'EAX laissée par le chargeur (attendu 0x2BADB002)
 *   mbi_phys : valeur d'EBX, adresse physique de struct multiboot_info
 *
 * Séquence K0.1 :
 *   1. Annoncer l'entrée dans le noyau (canal debugcon).
 *   2. Valider le magic et la structure Multiboot (bootinfo_init).
 *      En cas d'échec : message explicite puis arrêt sûr. On ne poursuit
 *      jamais avec des informations de démarrage invalides.
 *   3. Si la ligne de commande contient `test`, exécuter les autotests.
 *   4. Sinon, s'arrêter proprement (aucun autre sous-système n'existe encore).
 */
#include <samos/bootinfo.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/qemu_dbg.h>
#include <arch/cpu.h>

uintptr_t kmain_frame_address;

static SAMOS_NORETURN void boot_fail(const char *reason, uint32_t value)
{
    qemu_dbg_puts("SAMOS: FATAL: ");
    qemu_dbg_puts(reason);
    qemu_dbg_puts(" (");
    qemu_dbg_put_hex32(value);
    qemu_dbg_puts(")\nSAMOS: system halted\n");
    cpu_halt_forever();
}

void kmain(uint32_t magic, uint32_t mbi_phys)
{
    kmain_frame_address = (uintptr_t)__builtin_frame_address(0);

    qemu_dbg_puts("SAMOS: kernel entry (" SAMOS_NAME " " SAMOS_VERSION ")\n");

    samos_err_t err = bootinfo_init(magic, mbi_phys);
    if (err == SAMOS_EINVAL) {
        boot_fail("invalid multiboot magic", magic);
    }
    if (err != SAMOS_OK) {
        boot_fail("invalid multiboot info address", mbi_phys);
    }

    const struct bootinfo *bi = bootinfo_get();
    qemu_dbg_puts("SAMOS: multiboot magic ok, info at ");
    qemu_dbg_put_hex32(bi->mbi_phys);
    qemu_dbg_puts(", flags ");
    qemu_dbg_put_hex32(bi->flags);
    qemu_dbg_puts("\nSAMOS: cmdline \"");
    qemu_dbg_puts(bi->cmdline);
    qemu_dbg_puts("\"\n");

    if (cmdline_has("test")) {
        ktest_run_all();
    }

    qemu_dbg_puts("SAMOS: K0.1 boot complete, halting\n");
    cpu_halt_forever();
}
