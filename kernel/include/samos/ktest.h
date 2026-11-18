/*
 * SAMOS / AtlasOS — kernel/include/samos/ktest.h
 * ---------------------------------------------------------------------------
 * Mini-cadre d'autotests exécutés DANS le noyau, sous QEMU, lorsque la ligne
 * de commande contient le jeton `test`. Chaque suite vérifie les invariants
 * d'un jalon K0. Le résultat global est transmis au script hôte via
 * isa-debug-exit (voir qemu_dbg.h) et le détail est journalisé.
 */
#ifndef SAMOS_KTEST_H
#define SAMOS_KTEST_H

#include <stdbool.h>

void ktest_suite(const char *name);
void ktest_check(bool cond, const char *expr, const char *file, int line);
unsigned ktest_failures(void);

#define KTEST_CHECK(cond) ktest_check((cond), #cond, __FILE__, __LINE__)

/* Exécute toutes les suites, puis termine QEMU avec SUCCESS ou FAILURE. */
_Noreturn void ktest_run_all(void);

/* Suites (une par jalon), définies dans kernel/tests/. */
void ktest_suite_boot(void);       /* K0.1 */
void ktest_suite_layout(void);     /* K0.2 */
void ktest_suite_console(void);    /* K0.3 */
void ktest_suite_cpu(void);        /* K0.4 */
void ktest_suite_irq(void);        /* K0.5 */
void ktest_suite_mm(void);         /* K0.6 */
void ktest_keyboard_line(void);    /* K0.5, mode test=kbd */
void ktest_suite_canary(void);     /* K0.7-R1b */
void ktest_canary_smash(void);     /* K0.7-R1b, mode test=canary */

/* Déclenche volontairement une faute fatale (modes test=ud, de, gp, np,
 * intbad, df, triple ; K0.6 : null, wtext, wrodata, unmapped, stack). Retourne false si le mode est inconnu. */
bool ktest_fatal_mode(const char *mode);

#endif /* SAMOS_KTEST_H */
