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
void ktest_suite_df_sentinel(void); /* K0.7-R1d */
void ktest_suite_stack_layout(void); /* K0.7-R1f-1 */
void ktest_suite_top_guard(void);   /* K0.7-R1f-2 */
void ktest_top_guard_write(bool last_word); /* K0.7-R1f-2, test=topguard[_end] */
void ktest_suite_df_guard(void);    /* K0.7-R1f-3 */
void ktest_df_guard_write(bool last_word);  /* K0.7-R1f-3, test=dfguard[_end] */
/* K0.7-R1h (F1) : motif dans S2 avant gdt_init (appelée par kmain, image de
 * test seulement) ; mémorise le nombre de mots relus égaux au motif. */
void ktest_s2_prefill(void);
void ktest_suite_s2_init(void);     /* K0.7-R1h (F1) */
void ktest_suite_critical_layout(void); /* K0.7-P4b */
void ktest_suite_ptgate(void);      /* K0.7-P4c (prototype, image de test) */
void ktest_suite_sealed(void);      /* K0.7-P4d (après scellement) */
void ktest_suite_vmm_gate(void);    /* K0.7-P4e-1 */
void ktest_suite_pt_seal(void);     /* K0.7-P4e-2 */
void ktest_pt_sealed_write(const char *what); /* K0.7-P4e-2, test=sealpd|sealpt|sealnewpt */
void ktest_sealed_write(const char *what); /* K0.7-P4d, test=sealidt|sealhandlers|sealvmm */
#define KTEST_S2_PREFILL 0x5F2A11EDu

/* Déclenche volontairement une faute fatale (modes test=ud, de, gp, np,
 * intbad, df, triple ; K0.6 : null, wtext, wrodata, unmapped, stack). Retourne false si le mode est inconnu. */
bool ktest_fatal_mode(const char *mode);

#endif /* SAMOS_KTEST_H */
