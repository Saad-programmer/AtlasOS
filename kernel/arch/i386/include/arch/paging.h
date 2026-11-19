/*
 * SAMOS / AtlasOS — kernel/arch/i386/include/arch/paging.h
 * ---------------------------------------------------------------------------
 * Pagination 32 bits à deux niveaux, pages de 4 KiB (K0.6).
 * Référence : Intel SDM vol. 3A, §4.3 (« 32-Bit Paging »).
 *
 *   adresse linéaire : | répertoire (10) | table (10) | décalage (12) |
 *   CR3 -> répertoire de pages (1024 PDE) -> table de pages (1024 PTE) -> cadre
 *
 * Disposition de l'espace d'adressage du noyau en K0 :
 *   [0, 4 KiB)                 NON PROJETÉE : déréférencement de NULL => #PF
 *   [4 KiB, 1 MiB)             RW  (BIOS, VGA 0xB8000, ...)
 *   .text, .rodata             R   (CR0.WP = 1 : vrai même en ring 0)
 *   .data, .bss                RW
 *   page de garde de pile      NON PROJETÉE : débordement => #PF puis #DF
 *   pile de démarrage          RW
 *   reste de la RAM gérée      RW  (identité, pour accéder aux cadres)
 *   au-delà                    non projeté
 * i386 sans PAE ne possède PAS de bit « non exécutable » : toute page
 * présente est exécutable (limite documentée, Details.md §35).
 */
#ifndef SAMOS_ARCH_PAGING_H
#define SAMOS_ARCH_PAGING_H

#include <stdbool.h>
#include <stdint.h>
#include <samos/errno.h>

#define PTE_PRESENT  0x001u
#define PTE_WRITE    0x002u
#define PTE_USER     0x004u
#define PTE_ACCESSED 0x020u
#define PTE_DIRTY    0x040u
#define PTE_FRAME    0xFFFFF000u

/* Borne de la zone projetée en identité (et donc de la mémoire gérée). */
#define PAGING_IDMAP_LIMIT 0x40000000u   /* 1 GiB */

/* Construit les tables, active la pagination (CR3, CR0.PG | CR0.WP),
 * installe le gestionnaire de #PF. `idmap_top` : fin (exclue) de la RAM à
 * projeter, arrondie à 4 KiB. */
samos_err_t paging_init(uint32_t idmap_top);

/* Projette une page. Refuse d'écraser une projection existante (EEXIST).
 * K0.7-M2.5 : seul un cadre ALLOCATED est accepté (sinon EPERM) ; `flags`
 * vaut 0 (lecture seule) ou PTE_WRITE (sinon EINVAL). PTE_PRESENT est posé
 * par le VMM ; la projection est toujours superviseur. */
samos_err_t vmm_map(uint32_t va, uint32_t pa, uint32_t flags);
/* Supprime une projection (INVLPG) ; *pa reçoit l'ancien cadre. */
samos_err_t vmm_unmap(uint32_t va, uint32_t *pa);
/* Parcours logiciel des tables : true si va est projetée. */
bool vmm_translate(uint32_t va, uint32_t *pa, uint32_t *flags);

uint32_t paging_directory(void);       /* adresse physique du répertoire (CR3) */
uint32_t paging_table_count(void);     /* tables de pages allouées */

/* K0.7-P4d — Retire PTE_WRITE de la projection d'identité de [start, end)
 * (pages de l'image, FRAME_KERNEL), sans retour possible. */
samos_err_t paging_seal_identity(uint32_t start, uint32_t end);
/* Scelle kro_idt, kro_handlers et kro_vmm (après la dernière écriture). */
samos_err_t paging_seal_critical(void);

/* K0.7-P4e-1 — Compteurs du passage d'écriture des PDE/PTE (fenêtres WP
 * ouvertes sous pagination ; refus). */
void paging_gate_stats(uint32_t *windows, uint32_t *refusals);

/* K0.7-P4e-2 — Audit des structures de pagination (voir paging.c). Vrai si
 * aucune anomalie : répertoire et tables sans W dans l'identité, aucun autre
 * alias, PDE conformes, tout cadre PAGETABLE publié. */
struct paging_audit {
    uint32_t tables;        /* PDE présents */
    uint32_t writable;      /* cadres de pagination dont l'identité porte W */
    uint32_t aliases;       /* projections d'un cadre de pagination hors identité */
    uint32_t bad_pdes;      /* PDE non conformes */
    uint32_t unpublished;   /* cadres PAGETABLE non publiés */
    /* K0.7-P4f-B1 — sans le PMM (répertoire actif et image du noyau) : */
    uint32_t foreign_aliases;   /* alias hors identité d'un cadre de pagination publié ou de l'image */
    uint32_t struct_writable;   /* cadre de pagination publié dont l'identité porte W */
    uint32_t dup_pdes;          /* K0.7-P4f-B2 : PDE vers le répertoire ou vers une table déjà désignée */
    /* K0.7-P4f-D (N5) : */
    uint32_t forbidden_tables;      /* répertoire ou table dans l'image ou la mémoire basse (sans le PMM) */
    uint32_t image_type_mismatch;   /* cadres de l'image dont le type PMM n'est pas KERNEL */
    uint32_t lowmem_allocatable;    /* cadres < 1 MiB de type FREE, ALLOCATED ou PAGETABLE */
};
bool paging_audit(struct paging_audit *a);
#if CONFIG_KTEST
/* Image de test : kind 0 = PDE, 1 = PTE. */
samos_err_t paging_gate_test_write(int kind, uint32_t va, uint32_t value);
/* K0.7-P4f-B3 : refus injecté de la publication du PDE dans table_create. */
void paging_test_inject_pde_refusal(bool on);
void paging_gate_test_set_active(bool active);
#endif

/* Sondes (probe.asm) : 0 = accès réussi, -1 = faute de page interceptée. */
int mem_probe_read(const volatile void *addr, uint32_t *out);
int mem_probe_write(volatile void *addr, uint32_t value);
/* Dernière faute interceptée par une sonde (adresse CR2 et code d'erreur). */
void mem_probe_last_fault(uint32_t *cr2, uint32_t *err);

/* Vrai si cette #PF provient d'une instruction de sonde (probe.asm). */
struct interrupt_frame;
bool paging_is_probe_fault(const struct interrupt_frame *f);

/* Diagnostic textuel d'une adresse fautive (utilisé par #PF et #DF). */
const char *paging_classify(uint32_t addr, uint32_t err);

#endif /* SAMOS_ARCH_PAGING_H */
