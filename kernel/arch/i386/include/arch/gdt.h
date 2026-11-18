/*
 * SAMOS / AtlasOS — kernel/arch/i386/include/arch/gdt.h
 * ---------------------------------------------------------------------------
 * Table globale des descripteurs (GDT) et segments d'état de tâche (TSS),
 * jalon K0.4. Référence : Intel SDM vol. 3A, §3.4.5 (descripteurs de
 * segment), §7.2 (TSS), §7.2.2 (descripteur de TSS).
 *
 * Modèle « plat » (flat) : les segments de code et de données couvrent tout
 * l'espace 0..4 GiB avec une base 0 ; la protection mémoire réelle sera
 * assurée par la pagination (K0.6), pas par la segmentation.
 *
 * Entrées :
 *   0  0x00  descripteur nul (obligatoire, SDM §3.4.2)
 *   1  0x08  code noyau  : base 0, limite 4 GiB, DPL 0, exécution/lecture
 *   2  0x10  données noyau : base 0, limite 4 GiB, DPL 0, lecture/écriture
 *   3  0x18  TSS principal (TR) : reçoit l'état sauvegardé lors d'une
 *            commutation de tâche matérielle
 *   4  0x20  TSS de double faute : tâche dédiée avec sa propre pile
 * Les segments utilisateur (DPL 3) ne sont PAS créés en K0 : aucun code
 * ring 3 n'existe, et un descripteur inutilisé est une surface d'attaque.
 */
#ifndef SAMOS_ARCH_GDT_H
#define SAMOS_ARCH_GDT_H

#include <stdint.h>
#include <samos/compiler.h>

#define GDT_KERNEL_CODE_SEL 0x08
#define GDT_KERNEL_DATA_SEL 0x10
#define GDT_TSS_MAIN_SEL    0x18
#define GDT_TSS_DF_SEL      0x20
#define GDT_ENTRIES         5

/* Descripteur de segment de 8 octets (SDM vol. 3A, figure 3-8). */
struct gdt_entry {
    uint16_t limit_low;     /* limite bits 0..15 */
    uint16_t base_low;      /* base bits 0..15 */
    uint8_t  base_mid;      /* base bits 16..23 */
    uint8_t  access;        /* P | DPL(2) | S | Type(4) */
    uint8_t  limit_flags;   /* G | D/B | L | AVL | limite bits 16..19 */
    uint8_t  base_high;     /* base bits 24..31 */
} SAMOS_PACKED;
SAMOS_STATIC_ASSERT(sizeof(struct gdt_entry) == 8, "descripteur GDT = 8 octets");

/* Segment d'état de tâche 32 bits (SDM vol. 3A, figure 7-2), 104 octets. */
struct tss {
    uint16_t prev_task, rsv0;
    uint32_t esp0;
    uint16_t ss0, rsv1;
    uint32_t esp1;
    uint16_t ss1, rsv2;
    uint32_t esp2;
    uint16_t ss2, rsv3;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint16_t es, rsv4, cs, rsv5, ss, rsv6, ds, rsv7, fs, rsv8, gs, rsv9;
    uint16_t ldt, rsv10;
    uint16_t trap, iomap_base;
} SAMOS_PACKED;
SAMOS_STATIC_ASSERT(sizeof(struct tss) == 104, "TSS 32 bits = 104 octets");

void gdt_init(void);

/* Accès pour diagnostic/tests. */
const struct gdt_entry *gdt_table(void);
struct tss *tss_main(void);
struct tss *tss_double_fault(void);

/* Met à jour le CR3 utilisé par la tâche de double faute (K0.6 : appelé après
 * l'activation de la pagination, pour que la tâche voie les mêmes mappings). */
void gdt_set_double_fault_cr3(uint32_t cr3);

/*
 * K0.7-R1d — Sentinelles de la pile de double faute (S2).
 * S2 = df_stack_area[4096] (entry.asm, section .stacks de linker.ld, alignée
 * sur 4 KiB depuis R1f-1), utilisée UNIQUEMENT par la tâche matérielle #DF ;
 * elle croît vers le bas depuis df_stack_area + 4096. Les DF_SENTINEL_WORDS
 * premiers mots (les 16 octets les plus bas, juste au-dessus de la page
 * réservée à sa garde) contiennent des valeurs connues : si l'un d'eux
 * change, S2 a débordé (ou une écriture parasite l'a atteinte).
 * Détection seulement, au moment de la vérification : pas d'empêchement,
 * pas de remplacement d'une page de garde.
 */
#define DF_SENTINEL_WORDS 4u
/* Valeur attendue du mot i : constante du code, jamais relue dans S2. */
#define DF_SENTINEL_VALUE(i) (0xDF5E0000u | (0x1100u * ((i) + 1u)) | 0x00A5u)

/* Masque des mots altérés (bit i = mot i), 0 si intactes. Aucune écriture,
 * aucun appel, aucun pointeur lu dans S2 : sûre même si S2 est corrompue. */
uint32_t df_stack_sentinel_damage(void);
/* Adresse du premier mot sentinelle (= bas de S2), pour diagnostic et tests. */
volatile uint32_t *df_stack_sentinels(void);
/* Bornes de S2 [bas, sommet). */
uintptr_t df_stack_bottom(void);
uintptr_t df_stack_top(void);

/* Fonctions assembleur (gdt_flush.asm). */
void gdt_load(const void *gdtr);   /* LGDT + rechargement de CS..SS */
void tss_load(uint16_t selector);  /* LTR */

#endif /* SAMOS_ARCH_GDT_H */
