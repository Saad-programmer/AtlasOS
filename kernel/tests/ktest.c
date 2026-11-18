/*
 * SAMOS / AtlasOS — kernel/tests/ktest.c
 * Moteur des autotests noyau (voir ktest.h).
 *
 * Modes (ligne de commande) :
 *   test          toutes les suites, verdict SUCCESS/FAILURE
 *   test=panic    panique volontaire (vérifie le chemin panic, statut 37)
 *   test=ud|de|gp|np|df|triple   exceptions fatales volontaires (K0.4,
 *                 voir test_faults.c)
 *   test=kbd      ligne tapée au clavier (frappes injectées par QEMU, K0.5)
 */
#include <samos/bootinfo.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/panic.h>
#include <samos/qemu_dbg.h>
#include <samos/string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_suite;          /* suite en cours */
static unsigned g_suite_checks0;     /* compteurs au début de la suite */
static unsigned g_suite_fail0;
static unsigned g_suites;

/* Bilan de la suite en cours. Le banc de test exige pour CHAQUE suite une
 * ligne « done: N checks » avec N > 0 : une suite qui ne serait plus appelée,
 * ou qui ne vérifierait plus rien, ne peut donc pas passer inaperçue. */
static void suite_close(void)
{
    if (g_suite != 0) {
        kprintf("[TEST] suite %s done: %u checks, %u failures\n", g_suite,
                g_checks - g_suite_checks0, g_failures - g_suite_fail0);
    }
}

void ktest_suite(const char *name)
{
    suite_close();
    g_suite = name;
    g_suite_checks0 = g_checks;
    g_suite_fail0 = g_failures;
    g_suites++;
    kprintf("[TEST] suite %s\n", name);
}

void ktest_check(bool cond, const char *expr, const char *file, int line)
{
    g_checks++;
    if (cond) {
        kprintf("[PASS] %s\n", expr);
        return;
    }
    g_failures++;
    kprintf("[FAIL] %s at %s:%d\n", expr, file, line);
}

unsigned ktest_failures(void)
{
    return g_failures;
}

/* Fonctions imbriquées non inlinées : la trace de pile doit montrer plusieurs
 * cadres. */
static __attribute__((noinline)) SAMOS_NORETURN void deliberate_panic_inner(int code)
{
    panic("deliberate test panic (%d)", code);
}

static __attribute__((noinline)) SAMOS_NORETURN void deliberate_panic(void)
{
    deliberate_panic_inner(42);
}

void ktest_run_all(void)
{
    char mode[32];
    if (cmdline_value("test", mode, sizeof(mode))) {
        if (strcmp(mode, "panic") == 0) {
            deliberate_panic();
        }
        if (strcmp(mode, "kbd") == 0) {
            ktest_keyboard_line();
            qemu_exit(QEMU_EXIT_SUCCESS);   /* le verdict porte sur la ligne affichée */
        }
        if (ktest_fatal_mode(mode)) {
            kprintf("[TEST] fatal mode '%s' returned: fault not raised\n", mode);
            qemu_exit(QEMU_EXIT_FAILURE);
        }
        kprintf("[TEST] unknown test mode '%s'\n", mode);
        qemu_exit(QEMU_EXIT_FAILURE);
    }

    ktest_suite_boot();
    ktest_suite_layout();
    ktest_suite_console();
    ktest_suite_cpu();
    ktest_suite_irq();
    ktest_suite_mm();
    ktest_suite_canary();
    ktest_suite_df_sentinel();
    ktest_suite_stack_layout();
    ktest_suite_top_guard();

    suite_close();
    kprintf("[TEST] suites=%u checks=%u failures=%u\n", g_suites, g_checks, g_failures);
    if (g_failures == 0) {
        kprintf("[TEST] ALL TESTS PASSED\n");
        qemu_exit(QEMU_EXIT_SUCCESS);
    }
    kprintf("[TEST] SOME TESTS FAILED\n");
    qemu_exit(QEMU_EXIT_FAILURE);
}
