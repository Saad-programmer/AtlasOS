/*
 * SAMOS / AtlasOS — kernel/arch/i386/irq.c
 * Répartition des IRQ matérielles. Voir irq.h et Details.md §28.
 */
#include <arch/idt.h>
#include <arch/irq.h>
#include <arch/pic.h>

/* K0.7-P4b — Même page que g_handlers (.bss.p4_handlers). */
static irq_handler_t g_irq_handlers[PIC_IRQ_COUNT] SAMOS_SECTION(".bss.p4_handlers");
static volatile uint32_t g_irq_count[PIC_IRQ_COUNT];
static volatile uint32_t g_spurious;
static volatile uint32_t g_unhandled;
extern volatile uint32_t g_irq_depth;      /* exceptions.c : cpu_in_interrupt() */

bool irq_register(uint8_t irq, irq_handler_t handler)
{
    if (irq >= PIC_IRQ_COUNT || handler == 0 || g_irq_handlers[irq] != 0) {
        return false;
    }
    g_irq_handlers[irq] = handler;
    idt_set_present((uint8_t)(PIC_IRQ_BASE + irq), true);
    pic_unmask(irq);
    return true;
}

void irq_unregister(uint8_t irq)
{
    if (irq >= PIC_IRQ_COUNT) {
        return;
    }
    pic_mask(irq);
    g_irq_handlers[irq] = 0;
}

/*
 * IRQ parasite (spurious) : si une ligne IRQ retombe avant que le CPU n'ait
 * acquitté l'interruption, le 8259A signale quand même une interruption sur
 * sa ligne de plus basse priorité (IRQ 7 maître, IRQ 15 esclave) SANS mettre
 * le bit correspondant dans l'ISR. Il ne faut alors PAS envoyer d'EOI à ce
 * contrôleur (cela acquitterait une autre IRQ réellement en service). Pour un
 * parasite de l'esclave (15), le maître, lui, a bien vu une IRQ 2 réelle et
 * doit recevoir son EOI.
 */
static bool is_spurious(uint8_t irq)
{
    if (irq != 7 && irq != 15) {
        return false;
    }
    if (pic_isr() & (1u << irq)) {
        return false;
    }
    g_spurious++;
    if (irq == 15) {
        pic_eoi(PIC_CASCADE_IRQ);      /* EOI au maître seulement */
    }
    return true;
}

void irq_dispatch(struct interrupt_frame *frame)
{
    uint8_t irq = (uint8_t)(frame->vector - PIC_IRQ_BASE);
    if (irq >= PIC_IRQ_COUNT || is_spurious(irq)) {
        return;
    }
    g_irq_count[irq]++;
    g_irq_depth++;
    irq_handler_t h = g_irq_handlers[irq];
    if (h != 0) {
        h();
    } else {
        g_unhandled++;                 /* ne devrait pas arriver : IRQ masquée */
    }
    g_irq_depth--;
    pic_eoi(irq);                      /* après le gestionnaire : IF = 0, pas d'imbrication */
}

uint32_t irq_count(uint8_t irq) { return (irq < PIC_IRQ_COUNT) ? g_irq_count[irq] : 0; }
uint32_t irq_spurious_count(void) { return g_spurious; }
uint32_t irq_unhandled_count(void) { return g_unhandled; }
