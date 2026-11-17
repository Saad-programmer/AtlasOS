/*
 * SAMOS / AtlasOS — kernel/include/samos/pit.h
 * ---------------------------------------------------------------------------
 * Minuterie programmable Intel 8253/8254 (PIT), canal 0 sur IRQ 0 (K0.5).
 *
 * Le PIT décrémente un compteur 16 bits à la fréquence d'un quartz de
 * 1,193182 MHz (= 14,31818 MHz / 12, héritage de l'IBM PC). En mode 2
 * (« rate generator »), il produit une impulsion — donc une IRQ 0 — chaque
 * fois que le compteur atteint 1, puis se recharge avec le diviseur :
 *     f_irq = 1 193 182 / diviseur
 * SAMOS utilise 100 Hz : diviseur 11932, f réelle = 99,998 Hz.
 */
#ifndef SAMOS_PIT_H
#define SAMOS_PIT_H

#include <stdbool.h>
#include <stdint.h>

#define PIT_BASE_HZ 1193182u
#define PIT_DEFAULT_HZ 100u

/* hz doit être dans [19, 10000] (diviseur 16 bits, coût des IRQ borné). */
bool pit_init(uint32_t hz);

uint64_t pit_ticks(void);          /* lecture atomique du compteur 64 bits */
uint32_t pit_hz(void);             /* fréquence demandée */
uint32_t pit_divisor(void);
uint64_t pit_uptime_ms(void);      /* calcul exact à partir du diviseur réel */
uint16_t pit_read_counter(void);   /* valeur instantanée du compteur matériel */

/* Attend au moins `ms` millisecondes en dormant (HLT). Exige IF = 1 ;
 * retourne false (sans attendre) si les interruptions sont masquées. */
bool pit_sleep_ms(uint32_t ms);

#endif /* SAMOS_PIT_H */
