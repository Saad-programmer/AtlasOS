/*
 * SAMOS / AtlasOS — kernel/core/panic.c
 * Panique du noyau et trace de pile. Voir panic.h et Details.md §18.4.
 */
#include <stdarg.h>
#include <samos/config.h>
#include <samos/kernel.h>
#include <samos/log.h>
#include <samos/panic.h>
#include <samos/qemu_dbg.h>
#include <samos/serial.h>
#include <samos/vga.h>
#include <arch/cpu.h>
#include <arch/gdt.h>

#define PANIC_MAX_FRAMES 16

static volatile int g_in_panic;

#if CONFIG_KTEST
/* Image de test : une panique termine QEMU (statut 37) pour que le banc de
 * test obtienne un verdict immédiat, même hors du mode « test » (cas de
 * démarrage avec peu de RAM, par exemple). */
static bool g_qemu_exit = true;

void panic_set_qemu_exit(bool enabled)
{
    g_qemu_exit = enabled;
}

static void test_exit_if_enabled(void)
{
    if (g_qemu_exit) {
        qemu_exit(QEMU_EXIT_PANIC);
    }
}
#else
static void test_exit_if_enabled(void)
{
    /* Production : jamais de sortie vers QEMU. */
}
#endif

/* Sortie d'urgence, sans formatage ni pilote VGA : debugcon (si présent) et
 * octets bruts sur le port série. Utilisée en cas de panique récursive. */
SAMOS_NO_STACK_PROTECTOR   /* K0.7-R1 : chemin de panique, jamais instrumenté */
static void emergency_puts(const char *s)
{
    qemu_dbg_puts(s);
    for (; *s != '\0'; s++) {
        serial_putc(*s);
    }
}

/*
 * Disposition d'un cadre créé par le prologue `push ebp ; mov ebp, esp` :
 *     [EBP + 4] = adresse de retour dans l'appelant
 *     [EBP + 0] = EBP de l'appelant (cadre précédent)
 * _start met EBP à 0 avant d'appeler kmain : la chaîne se termine par 0.
 */
SAMOS_NO_STACK_PROTECTOR
size_t stack_collect(uintptr_t ebp, uintptr_t *out, size_t max)
{
    uintptr_t lo = (uintptr_t)boot_stack_bottom;
    uintptr_t hi = (uintptr_t)boot_stack_top;
    size_t n = 0;

    while (n < max && ebp != 0) {
        if (ebp < lo || ebp > hi - 8 || (ebp & 3u) != 0) {
            break;                               /* cadre hors pile : on s'arrête */
        }
        const uintptr_t *frame = (const uintptr_t *)ebp;
        out[n++] = frame[1];
        uintptr_t next = frame[0];
        if (next != 0 && next <= ebp) {
            break;                               /* la chaîne doit monter vers le sommet */
        }
        ebp = next;
    }
    return n;
}

SAMOS_NO_STACK_PROTECTOR   /* contient frames[] : serait instrumentée par -strong */
void panic_at(const char *file, int line, const char *fmt, ...)
{
    cpu_cli();

    if (g_in_panic) {
        /* Panique pendant la panique : le journal lui-même est peut-être en
         * cause. Sortie minimale sur debugcon uniquement, puis arrêt. */
        emergency_puts("\nSAMOS: RECURSIVE PANIC\n");
        test_exit_if_enabled();
        cpu_halt_forever();
    }
    g_in_panic = 1;

    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n*** KERNEL PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    (void)kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n*** at %s:%d\n", file, line);

    /* K0.7-R1d — État des sentinelles de S2, signalé seulement s'il est
     * anormal (aucune nouvelle panique : pas de récursion). La vérification
     * n'appelle rien et n'est pas instrumentée. */
    uint32_t df_damage = df_stack_sentinel_damage();
    if (df_damage != 0) {
        kprintf("*** double-fault stack sentinels CORRUPTED (mask 0x%x, words at %p)\n",
                df_damage, (void *)df_stack_bottom());
    }

    uintptr_t frames[PANIC_MAX_FRAMES];
    size_t n = stack_collect((uintptr_t)__builtin_frame_address(0), frames, PANIC_MAX_FRAMES);
    kprintf("*** stack trace (%zu frames, addr2line -e build/samos.elf <addr>):\n", n);
    for (size_t i = 0; i < n; i++) {
        kprintf("***   #%zu %p\n", i, (void *)frames[i]);
    }
    kprintf("*** system halted\n");

    test_exit_if_enabled();
    cpu_halt_forever();
}
