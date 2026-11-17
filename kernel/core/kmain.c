/*
 * SAMOS / AtlasOS — kernel/core/kmain.c
 * ---------------------------------------------------------------------------
 * Premier code C exécuté. Appelé par _start (entry.asm) avec :
 *   magic    : valeur d'EAX laissée par le chargeur (attendu 0x2BADB002)
 *   mbi_phys : valeur d'EBX, adresse physique de struct multiboot_info
 *
 * Ordre d'initialisation (chaque étape ne dépend que des précédentes) :
 *   1. Journalisation (K0.3) : VGA + COM1 + debugcon. Ne dépend d'aucune
 *      information de démarrage : on l'initialise d'abord pour pouvoir
 *      signaler toute erreur ultérieure.
 *   2. GDT + IDT (K0.4) : toute exception devient diagnosticable.
 *   3. Validation Multiboot (K0.1) : en cas d'échec -> panic().
 *   4. Disposition mémoire (K0.2) : affichage et vérification -> panic().
 *   5. Mode test (`test` sur la ligne de commande) : autotests sous QEMU.
 *   6. Sinon : rapport final et arrêt sûr.
 */
#include <samos/bootinfo.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/layout.h>
#include <samos/log.h>
#include <samos/panic.h>
#include <samos/serial.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/interrupt.h>

uintptr_t kmain_frame_address;

void kmain(uint32_t magic, uint32_t mbi_phys)
{
    kmain_frame_address = (uintptr_t)__builtin_frame_address(0);

    unsigned sinks = log_init();
    kprintf("SAMOS: kernel entry (" SAMOS_NAME " " SAMOS_VERSION ")\n");
    klog(LOG_INFO, "console: debugcon%s%s\n",
         (sinks & LOG_SINK_SERIAL) ? " + serial COM1 115200 8N1" : " (serial absent)",
         (sinks & LOG_SINK_VGA) ? " + VGA 80x25" : "");

    /* K0.4 : GDT propre au noyau puis IDT. Avant ce point, toute exception
     * provoquait un triple fault ; après, elle produit un rapport et panic(). */
    gdt_init();
    idt_init();
    klog(LOG_INFO, "cpu: GDT loaded (flat 4 GiB, TSS main + double-fault task)\n");
    klog(LOG_INFO, "cpu: IDT loaded (%u exception gates, #DF task gate)\n", (unsigned)EXC_COUNT);

    samos_err_t err = bootinfo_init(magic, mbi_phys);
    if (err == SAMOS_EINVAL) {
        panic("invalid multiboot magic %p", (void *)(uintptr_t)magic);
    }
    if (err != SAMOS_OK) {
        panic("invalid multiboot info address %p", (void *)(uintptr_t)mbi_phys);
    }

    const struct bootinfo *bi = bootinfo_get();
    kprintf("SAMOS: multiboot magic ok, info at %p, flags %p\n",
            (void *)(uintptr_t)bi->mbi_phys, (void *)(uintptr_t)bi->flags);
    kprintf("SAMOS: cmdline \"%s\"%s\n", bi->cmdline, bi->cmdline_truncated ? " (truncated)" : "");

    kernel_layout_print();
    if (!kernel_layout_valid()) {
        panic("kernel layout invariants violated");
    }

    if (cmdline_has("test")) {
        panic_set_qemu_exit(true);
        ktest_run_all();
    }

    kprintf("SAMOS: boot stack high-water %zu bytes, guard %s\n",
            boot_stack_high_water(), boot_stack_guard_intact() ? "intact" : "CORRUPTED");
    kprintf("SAMOS: K0.4 boot complete, halting\n");
    cpu_halt_forever();
}
