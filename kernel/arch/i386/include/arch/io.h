/*
 * SAMOS / AtlasOS — kernel/arch/i386/include/arch/io.h
 * ---------------------------------------------------------------------------
 * Accès à l'espace d'entrées/sorties x86 (« port I/O »).
 *
 * L'architecture x86 possède, en plus de l'espace mémoire, un espace d'adresses
 * d'E/S séparé de 65536 ports de 8 bits, accessible uniquement par les
 * instructions IN/OUT (Intel SDM vol. 1, chap. 19 « Input/Output »).
 * En mode protégé, ces instructions sont privilégiées : elles ne sont
 * autorisées que si CPL <= IOPL (ou via la bitmap d'E/S du TSS). Le noyau
 * s'exécute en CPL 0, il y a donc toujours accès.
 *
 * Contraintes d'opérandes (asm inline GCC/Clang) :
 *   "a"  : la valeur doit être dans AL/AX/EAX (seul registre accepté par IN/OUT)
 *   "Nd" : le port est soit une constante 8 bits (immédiat), soit dans DX
 * `volatile` empêche le compilateur de supprimer ou de déplacer l'accès ;
 * le clobber "memory" ordonne l'E/S par rapport aux accès mémoire voisins.
 */
#ifndef SAMOS_ARCH_IO_H
#define SAMOS_ARCH_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ __volatile__("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ __volatile__("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

/*
 * Petite temporisation d'E/S : une écriture sur le port 0x80 (port de code
 * POST, inutilisé après le démarrage) prend environ 1 µs sur le bus ISA.
 * Utilisée entre deux accès à d'anciens contrôleurs lents (PIC 8259).
 */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif /* SAMOS_ARCH_IO_H */
