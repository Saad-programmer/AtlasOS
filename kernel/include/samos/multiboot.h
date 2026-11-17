/*
 * SAMOS / AtlasOS — kernel/include/samos/multiboot.h
 * ---------------------------------------------------------------------------
 * Structures de la spécification Multiboot 0.6.96 (section 3.3, « Boot
 * information format »), écrites indépendamment à partir du texte de la
 * spécification.
 *
 * IMPORTANT (sécurité) : toutes les données de ces structures proviennent du
 * chargeur et sont traitées comme des entrées NON FIABLES. Chaque champ n'est
 * valide que si le bit correspondant de `flags` est positionné, et tout
 * pointeur/longueur doit être borné avant usage.
 */
#ifndef SAMOS_MULTIBOOT_H
#define SAMOS_MULTIBOOT_H

#include <stdint.h>
#include <samos/compiler.h>

/* Valeur placée dans EAX par un chargeur conforme (spéc. §3.2). */
#define MULTIBOOT_BOOTLOADER_MAGIC  0x2BADB002u

/* Bits de multiboot_info.flags (spéc. §3.3) */
#define MBI_FLAG_MEM        (1u << 0)   /* mem_lower / mem_upper valides */
#define MBI_FLAG_BOOTDEV    (1u << 1)   /* boot_device valide */
#define MBI_FLAG_CMDLINE    (1u << 2)   /* cmdline valide */
#define MBI_FLAG_MODS       (1u << 3)   /* mods_count / mods_addr valides */
#define MBI_FLAG_AOUT_SYMS  (1u << 4)
#define MBI_FLAG_ELF_SHDR   (1u << 5)
#define MBI_FLAG_MMAP       (1u << 6)   /* mmap_length / mmap_addr valides */
#define MBI_FLAG_DRIVES     (1u << 7)
#define MBI_FLAG_CONFIG     (1u << 8)
#define MBI_FLAG_LOADER     (1u << 9)   /* boot_loader_name valide */
#define MBI_FLAG_APM        (1u << 10)
#define MBI_FLAG_VBE        (1u << 11)
#define MBI_FLAG_FB         (1u << 12)

/* Structure d'information transmise par le chargeur (offsets de la spéc.). */
struct multiboot_info {
    uint32_t flags;             /*  0 */
    uint32_t mem_lower;         /*  4 : KiB de mémoire basse (à partir de 0) */
    uint32_t mem_upper;         /*  8 : KiB de mémoire haute (à partir de 1 MiB) */
    uint32_t boot_device;       /* 12 */
    uint32_t cmdline;           /* 16 : adresse physique d'une chaîne C */
    uint32_t mods_count;        /* 20 */
    uint32_t mods_addr;         /* 24 */
    uint32_t syms[4];           /* 28 : a.out ou ELF (union dans la spéc.) */
    uint32_t mmap_length;       /* 44 : longueur totale du tampon mmap en octets */
    uint32_t mmap_addr;         /* 48 : adresse physique du premier élément */
    uint32_t drives_length;     /* 52 */
    uint32_t drives_addr;       /* 56 */
    uint32_t config_table;      /* 60 */
    uint32_t boot_loader_name;  /* 64 */
    uint32_t apm_table;         /* 68 */
    uint32_t vbe_control_info;  /* 72 */
    uint32_t vbe_mode_info;     /* 76 */
    uint16_t vbe_mode;          /* 80 */
    uint16_t vbe_interface_seg; /* 82 */
    uint16_t vbe_interface_off; /* 84 */
    uint16_t vbe_interface_len; /* 86 */
    /* Champs framebuffer (88..) non utilisés en K0. */
} SAMOS_PACKED;

SAMOS_STATIC_ASSERT(__builtin_offsetof(struct multiboot_info, cmdline) == 16, "offset cmdline");
SAMOS_STATIC_ASSERT(__builtin_offsetof(struct multiboot_info, mmap_length) == 44, "offset mmap_length");
SAMOS_STATIC_ASSERT(__builtin_offsetof(struct multiboot_info, boot_loader_name) == 64, "offset loader");
SAMOS_STATIC_ASSERT(sizeof(struct multiboot_info) == 88, "taille multiboot_info");

/*
 * Élément de la carte mémoire (spéc. §3.3, champ mmap_*).
 * ATTENTION : le champ `size` donne la taille de l'élément SANS compter le
 * champ size lui-même ; l'élément suivant se trouve à (adresse + size + 4).
 * Un chargeur peut fournir des éléments plus grands que 20 octets.
 */
struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
} SAMOS_PACKED;

SAMOS_STATIC_ASSERT(sizeof(struct multiboot_mmap_entry) == 24, "taille mmap_entry");

/* Types de région (spéc. §3.3 ; valeurs 3-5 issues de l'ACPI / E820). */
#define MULTIBOOT_MEMORY_AVAILABLE        1
#define MULTIBOOT_MEMORY_RESERVED         2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MEMORY_NVS              4
#define MULTIBOOT_MEMORY_BADRAM           5

#endif /* SAMOS_MULTIBOOT_H */
