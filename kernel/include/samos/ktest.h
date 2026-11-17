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

#endif /* SAMOS_KTEST_H */
