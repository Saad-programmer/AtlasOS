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
        /* K0.7-P4b — Structures critiques séparées par mutabilité, une page
         * (au moins) chacune, en tête de .bss : elles restent NOBITS et sont
         * remises à zéro par _start avec le reste de .bss. Les motifs
         * explicites précèdent *(.bss.*) : ld range une section d'entrée dans
         * la PREMIÈRE règle qui la désigne. Aucun changement de droits ici
         * (P4b) ; P4d scellera kro_idt, kro_handlers et kro_vmm. */
        __kcpu_start = .;            /* GDT + 2 TSS : écrites par le CPU, restent RW */
        *(.bss.p4_cpu)
        . = ALIGN(PAGE_SIZE);
        __kcpu_end = .;
        __kro_idt_start = .;         /* IDT : candidate RO */
        *(.bss.p4_idt)
        . = ALIGN(PAGE_SIZE);
        __kro_idt_end = .;
        __kro_handlers_start = .;    /* pointeurs de gestionnaires : candidates RO */
        *(.bss.p4_handlers)
        . = ALIGN(PAGE_SIZE);
        __kro_handlers_end = .;
        __kro_vmm_start = .;         /* racines du VMM : candidates RO */
        *(.bss.p4_vmm)
        . = ALIGN(PAGE_SIZE);
        __kro_vmm_end = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } :data

    /* K0.7-R1f-1 — Piles et pages de garde, dans UNE section NOBITS dont
     * l'ordre interne est fixé ici (il ne dépend plus de l'ordre des objets
     * de .bss, non garanti : constat R1d). De bas en haut, pages de 4 KiB :
     *   [__df_guard_start, __df_guard_end)   garde basse de S2 (R1f-3 : non
     *                                        présente ; R1f-1 : réservée, projetée)
     *   [__df_stack_start, __df_stack_end)   S2, pile de la tâche #DF
     *                                        (df_stack_area, entry.asm)
     *   [__stack_guard_start, __stack_guard_end) garde basse de S1, NON
     *                                        présente depuis K0.6 ; garde aussi
     *                                        le sommet de S2
     *   [boot_stack_bottom, boot_stack_top)  S1, 16 KiB (entry.asm)
     *   [__stack_top_guard_start, __stack_top_guard_end) garde haute de S1
     *                                        (R1f-2 : non présente ; R1f-1 :
     *                                        réservée, projetée)
     * Le premier cadre géré par le PMM (répertoire de pages) suit
     * __kernel_end = __stack_top_guard_end.
     * NOLOAD : aucun octet dans le fichier ; ni .bss (remise à zéro par
     * _start) ni le chargeur ne garantissent le contenu : S1 est peinte par
     * _start, S2 est remise à zéro par gdt_init. */
    .stacks ALIGN(PAGE_SIZE) (NOLOAD) :
    {
        __stacks_start = .;
        __df_guard_start = .;
        . += PAGE_SIZE;
        __df_guard_end = .;
        __df_stack_start = .;
        KEEP(*(.dfstack))
        __df_stack_end = .;
        __stack_guard_start = .;
        . += PAGE_SIZE;
        __stack_guard_end = .;
        KEEP(*(.bootstack))
        __stack_top_guard_start = .;
        . += PAGE_SIZE;
        __stack_top_guard_end = .;
        __stacks_end = .;
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
ASSERT(boot_stack_bottom == __stack_guard_end, "la pile doit suivre immediatement la zone de garde")
ASSERT((boot_stack_top % 16) == 0, "sommet de pile non aligne sur 16")
/* K0.7-R1f-1 — Section .stacks : tailles, alignements, contiguïté. Chaque
 * égalité de bornes interdit à la fois un trou et un recouvrement. */
ASSERT(__stacks_start >= __bss_end, ".stacks recouvre .bss")
ASSERT((__stacks_start % PAGE_SIZE) == 0, ".stacks non alignee sur 4 KiB")
ASSERT(__df_guard_start == __stacks_start, "garde S2 : debut de .stacks")
ASSERT(__df_guard_end - __df_guard_start == PAGE_SIZE, "garde S2 : 4 KiB")
ASSERT(__df_stack_start == __df_guard_end, "S2 suit sa garde")
ASSERT(df_stack_area == __df_stack_start, "df_stack_area = debut de S2")
ASSERT(__df_stack_end - __df_stack_start == 4096, "S2 : 4096 octets")
ASSERT((__df_stack_start % PAGE_SIZE) == 0, "S2 alignee sur 4 KiB")
ASSERT(__stack_guard_start == __df_stack_end, "garde basse S1 : juste au-dessus de S2")
ASSERT(__stack_guard_end - __stack_guard_start == PAGE_SIZE, "garde basse S1 : 4 KiB")
ASSERT(boot_stack_bottom == __stack_guard_end, "S1 suit sa garde basse")
ASSERT(boot_stack_top - boot_stack_bottom == 16384, "S1 : 16 KiB")
ASSERT((boot_stack_bottom % PAGE_SIZE) == 0, "S1 alignee sur 4 KiB")
ASSERT(__stack_top_guard_start == boot_stack_top, "garde haute S1 : au sommet de S1")
ASSERT(__stack_top_guard_end - __stack_top_guard_start == PAGE_SIZE, "garde haute S1 : 4 KiB")
ASSERT(__stacks_end == __stack_top_guard_end, "fin de .stacks")
ASSERT(__kernel_end == __stacks_end, ".stacks termine l'image")
/* K0.7-P4b — Pages des structures critiques : chacune commence au début de
 * .bss ou à la fin de la précédente, est alignée et fait EXACTEMENT une page
 * (un contenu plus grand qu'une page ferait échouer l'assertion de taille). */
ASSERT(__kcpu_start == __bss_start, "P4b : GDT/TSS en tete de .bss")
ASSERT((__kcpu_start % PAGE_SIZE) == 0, "P4b : page GDT/TSS alignee")
ASSERT(__kcpu_end - __kcpu_start == PAGE_SIZE, "P4b : GDT/TSS = une page")
ASSERT(__kro_idt_start == __kcpu_end, "P4b : page IDT contigue")
ASSERT(__kro_idt_end - __kro_idt_start == PAGE_SIZE, "P4b : IDT = une page")
ASSERT(__kro_handlers_start == __kro_idt_end, "P4b : page gestionnaires contigue")
ASSERT(__kro_handlers_end - __kro_handlers_start == PAGE_SIZE, "P4b : gestionnaires = une page")
ASSERT(__kro_vmm_start == __kro_handlers_end, "P4b : page racines VMM contigue")
ASSERT(__kro_vmm_end - __kro_vmm_start == PAGE_SIZE, "P4b : racines VMM = une page")
ASSERT(__kro_vmm_end <= __bss_end, "P4b : pages critiques dans .bss")
/* K0.6 identifiera l'image du noyau dans la carte mémoire ; on borne sa
 * taille pour garder des hypothèses simples (révisable). */
ASSERT(__kernel_end <= 0x00400000, "le noyau depasse 4 MiB : reviser la disposition")
