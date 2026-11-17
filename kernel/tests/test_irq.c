/*
 * SAMOS / AtlasOS — kernel/tests/test_irq.c
 * ---------------------------------------------------------------------------
 * Autotests du jalon K0.5 : PIC, PIT, IRQ, file circulaire, décodeur clavier.
 * Cette suite ACTIVE les interruptions (STI) et les remasque à la fin.
 */
#include <arch/cpu.h>
#include <arch/idt.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/pic.h>
#include <samos/kbd_decode.h>
#include <samos/keyboard.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pit.h>
#include <samos/ringbuf.h>

/* --- Horloge CMOS (RTC) : référence indépendante pour étalonner le PIT --- */
#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71
static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg);   /* bit 7 = 0 : NMI laissé actif */
    return inb(CMOS_DATA);
}
static uint8_t rtc_seconds(void)
{
    while (cmos_read(0x0A) & 0x80u) {   /* UIP : mise à jour en cours, attendre */
    }
    return cmos_read(0x00);
}
/* Attend le prochain changement de seconde RTC (borné par le TSC). */
static bool rtc_wait_edge(void)
{
    uint8_t s0 = rtc_seconds();
    uint64_t t0 = cpu_rdtsc();
    while (rtc_seconds() == s0) {
        if (cpu_rdtsc() - t0 > 20000000000ull) {   /* ~10 s à 2 GHz : garde-fou */
            return false;
        }
    }
    return true;
}

static bool wait_ticks(uint64_t n)
{
    uint64_t target = pit_ticks() + n;
    uint64_t t0 = cpu_rdtsc();
    while (pit_ticks() < target) {
        if (cpu_rdtsc() - t0 > 20000000000ull) {
            return false;                    /* le timer ne bat pas : échec, pas blocage */
        }
        __asm__ __volatile__("pause");
    }
    return true;
}

static void test_pic_state(void)
{
    uint16_t m = pic_masks();
    KTEST_CHECK((m & 0x0001u) == 0);                 /* IRQ0 (PIT) démasquée */
    KTEST_CHECK((m & 0x0002u) == 0);                 /* IRQ1 (clavier) démasquée */
    KTEST_CHECK((m & 0xFFF8u) == 0xFFF8u);           /* toutes les autres masquées (IRQ2 aussi) */
    const struct idt_entry *t = idt_table();
    KTEST_CHECK((t[PIC_IRQ_BASE + 0].type_attr & IDT_PRESENT) != 0);
    KTEST_CHECK((t[PIC_IRQ_BASE + 1].type_attr & IDT_PRESENT) != 0);
    KTEST_CHECK((t[PIC_IRQ_BASE + 2].type_attr & IDT_PRESENT) == 0);
    KTEST_CHECK(!irq_register(0, (irq_handler_t)0));  /* nul refusé */
    KTEST_CHECK(!irq_register(16, (irq_handler_t)0)); /* hors limites refusé */
    KTEST_CHECK(pic_isr() == 0);                     /* rien en service hors IRQ */
}

static void test_timer(void)
{
    KTEST_CHECK(pit_hz() == 100 && pit_divisor() == 11932);
    uint16_t c = pit_read_counter();
    KTEST_CHECK(c > 0 && c <= 11932);                /* compteur matériel dans [1, diviseur] */

    cpu_sti();
    uint64_t t0 = pit_ticks();
    uint32_t c0 = irq_count(0);
    bool ok = wait_ticks(10);
    uint64_t dt = pit_ticks() - t0;
    uint32_t dc = irq_count(0) - c0;
    KTEST_CHECK(ok && dt >= 10);                     /* les IRQ 0 se succèdent => EOI correct */
    KTEST_CHECK(dc >= 10 && dc <= dt + 1);           /* une IRQ comptée par tick */
    KTEST_CHECK(irq_unhandled_count() == 0);

    /* Étalonnage : nombre de ticks pendant UNE seconde de l'horloge CMOS. */
    uint64_t per_sec = 0;
    if (rtc_wait_edge()) {
        uint64_t a = pit_ticks();
        if (rtc_wait_edge()) {
            per_sec = pit_ticks() - a;
        }
    }
    kprintf("[TEST] PIT ticks per RTC second: %llu (expected ~100)\n", (unsigned long long)per_sec);
    KTEST_CHECK(per_sec >= 90 && per_sec <= 110);

    /* pit_sleep_ms garantit AU MOINS la durée demandée. */
    uint64_t m0 = pit_uptime_ms();
    KTEST_CHECK(pit_sleep_ms(50));
    uint64_t slept = pit_uptime_ms() - m0;
    KTEST_CHECK(slept >= 50 && slept < 200);

    /* Masquage : IRQ 0 masquée => plus aucun tick pendant ~5 périodes. */
    uint64_t tsc_a = cpu_rdtsc();
    (void)wait_ticks(5);
    uint64_t tsc_per_5ticks = cpu_rdtsc() - tsc_a;
    pic_mask(0);
    uint64_t frozen = pit_ticks();
    uint64_t tsc_b = cpu_rdtsc();
    while (cpu_rdtsc() - tsc_b < 2 * tsc_per_5ticks) {
        __asm__ __volatile__("pause");
    }
    uint64_t after_mask = pit_ticks();
    pic_unmask(0);
    bool resumed = wait_ticks(3);
    KTEST_CHECK(after_mask == frozen);               /* masquée : aucune IRQ délivrée */
    KTEST_CHECK(resumed);                            /* démasquée : reprise */
    KTEST_CHECK(irq_spurious_count() == 0);
    cpu_cli();

    /* Interruptions de nouveau masquées : sleep doit refuser plutôt que bloquer. */
    KTEST_CHECK(!pit_sleep_ms(10));
}

