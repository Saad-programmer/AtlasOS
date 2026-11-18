/*
 * SAMOS / AtlasOS — kernel/core/stack_protector.c
 * ---------------------------------------------------------------------------
 * K0.7-R1b — Réaction à la corruption d'un canari de pile.
 *
 * Quand est-on appelé ? À l'épilogue d'une fonction instrumentée, après
 * « cmpl -4(%ebp), %eax ; jne » : la copie locale du canari ne correspond plus
 * à __stack_chk_guard. Quelque chose a écrit dans le cadre, entre les
 * variables locales et l'adresse de retour.
 *
 * Pourquoi est-ce sûr de continuer sur la même pile ? L'épilogue n'a encore
 * rien dépilé : ESP est valide, et la fonction fautive n'a pas exécuté son
 * « ret » (l'adresse de retour, peut-être écrasée, n'est jamais utilisée).
 * __stack_chk_fail ne retourne jamais.
 *
 * Absence de récursion : cette fonction et panic_at() sont exclues de
 * l'instrumentation (SAMOS_NO_STACK_PROTECTOR). Si une fonction appelée par
 * panic (kprintf...) détectait à son tour une corruption, on reviendrait dans
 * panic_at, qui le reconnaît (g_in_panic) et s'arrête après un message
 * minimal : la chaîne est bornée à un niveau.
 *
 * Diagnostic : __builtin_return_address(0) est l'adresse qui suit le
 * « call __stack_chk_fail » DANS la fonction fautive (addr2line la nomme).
 */
#include <arch/cpu.h>
#include <samos/panic.h>
#include <samos/stack_protector.h>

SAMOS_NO_STACK_PROTECTOR
void __stack_chk_fail(void)
{
    cpu_cli();
    panic("stack smashing detected: canary corrupted in the function containing %p",
          __builtin_return_address(0));
}
