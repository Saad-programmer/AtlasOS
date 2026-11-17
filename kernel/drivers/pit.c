/*
 * SAMOS / AtlasOS — kernel/drivers/pit.c
 * PIT 8254, canal 0. Voir pit.h et Details.md §28.3.
 */
#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <samos/math64.h>
#include <samos/pit.h>

#define PIT_CH0_DATA 0x40
#define PIT_CMD      0x43
/* Octet de commande : canal 0 (00), accès poids faible puis fort (11),
 * mode 2 (010), binaire (0) => 0b00110100 */
#define PIT_CMD_CH0_LOHI_MODE2 0x34u
#define PIT_CMD_LATCH_CH0      0x00u

static volatile uint64_t g_ticks;
static uint32_t g_hz;
static uint32_t g_divisor;

/* Gestionnaire d'IRQ 0 : une seule incrémentation. L'incrément 64 bits
 * s'exécute avec IF = 0 (porte d'interruption) : il n'est jamais interrompu. */
static void pit_irq(void)
{
    g_ticks++;
}

bool pit_init(uint32_t hz)
{
    if (hz < 19 || hz > 10000) {
        return false;
    }
    /* Diviseur arrondi au plus proche : (base + hz/2) / hz. */
    uint32_t divisor = (PIT_BASE_HZ + hz / 2u) / hz;
    g_hz = hz;
    g_divisor = divisor;
    g_ticks = 0;
    outb(PIT_CMD, PIT_CMD_CH0_LOHI_MODE2);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CH0_DATA, (uint8_t)(divisor >> 8));
    return irq_register(0, pit_irq);
}

uint64_t pit_ticks(void)
{
    /* Sur i386, lire un uint64_t prend DEUX instructions de 32 bits. Une IRQ
     * entre les deux pourrait faire lire la moitié basse avant et la moitié
     * haute après une retenue : valeur « déchirée ». On masque donc les
     * interruptions pendant la lecture. */
    uint32_t f = irq_save();
    uint64_t t = g_ticks;
    irq_restore(f);
    return t;
}

uint32_t pit_hz(void) { return g_hz; }
uint32_t pit_divisor(void) { return g_divisor; }

uint64_t pit_uptime_ms(void)
{
    /* ms = ticks * diviseur * 1000 / 1193182 : exact, sans dérive due à
     * l'arrondi de la fréquence (99,998 Hz et non 100 Hz). */
    return u64_divmod_u32(pit_ticks() * g_divisor * 1000u, PIT_BASE_HZ, 0);
}

uint16_t pit_read_counter(void)
{
    uint32_t f = irq_save();
    outb(PIT_CMD, PIT_CMD_LATCH_CH0);  /* fige la valeur courante pour la lecture */
    uint8_t lo = inb(PIT_CH0_DATA);
    uint8_t hi = inb(PIT_CH0_DATA);
    irq_restore(f);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

bool pit_sleep_ms(uint32_t ms)
{
    if ((cpu_read_eflags() & EFLAGS_IF) == 0 || g_hz == 0) {
        return false;                  /* dormir sans interruptions = ne jamais se réveiller */
    }
    /* Nombre de ticks arrondi au supérieur, +1 car le tick courant est
     * déjà entamé : on garantit AU MOINS `ms`. */
    /* Division 64 bits via u64_divmod_u32 : « / » produirait __udivdi3 (libgcc). */
    uint64_t wait = u64_divmod_u32((uint64_t)ms * g_hz + 999u, 1000u, 0) + 1u;
    uint64_t target = pit_ticks() + wait;
    for (;;) {
        cpu_cli();
        if (g_ticks >= target) {
            cpu_sti();
            return true;
        }
        cpu_idle();                    /* sti; hlt : sans course (voir cpu.h) */
    }
}
