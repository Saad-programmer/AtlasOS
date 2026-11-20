/*
 * SAMOS / AtlasOS — kernel/arch/x86_64/skeleton.c          (K1.1-M64-1)
 *
 * SQUELETTE DE CONSTRUCTION, NON AMORÇABLE : aucune de ces définitions n'est
 * exécutée (voir kernel/arch/x86_64/boot/entry.asm).
 *
 * Rôle : faire passer au moins un fichier C par la chaîne de compilation
 * x86-64 (drapeaux X64_CFLAGS de mk/x86_64.mk) et peupler chacune des
 * classes de permissions que l'éditeur de liens doit séparer :
 *   - .text   : samos64_skeleton_probe (code 64 bits généré par Clang) ;
 *   - .rodata : samos64_build_id (constante) ;
 *   - .data   : samos64_data_marker (variable initialisée non nulle) ;
 *   - .bss    : samos64_bss_area (variable non initialisée).
 * tools/check_elf64.sh vérifie ensuite que ces sections commencent chacune
 * sur une frontière de 4 KiB et que les segments R-X / R-- / RW- ne partagent
 * aucune page (règles R4 et R5 de la conception, §5.4.5).
 *
 * Aucun en-tête du noyau i386 n'est inclus : seul <stdint.h>, fourni par le
 * compilateur en mode freestanding.
 */
#include <stdint.h>

/* Identification lisible dans l'image (readelf -p .rodata). */
const char samos64_build_id[] = "SAMOS x86-64 build skeleton (K1.1-M64-1, not bootable)";

/* Valeur non nulle : placée dans .data, pas dans .bss. */
uint32_t samos64_data_marker = 0x5A4D3634u;  /* « ZM64 » */

/* Non initialisée : placée dans .bss (NOBITS). */
uint8_t samos64_bss_area[64];

uint64_t samos64_skeleton_probe(uint64_t x);

/*
 * Fonction jamais appelée. Elle lit les trois objets pour qu'ils soient
 * référencés par du code, et produit du code 64 bits ordinaire (registres
 * généraux uniquement : -mgeneral-regs-only), inspecté par check_elf64.sh
 * (aucune instruction SSE, x87, de registre de contrôle ou de MSR).
 */
uint64_t samos64_skeleton_probe(uint64_t x)
{
    return x + samos64_data_marker + samos64_bss_area[0] + (uint8_t)samos64_build_id[0];
}