static void test_ringbuf(void)
{
    static uint8_t store[8];
    struct ringbuf rb;
    KTEST_CHECK(!ringbuf_init(&rb, store, 6));       /* pas une puissance de 2 */
    KTEST_CHECK(ringbuf_init(&rb, store, 8));
    uint8_t v = 0;
    KTEST_CHECK(!ringbuf_pop(&rb, &v));              /* vide */
    bool all = true;
    for (uint8_t i = 0; i < 8; i++) {
        all = all && ringbuf_push(&rb, i);
    }
    KTEST_CHECK(all && ringbuf_count(&rb) == 8);
    KTEST_CHECK(!ringbuf_push(&rb, 99) && rb.dropped == 1);   /* plein : refus compté */
    KTEST_CHECK(ringbuf_pop(&rb, &v) && v == 0);     /* FIFO */
}

static void test_decoder(void)
{
    struct kbd_decoder d;
    struct key_event ev;
    kbd_decoder_init(&d);
    KTEST_CHECK(kbd_decode(&d, 0x1F, &ev) && ev.ascii == 's' && ev.pressed);
    KTEST_CHECK(kbd_decode(&d, 0x9F, &ev) && !ev.pressed && ev.ascii == 0);
    KTEST_CHECK(kbd_decode(&d, 0x2A, &ev));          /* Shift gauche appuyé */
    KTEST_CHECK(kbd_decode(&d, 0x1F, &ev) && ev.ascii == 'S');
    KTEST_CHECK(kbd_decode(&d, 0xAA, &ev) && (ev.mods & KMOD_SHIFT) == 0);
    KTEST_CHECK(!kbd_decode(&d, 0xE0, &ev));         /* préfixe : pas d'événement */
    KTEST_CHECK(kbd_decode(&d, 0x48, &ev) && ev.keycode == KEY_UP && ev.ascii == 0);
}

static void test_keyboard_driver(void)
{
    const char *e = keyboard_error();
    KTEST_CHECK(e[0] == 'o' && e[1] == 'k');         /* initialisation complète réussie */
    KTEST_CHECK(keyboard_dropped() == 0);
}

void ktest_suite_irq(void)
{
    ktest_suite("K0.5 irq");
    test_pic_state();
    test_timer();
    test_ringbuf();
    test_decoder();
    test_keyboard_driver();
}

/*
 * Mode interactif test=kbd : le script hôte injecte de vraies frappes
 * (moniteur QEMU « sendkey ») ; le noyau les reçoit par IRQ 1, les décode et
 * renvoie la ligne tapée jusqu'à Entrée.
 */
void ktest_keyboard_line(void)
{
    char line[64];
    unsigned n = 0;
    struct key_event ev;
    cpu_sti();
    kprintf("[TEST] kbd ready\n");
    uint64_t deadline = pit_ticks() + 15u * pit_hz();
    while (pit_ticks() < deadline) {
        cpu_cli();
        if (!keyboard_get_event(&ev)) {
            cpu_idle();
            continue;
        }
        cpu_sti();
        if (!ev.pressed || ev.ascii == 0) {
            continue;
        }
        if (ev.ascii == '\n') {
            line[n] = '\0';
            kprintf("[TEST] kbd line: '%s' (bytes=%u dropped=%u irq1=%u)\n", line,
                    keyboard_bytes_received(), keyboard_dropped(), irq_count(1));
            return;
        }
        if (n < sizeof(line) - 1) {
            line[n++] = ev.ascii;
        }
    }
    line[n] = '\0';
    kprintf("[TEST] kbd timeout, partial line: '%s' (bytes=%u irq1=%u)\n", line,
            keyboard_bytes_received(), irq_count(1));
}
