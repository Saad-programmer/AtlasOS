/*
 * SAMOS / AtlasOS — kernel/core/log.c
 * Journalisation multi-puits. Voir log.h.
 */
#include <samos/config.h>
#include <samos/format.h>
#include <samos/log.h>
#include <samos/qemu_dbg.h>
#include <samos/serial.h>
#include <samos/string.h>
#include <samos/vga.h>

#if CONFIG_QEMU_DEBUGCON
static unsigned g_sinks = LOG_SINK_DEBUGCON;   /* debugcon utilisable avant toute init */
#else
static unsigned g_sinks;                       /* production : pas de port 0xE9 */
#endif
static unsigned g_truncations;

unsigned log_init(void)
{
    vga_init();
    g_sinks |= LOG_SINK_VGA;
    if (serial_init()) {
        g_sinks |= LOG_SINK_SERIAL;
    }
    return g_sinks;
}

unsigned log_sinks(void)
{
    return g_sinks;
}

void console_write(const char *s, size_t n)
{
    if (g_sinks & LOG_SINK_DEBUGCON) {
        for (size_t i = 0; i < n; i++) {
            qemu_dbg_putc(s[i]);
        }
    }
    if (g_sinks & LOG_SINK_SERIAL) {
        serial_write(s, n);
    }
    if (g_sinks & LOG_SINK_VGA) {
        vga_write(s, n);
    }
}

int kvprintf(const char *fmt, va_list ap)
{
    char line[LOG_LINE_MAX];
    int n = kvsnprintf(line, sizeof(line), fmt, ap);
    if (n < 0) {
        return n;
    }
    size_t len = strlen(line);
    console_write(line, len);
    if ((size_t)n > len) {
        /* Troncature : le signaler explicitement plutôt que perdre la fin en silence. */
        static const char mark[] = " [...truncated]\n";
        console_write(mark, sizeof(mark) - 1);
        g_truncations++;
    }
    return n;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

void klog(enum log_level level, const char *fmt, ...)
{
    static const char *const prefix[] = { "[debug] ", "[info ] ", "[warn ] ", "[ERROR] " };
    unsigned idx = ((unsigned)level <= (unsigned)LOG_ERROR) ? (unsigned)level : (unsigned)LOG_ERROR;
    console_write(prefix[idx], 8);
    va_list ap;
    va_start(ap, fmt);
    (void)kvprintf(fmt, ap);
    va_end(ap);
}

unsigned log_truncations(void)
{
    return g_truncations;
}
