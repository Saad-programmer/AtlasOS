/*
 * SAMOS / AtlasOS — kernel/arch/i386/pic.c
 * Programmation du 8259A. Voir pic.h et Details.md §28.
 */
#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/pic.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_ICW4  0x01u   /* ICW4 sera envoyé */
#define ICW1_INIT  0x10u   /* début d'initialisation (front, cascade, vecteurs sur 8) */
#define ICW4_8086  0x01u   /* mode 8086/88 (et non MCS-80/85) */
#define OCW2_EOI   0x20u   /* EOI non spécifique */
#define OCW3_READ_IRR 0x0Au
#define OCW3_READ_ISR 0x0Bu

void pic_init(void)
{
    /* ICW1 : démarre la séquence d'initialisation sur les deux contrôleurs. */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    /* ICW2 : vecteur de base (doit être multiple de 8). */
    outb(PIC1_DATA, PIC_IRQ_BASE);
    io_wait();
    outb(PIC2_DATA, PIC_IRQ_BASE + 8);
    io_wait();
    /* ICW3 : câblage de la cascade. Maître : bitmap des entrées reliées à un
     * esclave (bit 2). Esclave : numéro (binaire) de l'entrée du maître (2). */
    outb(PIC1_DATA, 1u << PIC_CASCADE_IRQ);
    io_wait();
    outb(PIC2_DATA, PIC_CASCADE_IRQ);
    io_wait();
    /* ICW4 : mode 8086, EOI manuel (non automatique). */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();
    /* OCW1 : tout masquer. Chaque pilote démasque explicitement son IRQ. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* Lecture-modification-écriture de l'IMR : elle doit être atomique vis-à-vis
 * des interruptions (K0.7). Sinon un gestionnaire qui modifierait l'IMR entre
 * le inb et le outb verrait sa modification écrasée par une valeur périmée. */
void pic_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint32_t f = irq_save();
    outb(port, (uint8_t)(inb(port) | (1u << (irq & 7u))));
    irq_restore(f);
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint32_t f = irq_save();
    outb(port, (uint8_t)(inb(port) & ~(1u << (irq & 7u))));
    irq_restore(f);
    if (irq >= 8) {
        pic_unmask(PIC_CASCADE_IRQ);   /* l'esclave n'est entendu que via IRQ 2 */
    }
}

void pic_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_CMD, OCW2_EOI);      /* l'esclave d'abord, puis le maître (cascade) */
    }
    outb(PIC1_CMD, OCW2_EOI);
}

uint16_t pic_masks(void)
{
    return (uint16_t)(((uint16_t)inb(PIC2_DATA) << 8) | inb(PIC1_DATA));
}

static uint16_t read_reg(uint8_t ocw3)
{
    outb(PIC1_CMD, ocw3);
    outb(PIC2_CMD, ocw3);
    return (uint16_t)(((uint16_t)inb(PIC2_CMD) << 8) | inb(PIC1_CMD));
}

uint16_t pic_isr(void) { return read_reg(OCW3_READ_ISR); }
uint16_t pic_irr(void) { return read_reg(OCW3_READ_IRR); }
