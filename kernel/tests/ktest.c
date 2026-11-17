/*
 * SAMOS / AtlasOS — kernel/tests/ktest.c
 * Moteur des autotests noyau (voir ktest.h).
 */
#include <samos/ktest.h>
#include <samos/qemu_dbg.h>

static unsigned g_checks;
static unsigned g_failures;

static void out(const char *s)
{
    qemu_dbg_puts(s);
}

void ktest_suite(const char *name)
{
    out("[TEST] suite ");
    out(name);
    out("\n");
}

void ktest_check(bool cond, const char *expr, const char *file, int line)
{
    g_checks++;
    if (cond) {
        out("[PASS] ");
        out(expr);
        out("\n");
        return;
    }
    g_failures++;
    out("[FAIL] ");
    out(expr);
    out(" at ");
    out(file);
    out(":");
    qemu_dbg_put_dec32((uint32_t)line);
    out("\n");
}

unsigned ktest_failures(void)
{
    return g_failures;
}

void ktest_run_all(void)
{
    ktest_suite_boot();

    out("[TEST] checks=");
    qemu_dbg_put_dec32(g_checks);
    out(" failures=");
    qemu_dbg_put_dec32(g_failures);
    out("\n");
    if (g_failures == 0) {
        out("[TEST] ALL TESTS PASSED\n");
        qemu_exit(QEMU_EXIT_SUCCESS);
    }
    out("[TEST] SOME TESTS FAILED\n");
    qemu_exit(QEMU_EXIT_FAILURE);
}
