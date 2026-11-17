/*
 * SAMOS / AtlasOS — kernel/include/samos/bootinfo.h
 * ---------------------------------------------------------------------------
 * Validation et copie des informations transmises par le chargeur Multiboot.
 *
 * Principe de sécurité : la structure multiboot_info et les chaînes qu'elle
 * désigne sont des entrées externes. bootinfo_init() :
 *   - vérifie le magic,
 *   - vérifie que les adresses ne débordent pas de l'espace 32 bits et ne
 *     chevauchent pas l'image du noyau,
 *   - copie les chaînes (cmdline, nom du chargeur) dans des tampons du noyau
 *     de taille fixe, avec une lecture bornée.
 * Le reste du noyau n'utilise ensuite que ces copies validées.
 */
#ifndef SAMOS_BOOTINFO_H
#define SAMOS_BOOTINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <samos/errno.h>
#include <samos/multiboot.h>

#define BOOTINFO_CMDLINE_MAX 256
#define BOOTINFO_LOADER_MAX  64

struct bootinfo {
    uint32_t magic;
    uint32_t mbi_phys;
    uint32_t flags;                       /* copie de multiboot_info.flags */
    uint32_t mem_lower_kib;               /* valides si flags & MBI_FLAG_MEM */
    uint32_t mem_upper_kib;
    uint32_t mmap_addr;                   /* valides si flags & MBI_FLAG_MMAP */
    uint32_t mmap_length;
    char cmdline[BOOTINFO_CMDLINE_MAX];   /* "" si absente */
    char loader_name[BOOTINFO_LOADER_MAX];
    bool cmdline_truncated;
};

/* Codes de retour spécifiques à la validation. */
samos_err_t bootinfo_init(uint32_t magic, uint32_t mbi_phys);
const struct bootinfo *bootinfo_get(void);

/*
 * Vérifie qu'une région physique [addr, addr+len) est « plausible » :
 * non nulle, sans débordement 32 bits, et hors de l'image du noyau.
 * Exportée car réutilisée par la carte mémoire (K0.6) et les tests.
 */
bool bootinfo_region_ok(uint32_t addr, uint32_t len);

/*
 * Ligne de commande : jetons séparés par des espaces.
 * cmdline_has("test")         -> vrai si le jeton "test" ou "test=..." existe
 * cmdline_value("test", b, n) -> copie la valeur après "test=" dans b
 */
bool cmdline_has(const char *key);
bool cmdline_value(const char *key, char *out, size_t cap);

#endif /* SAMOS_BOOTINFO_H */
