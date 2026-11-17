/*
 * SAMOS / AtlasOS — kernel/arch/i386/linker.ld
 * ---------------------------------------------------------------------------
 * Script de l'éditeur de liens GNU ld pour le noyau i386 (K0).
 *
 * Disposition :
 *   - Le noyau est lié ET chargé à l'adresse physique 1 MiB (0x00100000),
 *     en identité (adresse virtuelle = adresse physique, pagination inactive).
 *     Sous 1 MiB se trouvent l'IVT, la BDA, l'EBDA, la mémoire vidéo
 *     (0xA0000-0xBFFFF) et les ROM du BIOS (0xC0000-0xFFFFF).
 *   - Trois segments de chargement (program headers) avec des permissions
 *     distinctes afin qu'aucun segment ne soit à la fois inscriptible et
 *     exécutable (W^X au niveau du fichier ELF) :
 *         text   : R-X  (.multiboot + .text)
 *         rodata : R--  (.rodata)
 *         data   : RW-  (.data + .bss)
 *     Tant que la pagination est inactive (avant K0.6), le CPU n'applique PAS
 *     ces permissions ; elles documentent l'intention et seront appliquées
 *     par les tables de pages en K0.6.
 *   - Chaque section commence sur une frontière de 4 KiB (taille de page)
 *     pour que la pagination puisse attribuer des droits différents.
 *   - Des symboles __*_start / __*_end exposent la disposition au code C/ASM.
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

KERNEL_LOAD_ADDR = 0x00100000;
PAGE_SIZE        = 0x1000;

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4);   /* PF_R        */
    data   PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = KERNEL_LOAD_ADDR;
    __kernel_start = .;

    .text ALIGN(PAGE_SIZE) :
    {
        __text_start = .;
        /* L'en-tête Multiboot DOIT être le premier contenu du premier
         * segment pour se trouver dans les 8 premiers KiB du fichier.
         * KEEP empêche toute élimination (--gc-sections). */
        KEEP(*(.multiboot))
        *(.text .text.*)
        __text_end = .;
    } :text

    .rodata ALIGN(PAGE_SIZE) :
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } :rodata

    .data ALIGN(PAGE_SIZE) :
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } :data

    /* .bss : NOBITS, n'occupe aucun octet dans le fichier. Le début et la
     * fin sont alignés sur 4 octets pour que entry.asm puisse la remplir de
     * zéros mot par mot (rep stosd). */
    .bss ALIGN(PAGE_SIZE) :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } :data

    . = ALIGN(PAGE_SIZE);
    __kernel_end = .;

    /* Sections inutiles pour un noyau freestanding. Les sections DWARF
     * (.debug_*) sont conservées (non chargées) pour GDB. */
    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}

/* Vérifications statiques effectuées au moment de l'édition de liens. */
ASSERT(__kernel_start == KERNEL_LOAD_ADDR, "noyau non lie a 1 MiB")
ASSERT((__bss_start % 4) == 0, ".bss non alignee sur 4")
ASSERT((__bss_end % 4) == 0, "fin de .bss non alignee sur 4")
