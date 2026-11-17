/*
 * SAMOS / AtlasOS — kernel/arch/i386/include/arch/cpu.h
 * Primitives processeur i386 : lecture des registres de contrôle et d'état,
 * contrôle des interruptions, arrêt du CPU.
 * Référence : Intel SDM vol. 3A, §2.5 (registres de contrôle), §2.3 (EFLAGS).
 */
#ifndef SAMOS_ARCH_CPU_H
#define SAMOS_ARCH_CPU_H

#include <stdint.h>
#include <samos/compiler.h>

/* Bits d'EFLAGS (SDM vol. 1, §3.4.3) */
#define EFLAGS_CF   (1u << 0)
#define EFLAGS_RSV1 (1u << 1)   /* réservé, toujours lu à 1 */
#define EFLAGS_IF   (1u << 9)   /* Interrupt enable Flag */
#define EFLAGS_DF   (1u << 10)  /* Direction Flag */

/* Bits de CR0 (SDM vol. 3A, §2.5) */
#define CR0_PE      (1u << 0)   /* Protection Enable : mode protégé */
#define CR0_WP      (1u << 16)  /* Write Protect : respecter R/O même en ring 0 */
#define CR0_PG      (1u << 31)  /* Paging */

static inline uint32_t cpu_read_eflags(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushfl\n\tpopl %0" : "=r"(flags) : : "memory");
    return flags;
}

static inline uint32_t cpu_read_cr0(void)
{
    uint32_t v;
    __asm__ __volatile__("movl %%cr0, %0" : "=r"(v));
    return v;
}

static inline void cpu_write_cr0(uint32_t v)
{
    __asm__ __volatile__("movl %0, %%cr0" : : "r"(v) : "memory");
}

static inline uint32_t cpu_read_cr2(void)
{
    uint32_t v;
    __asm__ __volatile__("movl %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint32_t cpu_read_cr3(void)
{
    uint32_t v;
    __asm__ __volatile__("movl %%cr3, %0" : "=r"(v));
    return v;
}

static inline void cpu_write_cr3(uint32_t v)
{
    __asm__ __volatile__("movl %0, %%cr3" : : "r"(v) : "memory");
}

static inline uint32_t cpu_read_esp(void)
{
    uint32_t v;
    __asm__ __volatile__("movl %%esp, %0" : "=r"(v));
    return v;
}

/* Registres de table de descripteurs (SGDT/SIDT, SDM vol. 3A §2.4) :
 * limite 16 bits + base linéaire 32 bits, sans remplissage. */
struct cpu_dtr {
    uint16_t limit;
    uint32_t base;
} SAMOS_PACKED;

static inline void cpu_sgdt(struct cpu_dtr *d) { __asm__ __volatile__("sgdt %0" : "=m"(*d)); }
static inline void cpu_sidt(struct cpu_dtr *d) { __asm__ __volatile__("sidt %0" : "=m"(*d)); }

static inline uint16_t cpu_str(void)
{
    uint16_t tr;
    __asm__ __volatile__("str %0" : "=r"(tr));
    return tr;
}

#define CPU_READ_SEG(name)                                                  \
    static inline uint16_t cpu_read_##name(void)                            \
    {                                                                       \
        uint16_t v;                                                         \
        __asm__ __volatile__("mov %%" #name ", %0" : "=r"(v));              \
        return v;                                                           \
    }
CPU_READ_SEG(cs)
CPU_READ_SEG(ds)
CPU_READ_SEG(es)
CPU_READ_SEG(fs)
CPU_READ_SEG(gs)
CPU_READ_SEG(ss)
#undef CPU_READ_SEG

static inline void cpu_cli(void) { __asm__ __volatile__("cli" ::: "memory"); }
static inline void cpu_sti(void) { __asm__ __volatile__("sti" ::: "memory"); }

/* Invalide l'entrée TLB correspondant à une adresse virtuelle (SDM vol. 2, INVLPG). */
static inline void cpu_invlpg(const void *vaddr)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/*
 * Arrêt définitif et sûr : masque les interruptions puis exécute HLT en
 * boucle. La boucle est nécessaire car un NMI (non masquable par CLI) ou un
 * SMI peut réveiller le processeur après HLT.
 */
static inline SAMOS_NORETURN void cpu_halt_forever(void)
{
    for (;;) {
        __asm__ __volatile__("cli\n\thlt" ::: "memory");
    }
}

#endif /* SAMOS_ARCH_CPU_H */
