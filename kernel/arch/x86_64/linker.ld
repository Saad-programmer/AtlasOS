/*
 * SAMOS / AtlasOS — kernel/arch/x86_64/linker.ld            (K1.1-M64-1)
 * ---------------------------------------------------------------------------
 * Script de liaison du SQUELETTE x86-64, NON AMORÇABLE.
 *
 * Objectif : un exécutable ELF64 statique, à adresse fixe, dont la
 * disposition est vérifiable statiquement (tools/check_elf64.sh).
 *
 * HYPOTHÈSE LOCALE DE M64-1 (pas une décision d'architecture) :
 *   KERNEL_LOAD_ADDR = 1 MiB, adresse virtuelle = adresse physique, comme
 *   l'i386. C'est la seule disposition qui permet aujourd'hui de lier sans
 *   trancher D-LAYOUT (identité basse ou noyau « higher-half ») ni A-6
 *   (modèle de code). Avec -mcmodel=small (mk/x86_64.mk), toutes les
 *   adresses doivent tenir sous 2 GiB : c'est le cas ici. Ce choix sera
 *   réexaminé par D-LAYOUT ; il ne préjuge pas du format d'image (A-1).
 *
 * Disposition (chaque section de sortie commence sur une frontière de
 * 4 KiB ; une page ne contient qu'une classe de permissions, conception
 * §5.4.5, règle R4) :
 *   .text    R-X   code
 *   .rodata  R--   constantes
 *   .data    RW-   données initialisées
 *   .bss     RW-   données non initialisées (NOBITS)
 * Les trois PHDRS imposent trois segments PT_LOAD aux droits distincts ;
 * aucun n'est W+X (règle R5), et --fatal-warnings (mk/x86_64.mk) rejette un
 * segment RWX à la liaison.
 *
 * ABSENT VOLONTAIREMENT : en-tête Multiboot, section .multiboot, piles et
 * pages de garde, tables de pages, pages critiques P4 (M64-2 et suivants).
 * ---------------------------------------------------------------------------
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_LOAD_ADDR = 0x00100000;   /* hypothèse locale M64-1, voir ci-dessus */
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

    .bss ALIGN(PAGE_SIZE) :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } :data

    . = ALIGN(PAGE_SIZE);
    __kernel_end = .;

    /* Aucune métadonnée chargée : ni commentaire, ni note, ni table de
     * déroulement (le noyau n'utilise pas d'exceptions C++ ni d'unwinding). */
    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}

/* Contrôles de liaison (échec = image non produite). */
ASSERT(__kernel_start == KERNEL_LOAD_ADDR, "squelette x86-64 non lie a 1 MiB (hypothese M64-1)")
ASSERT((__text_start % PAGE_SIZE) == 0, "R4 : .text non alignee sur 4 KiB")
ASSERT((__rodata_start % PAGE_SIZE) == 0, "R4 : .rodata non alignee sur 4 KiB")
ASSERT((__data_start % PAGE_SIZE) == 0, "R4 : .data non alignee sur 4 KiB")
ASSERT((__bss_start % PAGE_SIZE) == 0, "R4 : .bss non alignee sur 4 KiB")
ASSERT(__text_end <= __rodata_start, "R4 : .text recouvre .rodata")
ASSERT(__rodata_end <= __data_start, "R4 : .rodata recouvre .data")
ASSERT(__data_end <= __bss_start, "R4 : .data recouvre .bss")
ASSERT(__kernel_end <= 0x00400000, "squelette > 4 MiB : hors de l'hypothese de la conception (§5.4)")
