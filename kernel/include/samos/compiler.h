/*
 * SAMOS / AtlasOS — kernel/include/samos/compiler.h
 * Macros d'attributs du compilateur, centralisées pour pouvoir changer de
 * compilateur (Clang/GCC) sans modifier tout le code.
 */
#ifndef SAMOS_COMPILER_H
#define SAMOS_COMPILER_H

/* La fonction ne retourne jamais (C11 _Noreturn). */
#define SAMOS_NORETURN      _Noreturn
/* Structure sans octets de remplissage : disposition binaire exacte imposée
 * par une spécification matérielle ou un protocole (Multiboot, GDT, IDT...). */
#define SAMOS_PACKED        __attribute__((packed))
#define SAMOS_ALIGNED(n)    __attribute__((aligned(n)))
#define SAMOS_UNUSED        __attribute__((unused))
/* Vérification de format printf à la compilation. */
#define SAMOS_PRINTF(f, a)  __attribute__((format(printf, f, a)))
/* Jamais intégrée à l'appelant (le cadre de pile propre est requis). */
#define SAMOS_NOINLINE      __attribute__((noinline))
/* K0.7-R1 — Fonction exclue de l'instrumentation -fstack-protector-* : chemin
 * de réaction à une corruption (__stack_chk_fail, panic), qui doit rester
 * minimal et ne jamais pouvoir se déclencher lui-même. */
#define SAMOS_NO_STACK_PROTECTOR __attribute__((no_stack_protector))

#define SAMOS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define SAMOS_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Barrière de compilation : empêche le compilateur de réordonner les accès
 * mémoire autour de ce point (ne génère aucune instruction). */
#define SAMOS_BARRIER()     __asm__ __volatile__("" ::: "memory")

#endif /* SAMOS_COMPILER_H */
