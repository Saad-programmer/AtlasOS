/*
 * SAMOS / AtlasOS — kernel/tests/test_console.c
 * Autotests du jalon K0.3 : VGA, série, formatage (compilé pour i386),
 * division 64 bits, journalisation, trace de pile.
 *
 * Les tests VGA relisent la mémoire vidéo (0xB8000) après écriture. Comme le
 * journal écrit lui aussi sur l'écran, chaque test capture les cellules dans
 * des variables locales AVANT d'appeler KTEST_CHECK (qui journalise).
 */
#include <stdint.h>
#include <samos/format.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/math64.h>
#include <samos/panic.h>
#include <samos/serial.h>
#include <samos/string.h>
#include <samos/vga.h>

#define CELL_CHAR(c) ((char)((c) & 0xFFu))
#define CELL_ATTR(c) ((uint8_t)((c) >> 8))

static void test_vga(void)
{
    unsigned r0, c0;
    vga_get_pos(&r0, &c0);

    /* Écriture simple + curseur matériel */
    vga_set_pos(5, 10);
    vga_write("VGA", 3);
    uint16_t a = vga_cell(5, 10), b = vga_cell(5, 11), c = vga_cell(5, 12);
    unsigned row, col;
    vga_get_pos(&row, &col);
    uint16_t hw = vga_hw_cursor();
    KTEST_CHECK(CELL_CHAR(a) == 'V' && CELL_CHAR(b) == 'G' && CELL_CHAR(c) == 'A');
    KTEST_CHECK(row == 5 && col == 13);
    KTEST_CHECK(hw == 5 * VGA_COLS + 13);          /* le CRTC suit le curseur logiciel */

    /* Retour à la ligne automatique en fin de ligne */
    vga_set_pos(3, 78);
    vga_write("xyz", 3);
    uint16_t w1 = vga_cell(3, 79), w2 = vga_cell(4, 0);
    vga_get_pos(&row, &col);
    KTEST_CHECK(CELL_CHAR(w1) == 'y' && CELL_CHAR(w2) == 'z');
    KTEST_CHECK(row == 4 && col == 1);

    /* Tabulation, retour arrière */
    vga_set_pos(6, 0);
    vga_write("\tT", 2);
    uint16_t t = vga_cell(6, 8);
    vga_set_pos(7, 0);
    vga_write("ab\bc", 4);
    uint16_t bs0 = vga_cell(7, 0), bs1 = vga_cell(7, 1);
    KTEST_CHECK(CELL_CHAR(t) == 'T');
    KTEST_CHECK(CELL_CHAR(bs0) == 'a' && CELL_CHAR(bs1) == 'c');

    /* Attribut de couleur : jaune (14) sur bleu (1) => 0x1E */
    vga_set_color(VGA_YELLOW, VGA_BLUE);
    vga_set_pos(8, 0);
    vga_write("C", 1);
    uint16_t col_cell = vga_cell(8, 0);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    KTEST_CHECK(CELL_ATTR(col_cell) == 0x1E);

    /* Défilement : un '\n' sur la dernière ligne remonte l'écran d'une ligne */
    vga_set_pos(VGA_ROWS - 1, 0);
    vga_write("SCROLLMARK\n", 11);
    uint16_t s0 = vga_cell(VGA_ROWS - 2, 0), s9 = vga_cell(VGA_ROWS - 2, 9);
    uint16_t last = vga_cell(VGA_ROWS - 1, 0);
    vga_get_pos(&row, &col);
    KTEST_CHECK(CELL_CHAR(s0) == 'S' && CELL_CHAR(s9) == 'K');
    KTEST_CHECK(CELL_CHAR(last) == ' ');
    KTEST_CHECK(row == VGA_ROWS - 1 && col == 0);

    /* Bornes */
    vga_set_pos(1000, 1000);
    vga_get_pos(&row, &col);
    KTEST_CHECK(row == VGA_ROWS - 1 && col == VGA_COLS - 1);
    KTEST_CHECK(vga_cell(VGA_ROWS, 0) == 0 && vga_cell(0, VGA_COLS) == 0);

    vga_set_pos(r0, c0);
}

static void test_serial(void)
{
    KTEST_CHECK(serial_available());               /* QEMU fournit COM1 */
    KTEST_CHECK((log_sinks() & LOG_SINK_SERIAL) != 0);
    KTEST_CHECK(serial_dropped() == 0);
}

static void test_format_i386(void)
{
    /* Les mêmes routines que les tests hébergés, mais compilées pour i386 :
     * vérifie notamment l'absence de libgcc pour les entiers 64 bits. */
    char buf[64];
    ksnprintf(buf, sizeof buf, "%llu", (unsigned long long)UINT64_MAX);
    KTEST_CHECK(strcmp(buf, "18446744073709551615") == 0);
    ksnprintf(buf, sizeof buf, "%lld", (long long)INT64_MIN);
    KTEST_CHECK(strcmp(buf, "-9223372036854775808") == 0);
    ksnprintf(buf, sizeof buf, "%08x|%-4d|%s|%p", 0xbeefu, -7, "ok", (void *)0x1000);
    KTEST_CHECK(strcmp(buf, "0000beef|-7  |ok|0x00001000") == 0);
    int n = ksnprintf(buf, 4, "%s", "abcdef");
    KTEST_CHECK(n == 6 && strcmp(buf, "abc") == 0);
    uint32_t r;
    KTEST_CHECK(u64_divmod_u32(1000000000000ull, 7, &r) == 142857142857ull && r == 1);
}

static void test_log_truncation(void)
{
    unsigned before = log_truncations();
    char longline[LOG_LINE_MAX + 32];
    memset(longline, 'L', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    kprintf("%s\n", longline);
    KTEST_CHECK(log_truncations() == before + 1);   /* troncature détectée et signalée */
}

static __attribute__((noinline)) size_t level3(uintptr_t *out, size_t max)
{
    return stack_collect((uintptr_t)__builtin_frame_address(0), out, max);
}
static __attribute__((noinline)) size_t level2(uintptr_t *out, size_t max) { return level3(out, max) + 0; }
static __attribute__((noinline)) size_t level1(uintptr_t *out, size_t max) { return level2(out, max) + 0; }

static void test_stack_trace(void)
{
    uintptr_t frames[16];
    size_t n = level1(frames, 16);
    KTEST_CHECK(n >= 4);   /* level2, level1, test_stack_trace, ... jusqu'à kmain */
    bool in_text = true;
    for (size_t i = 0; i < n; i++) {
        if (frames[i] < (uintptr_t)__text_start || frames[i] >= (uintptr_t)__text_end) {
            in_text = false;
        }
    }
    KTEST_CHECK(in_text);  /* toutes les adresses de retour pointent dans .text */
    /* Cadre invalide : hors pile => aucun cadre, sans faute */
    KTEST_CHECK(stack_collect(0x1000, frames, 16) == 0);
    KTEST_CHECK(stack_collect(0, frames, 16) == 0);
    KTEST_CHECK(stack_collect((uintptr_t)boot_stack_bottom + 2, frames, 16) == 0);  /* non aligné */
}

void ktest_suite_console(void)
{
    ktest_suite("K0.3 console");
    test_vga();
    test_serial();
    test_format_i386();
    test_log_truncation();
    test_stack_trace();
}
