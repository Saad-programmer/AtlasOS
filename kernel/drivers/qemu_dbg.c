/*
 * SAMOS / AtlasOS — kernel/drivers/qemu_dbg.c
 * Canal debugcon (port 0xE9) et sortie isa-debug-exit. Voir qemu_dbg.h.
 */
#include <samos/qemu_dbg.h>
#include <arch/io.h>
#include <arch/cpu.h>

#ifndef CONFIG_QEMU_DEBUGCON
#define CONFIG_QEMU_DEBUGCON 1
#endif

void qemu_dbg_putc(char c)
{
#if CONFIG_QEMU_DEBUGCON
    outb(QEMU_DEBUGCON_PORT, (uint8_t)c);
#else
    (void)c;
#endif
}

void qemu_dbg_puts(const char *s)
{
    while (*s != '\0') {
        qemu_dbg_putc(*s++);
    }
}

void qemu_dbg_put_hex32(uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    qemu_dbg_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        qemu_dbg_putc(digits[(v >> shift) & 0xFu]);
    }
}

void qemu_dbg_put_dec32(uint32_t v)
{
    char buf[10];       /* 2^32 - 1 = 4294967295 : 10 chiffres au maximum */
    int i = 0;
    do {
        buf[i++] = (char)('0' + (v % 10u));   /* division 32 bits : instruction DIV native */
        v /= 10u;
    } while (v != 0 && i < (int)sizeof(buf));
    while (i > 0) {
        qemu_dbg_putc(buf[--i]);
    }
}

void qemu_exit(uint32_t code)
{
    outl(QEMU_DEBUG_EXIT_PORT, code);
    /* Si le périphérique est absent (exécution normale, matériel réel),
     * l'écriture n'a aucun effet : on s'arrête proprement. */
    cpu_halt_forever();
}
