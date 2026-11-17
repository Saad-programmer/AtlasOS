/*
 * SAMOS / AtlasOS — kernel/include/samos/vga.h
 * ---------------------------------------------------------------------------
 * Pilote du mode texte VGA 80x25 (K0.3).
 *
 * Matériel : en mode texte couleur, la carte VGA projette un tampon de
 * 80 x 25 cellules de 16 bits à l'adresse PHYSIQUE 0xB8000 (E/S projetées en
 * mémoire). Chaque cellule = octet bas : code de caractère (page de code 437),
 * octet haut : attribut (bits 0-3 couleur du texte, 4-6 couleur du fond,
 * bit 7 clignotement ou fond intense selon la configuration).
 * Le curseur matériel est piloté par le contrôleur CRTC (ports d'E/S
 * index/données 0x3D4/0x3D5 en mode couleur, registres 0x0E/0x0F).
 *
 * Limite : disponible uniquement avec un BIOS (ou CSM) ; un firmware UEFI pur
 * ne fournit pas de mode texte VGA. Le port série est le canal de secours.
 */
#ifndef SAMOS_VGA_H
#define SAMOS_VGA_H

#include <stddef.h>
#include <stdint.h>

#define VGA_COLS 80
#define VGA_ROWS 25

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE = 1, VGA_GREEN = 2, VGA_CYAN = 3,
    VGA_RED = 4, VGA_MAGENTA = 5, VGA_BROWN = 6, VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8, VGA_LIGHT_BLUE = 9, VGA_LIGHT_GREEN = 10, VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12, VGA_LIGHT_MAGENTA = 13, VGA_YELLOW = 14, VGA_WHITE = 15,
};

void vga_init(void);
void vga_clear(void);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_putc(char c);
void vga_write(const char *s, size_t n);

/* Accès pour diagnostic et tests */
void vga_set_pos(unsigned row, unsigned col);   /* bornées à l'écran */
void vga_get_pos(unsigned *row, unsigned *col);
uint16_t vga_cell(unsigned row, unsigned col);   /* 0 si hors écran */
uint16_t vga_hw_cursor(void);                    /* position lue dans le CRTC */

#endif /* SAMOS_VGA_H */
