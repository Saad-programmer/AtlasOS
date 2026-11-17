/*
 * SAMOS / AtlasOS — kernel/drivers/vga.c
 * Pilote VGA mode texte. Voir vga.h et Details.md §18.
 *
 * Invariants :
 *   - g_row < VGA_ROWS et g_col < VGA_COLS à la sortie de chaque fonction
 *     publique : aucune écriture hors du tampon de 4000 octets.
 *   - Tous les accès au tampon passent par un pointeur volatile : chaque
 *     écriture est réellement émise vers la mémoire vidéo (pas de mise en
 *     cache dans un registre ni d'élimination par l'optimiseur).
 */
#include <samos/vga.h>
#include <arch/cpu.h>
#include <arch/io.h>

#define VGA_PHYS_ADDR   0xB8000u
#define VGA_TAB_WIDTH   8

/* Registres VGA (ports d'E/S) */
#define VGA_MISC_OUTPUT_READ 0x3CC   /* bit 0 : 1 = E/S couleur en 0x3Dx, 0 = mono en 0x3Bx */
#define CRTC_CURSOR_START    0x0A    /* bit 5 : curseur désactivé */
#define CRTC_CURSOR_END      0x0B
#define CRTC_CURSOR_HIGH     0x0E
#define CRTC_CURSOR_LOW      0x0F

static volatile uint16_t *const g_buf = (volatile uint16_t *)(uintptr_t)VGA_PHYS_ADDR;
static unsigned g_row;
static unsigned g_col;
static uint8_t g_attr = (uint8_t)(VGA_LIGHT_GREY | (VGA_BLACK << 4));
static uint16_t g_crtc_index = 0x3D4;

static inline uint16_t make_cell(char c, uint8_t attr)
{
    return (uint16_t)((uint16_t)(uint8_t)c | ((uint16_t)attr << 8));
}

static void crtc_write(uint8_t reg, uint8_t value)
{
    outb(g_crtc_index, reg);
    outb((uint16_t)(g_crtc_index + 1), value);
}

static uint8_t crtc_read(uint8_t reg)
{
    outb(g_crtc_index, reg);
    return inb((uint16_t)(g_crtc_index + 1));
}

static void update_hw_cursor(void)
{
    uint16_t pos = (uint16_t)(g_row * VGA_COLS + g_col);
    crtc_write(CRTC_CURSOR_HIGH, (uint8_t)(pos >> 8));
    crtc_write(CRTC_CURSOR_LOW, (uint8_t)(pos & 0xFFu));
}

/* Fait remonter l'écran d'une ligne : la ligne 0 est perdue, la dernière
 * ligne est effacée. Coût : 80*24 copies de 16 bits. */
static void scroll(void)
{
    for (unsigned i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++) {
        g_buf[i] = g_buf[i + VGA_COLS];
    }
    for (unsigned c = 0; c < VGA_COLS; c++) {
        g_buf[(VGA_ROWS - 1) * VGA_COLS + c] = make_cell(' ', g_attr);
    }
}

static void newline(void)
{
    g_col = 0;
    if (g_row + 1 < VGA_ROWS) {
        g_row++;
    } else {
        scroll();
    }
}

void vga_clear(void)
{
    for (unsigned i = 0; i < VGA_ROWS * VGA_COLS; i++) {
        g_buf[i] = make_cell(' ', g_attr);
    }
    g_row = 0;
    g_col = 0;
    update_hw_cursor();
}

void vga_init(void)
{
    /* Choisir les ports CRTC selon le mode couleur/mono configuré par le BIOS. */
    g_crtc_index = (inb(VGA_MISC_OUTPUT_READ) & 0x01u) ? 0x3D4 : 0x3B4;
    /* Curseur visible, forme « soulignement » (lignes de balayage 14-15). */
    crtc_write(CRTC_CURSOR_START, (uint8_t)((crtc_read(CRTC_CURSOR_START) & 0xC0u) | 14u));
    crtc_write(CRTC_CURSOR_END, (uint8_t)((crtc_read(CRTC_CURSOR_END) & 0xE0u) | 15u));
    vga_clear();
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    g_attr = (uint8_t)(((unsigned)fg & 0x0Fu) | (((unsigned)bg & 0x07u) << 4));
}

static void putc_nocursor(char c)
{
    /* Défense en profondeur (K0.7) : si l'état a été laissé incohérent (par
     * exemple une panique survenue au milieu d'une écriture), on le ramène
     * dans les bornes AVANT tout accès au tampon. */
    if (g_row >= VGA_ROWS) {
        g_row = VGA_ROWS - 1;
    }
    if (g_col >= VGA_COLS) {
        newline();
    }
    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        g_col = 0;
        break;
    case '\t':
        /* Avancer jusqu'au prochain multiple de 8 (au moins une espace) ;
         * un retour à la ligne automatique (g_col == 0) termine aussi. */
        do {
            putc_nocursor(' ');
        } while (g_col % VGA_TAB_WIDTH != 0);
        break;
    case '\b':
        if (g_col > 0) {
            g_col--;
            g_buf[g_row * VGA_COLS + g_col] = make_cell(' ', g_attr);
        }
        break;
    default:
        g_buf[g_row * VGA_COLS + g_col] = make_cell(c, g_attr);
        if (++g_col == VGA_COLS) {
            newline();
        }
        break;
    }
}

/* L'état du curseur (g_row, g_col) est partagé entre le code normal et une
 * éventuelle panique en contexte d'interruption : les écritures sont faites
 * interruptions masquées (K0.7). Coût : quelques microsecondes par ligne
 * (écritures en mémoire vidéo), sans effet mesurable sur le PIT à 100 Hz. */
void vga_putc(char c)
{
    uint32_t f = irq_save();
    putc_nocursor(c);
    update_hw_cursor();
    irq_restore(f);
}

void vga_write(const char *s, size_t n)
{
    uint32_t f = irq_save();
    for (size_t i = 0; i < n; i++) {
        putc_nocursor(s[i]);
    }
    update_hw_cursor();   /* une seule mise à jour du curseur : 4 E/S au lieu de 4n */
    irq_restore(f);
}

void vga_set_pos(unsigned row, unsigned col)
{
    g_row = (row < VGA_ROWS) ? row : VGA_ROWS - 1;
    g_col = (col < VGA_COLS) ? col : VGA_COLS - 1;
    update_hw_cursor();
}

void vga_get_pos(unsigned *row, unsigned *col)
{
    *row = g_row;
    *col = g_col;
}

uint16_t vga_cell(unsigned row, unsigned col)
{
    if (row >= VGA_ROWS || col >= VGA_COLS) {
        return 0;
    }
    return g_buf[row * VGA_COLS + col];
}

uint16_t vga_hw_cursor(void)
{
    return (uint16_t)(((uint16_t)crtc_read(CRTC_CURSOR_HIGH) << 8) | crtc_read(CRTC_CURSOR_LOW));
}
