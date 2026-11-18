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
 *   5. Mémoire (K0.6) : carte mémoire, cadres physiques, pagination.
 *   6. Interruptions matérielles (K0.5) : PIC remappé et tout masqué, puis
 *      PIT (IRQ 0) et clavier (IRQ 1). IF reste à 0 jusqu'à l'étape 8.
 *   7. Mode test (`test` sur la ligne de commande) : autotests sous QEMU.
 *   8. Sinon : STI puis boucle d'inactivité (écho clavier).
 */
#include <samos/bootinfo.h>
#include <samos/config.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/layout.h>
#include <samos/mm.h>
#include <samos/log.h>
#include <samos/panic.h>
#include <samos/serial.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/paging.h>
#include <arch/interrupt.h>
#include <arch/pic.h>
#include <samos/keyboard.h>
#include <samos/pit.h>

uintptr_t kmain_frame_address;

/*
 * Boucle d'inactivité (K0.5) : traite les touches en dehors du contexte
 * d'interruption, puis dort jusqu'à la prochaine interruption.
 * Le test « rien à faire ? » est fait avec IF = 0, puis cpu_idle() exécute
 * « sti ; hlt » : une IRQ survenue entre le test et HLT ne peut pas être
 * manquée (Details.md §28.5).
 */
static SAMOS_NORETURN void idle_loop(void)
{
    struct key_event ev;
    for (;;) {
        cpu_cli();
        if (!keyboard_get_event(&ev)) {
            /* File vide, vérifiée avec IF = 0 : dormir immédiatement.
             * NE PAS réactiver IF avant (sti ... cli ... hlt) : une IRQ
             * arrivée entre les deux déposerait une touche, puis le CPU
             * dormirait avec cette touche en attente (réveil perdu). */
            cpu_idle();
            continue;
        }
        cpu_sti();
        if (ev.pressed && ev.ascii != 0) {
            kprintf("%c", ev.ascii);
        }
    }
}

void kmain(uint32_t magic, uint32_t mbi_phys)
{
    kmain_frame_address = (uintptr_t)__builtin_frame_address(0);

    unsigned sinks = log_init();
    kprintf("SAMOS: kernel entry (" SAMOS_NAME " " SAMOS_VERSION ")\n");
    klog(LOG_INFO, "console:%s%s%s (%s image)\n",
         (sinks & LOG_SINK_DEBUGCON) ? " debugcon" : "",
         (sinks & LOG_SINK_SERIAL) ? " serial COM1 115200 8N1" : " (serial absent)",
         (sinks & LOG_SINK_VGA) ? " VGA 80x25" : "",
         CONFIG_KTEST ? "test" : "production");

    /* K0.4 : GDT propre au noyau puis IDT. Avant ce point, toute exception
     * provoquait un triple fault ; après, elle produit un rapport et panic(). */
#if CONFIG_KTEST
    /* K0.7-R1h (F1) — Image de TEST uniquement : remplit S2 d'un motif non nul
     * AVANT son initialisation légitime, pour que les autotests puissent
     * prouver que gdt_init la remet à zéro (le chargeur la remplit déjà de
     * zéros : sans ce motif, la preuve était vide, audit R1g). S2 ne peut pas
     * encore servir : ni TSS #DF, ni descripteur, ni porte de tâche. */
    ktest_s2_prefill();
#endif
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

    /* K0.6 : carte mémoire, allocateur physique, pagination. Avant les IRQ :
     * timer et clavier fonctionneront d'emblée sous pagination. */
    samos_err_t mm_err = mm_init();
    if (mm_err != SAMOS_OK) {
        panic("memory management initialisation failed (%d)", mm_err);
    }

    pic_init();
    klog(LOG_INFO, "irq: 8259 PIC remapped to vectors 0x%02x-0x%02x, all lines masked\n",
         PIC_IRQ_BASE, PIC_IRQ_BASE + PIC_IRQ_COUNT - 1);
    if (!pit_init(PIT_DEFAULT_HZ)) {
        panic("PIT initialisation failed");
    }
    klog(LOG_INFO, "timer: PIT channel 0, %u Hz (divisor %u)\n", pit_hz(), pit_divisor());
    if (keyboard_init()) {
        klog(LOG_INFO, "keyboard: 8042 self-test ok, PS/2 keyboard reset ok, IRQ1 enabled\n");
    } else {
        /* Pas fatal : le noyau reste utilisable sans clavier (port série). */
        klog(LOG_WARN, "keyboard: unavailable (%s)\n", keyboard_error());
    }

#if CONFIG_KTEST
    if (cmdline_has("test")) {
        panic_set_qemu_exit(true);
        ktest_run_all();
    }
#else
    if (cmdline_has("test")) {
        /* Production : les autotests et modes de faute ne sont pas compilés. */
        klog(LOG_WARN, "test mode requested but not available in the production image\n");
    }
#endif

    /* K0.7-P4d — Scellement des structures critiques immuables, APRÈS leurs
     * dernières écritures légitimes (idt_init, interrupt_register du #PF dans
     * paging_init, irq_register du PIT et du clavier ci-dessus). Ne passe pas
     * par une fenêtre CR0.WP : la protection reste active en permanence. */
    samos_err_t seal_err = paging_seal_critical();
    if (seal_err != SAMOS_OK) {
        panic("critical structure sealing failed (%d)", seal_err);
    }
    kprintf("SAMOS: critical structures sealed read-only (IDT, handler tables, VMM roots)\n");
    /* K0.7-P4e-2 — Les structures de pagination sont scellées par
     * paging_init (juste après PG | WP). Audit structurel indépendant : tout
     * écart (table inscriptible, alias, PDE non conforme, table non publiée)
     * est fatal. */
    struct paging_audit pa;
    if (!paging_audit(&pa)) {
        panic("page-table audit failed: %u writable, %u aliases, %u bad PDEs, %u unpublished, "
              "%u foreign aliases, %u writable structures, %u duplicate PDEs",
              pa.writable, pa.aliases, pa.bad_pdes, pa.unpublished, pa.foreign_aliases, pa.struct_writable,
              pa.dup_pdes);
    }
    kprintf("SAMOS: page directory and %u page tables sealed read-only (0 writable, 0 aliases)\n",
            pa.tables);
    kprintf("SAMOS: boot stack high-water %zu bytes, guard page %s\n",
            boot_stack_high_water(), boot_stack_guard_intact() ? "unmapped (MMU-enforced)" : "MAPPED (unsafe)");
    /* K0.7-R1d — Point de vérification de fonctionnement normal. */
    uint32_t df_damage = df_stack_sentinel_damage();
    if (df_damage != 0) {
        panic("double-fault stack sentinels corrupted (mask 0x%x)", df_damage);
    }
    kprintf("SAMOS: double-fault stack sentinels intact (%u words at %p)\n",
            DF_SENTINEL_WORDS, (void *)df_stack_bottom());
    kprintf("SAMOS: K0.6 boot complete, paging on, interrupts enabled, keyboard echo active\n");
    idle_loop();
}
