/*
 * SAMOS / AtlasOS — kernel/tests/test_faults.c
 * ---------------------------------------------------------------------------
 * Fautes FATALES volontaires (K0.4). Chaque mode est exécuté dans un
 * démarrage QEMU séparé (tests/qemu/cases.sh) car il ne revient jamais :
 *
 *   ud      ud2                       -> #UD non géré -> panic
 *   de      div par zéro              -> #DE non géré -> panic
 *   gp      MOV DS, 0x0F00            -> #GP(0x0F00) : index 480 hors GDT
 *   np      INT 0x2E                  -> #NP(0x172) : porte 46 (IRQ 14, inutilisée)
 *                                        de type valide, P = 0
 *   intbad  INT 0x80                  -> #GP(0x402) : entrée nulle = type invalide ;
 *                                        le type est vérifié AVANT le bit P
 *   df      #NP rendu non présent,    -> #NP pendant la livraison de #NP
 *           puis INT 0x20                => DOUBLE FAULT (porte de tâche)
 * (En K0.4 ces tests utilisaient INT 0x20 ; depuis K0.5 ce vecteur est l'IRQ 0
 *  du PIT : un INT 0x20 logiciel exécuterait le gestionnaire du timer et
 *  enverrait un EOI parasite au PIC. Voir Details.md §28.6.)
 * K0.6 (pagination active) :
 *   null     lecture de *(uint32_t *)0      -> #PF, « NULL pointer dereference »
 *   wtext    écriture dans .text            -> #PF(P|W), code en lecture seule
 *   wrodata  écriture dans .rodata          -> #PF(P|W)
 *   unmapped lecture à 0xE0000000           -> #PF, hors de la RAM projetée
 *   stack    récursion infinie              -> la pile atteint la page de
 *            garde : #PF, dont la livraison exige d'empiler sur cette même
 *            page => 2e #PF => #DF, attrapée par la porte de tâche sur sa pile
 *            dédiée (K0.4). Démonstration de bout en bout K0.2 + K0.4 + K0.6.
 * K0.7 :
 *   nested   un gestionnaire de #BP exécute ud2 => #UD PENDANT le traitement
 *            de #BP : la garde d'imbrication doit arrêter proprement le noyau
 *   triple  IDT de limite 0, INT3     -> aucune porte livrable : TRIPLE FAULT
 *                                        => réinitialisation du processeur
 *                                        (prouve que le banc de test la détecte)
 */
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/interrupt.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/panic.h>
#include <samos/kernel.h>
#include <samos/string.h>

static const uint32_t g_rodata_word = 0x12345678u;

/* Récursion (pratiquement) sans fin consommant ~256 octets de pile par niveau. noinline et
 * l'utilisation de buf APRÈS l'appel empêchent l'optimisation en boucle. */
/* Borne volatile jamais atteinte en pratique (la pile de 16 KiB s'épuise vers
 * la profondeur 60) : elle rend la récursion formellement finie, ce qui évite
 * l'avertissement -Winfinite-recursion sans le désactiver. */
static volatile uint32_t g_recursion_limit = UINT32_MAX;

static __attribute__((noinline)) uint32_t recurse_forever(uint32_t depth)
{
    volatile uint8_t buf[256];
    if (depth == g_recursion_limit) {
        return 0;
    }
    buf[0] = (uint8_t)depth;
    uint32_t r = recurse_forever(depth + 1);
    return r + buf[0];
}

static void faulting_bp_handler(struct interrupt_frame *f)
{
    (void)f;
    __asm__ __volatile__("ud2");
}

bool ktest_fatal_mode(const char *mode)
{
    kprintf("[TEST] fatal mode '%s'\n", mode);
    if (strcmp(mode, "ud") == 0) {
        __asm__ __volatile__("ud2");
    } else if (strcmp(mode, "de") == 0) {
        __asm__ __volatile__("xorl %%edx, %%edx\n\tmovl $1, %%eax\n\txorl %%ecx, %%ecx\n\tdivl %%ecx"
                             : : : "eax", "ecx", "edx", "cc");
    } else if (strcmp(mode, "gp") == 0) {
        __asm__ __volatile__("movw $0x0F00, %%ax\n\tmovw %%ax, %%ds" : : : "eax", "memory");
    } else if (strcmp(mode, "np") == 0) {
        __asm__ __volatile__("int $0x2e");
    } else if (strcmp(mode, "intbad") == 0) {
        __asm__ __volatile__("int $0x80");
    } else if (strcmp(mode, "df") == 0) {
        /* #NP (contributive) survenant pendant la livraison d'une #NP
         * (contributive) => #DF (SDM vol. 3A, tableau 6-5). */
        idt_set_present(EXC_NP, false);
        __asm__ __volatile__("int $0x2e");
    } else if (strcmp(mode, "null") == 0) {
        (void)*(volatile uint32_t *)(uintptr_t)0;
    } else if (strcmp(mode, "wtext") == 0) {
        *(volatile uint32_t *)(uintptr_t)&kmain = 0xCCCCCCCCu;
    } else if (strcmp(mode, "wrodata") == 0) {
        *(volatile uint32_t *)(uintptr_t)&g_rodata_word = 0;
    } else if (strcmp(mode, "unmapped") == 0) {
        (void)*(volatile uint32_t *)(uintptr_t)0xE0000000u;
    } else if (strcmp(mode, "stack") == 0) {
        (void)recurse_forever(0);
    } else if (strcmp(mode, "sealidt") == 0 || strcmp(mode, "sealhandlers") == 0 ||
               strcmp(mode, "sealvmm") == 0) {
        ktest_sealed_write(mode);              /* K0.7-P4d : doit finir en panic */
    } else if (strcmp(mode, "sealpd") == 0 || strcmp(mode, "sealpt") == 0 ||
               strcmp(mode, "sealnewpt") == 0) {
        ktest_pt_sealed_write(mode);           /* K0.7-P4e-2 : doit finir en panic */
    } else if (strcmp(mode, "pdesteal") == 0) {
        ktest_pde_steal();                     /* K0.7-P4f-B2 : doit finir en panic */
    } else if (strcmp(mode, "tablealias") == 0) {
        ktest_table_alias();                   /* K0.7-P4f-B3 : doit finir en panic */
    } else if (strcmp(mode, "tcseal") == 0) {
        ktest_table_seal_refused();            /* K0.7-P4f-B3 : doit finir en panic */
    } else if (strcmp(mode, "tcpde") == 0) {
        ktest_table_pde_refused();             /* K0.7-P4f-B3 : doit finir en panic */
    } else if (strcmp(mode, "recursive_panic") == 0) {
        /* K0.7-R1h (F4) — Panique PENDANT une panique : le formatage du
         * message de la première lit une chaîne à une adresse non projetée
         * (0xE0000000, comme test=unmapped) ; la #PF qui en résulte appelle
         * panic depuis son gestionnaire. panic_at doit le reconnaître
         * (g_in_panic), émettre « RECURSIVE PANIC » et s'arrêter. */
        panic("[TEST] first panic, formatting %s", (const char *)(uintptr_t)0xE0000000u);
    } else if (strcmp(mode, "topguard") == 0) {
        ktest_top_guard_write(false);          /* K0.7-R1f-2 : doit finir en panic */
    } else if (strcmp(mode, "topguard_end") == 0) {
        ktest_top_guard_write(true);
    } else if (strcmp(mode, "dfguard") == 0) {
        ktest_df_guard_write(false);           /* K0.7-R1f-3 : doit finir en panic */
    } else if (strcmp(mode, "dfguard_end") == 0) {
        ktest_df_guard_write(true);
    } else if (strcmp(mode, "dfsentinel") == 0) {
        /* K0.7-R1d : altère UN mot sentinelle de S2 (le mot 2, bas de pile,
         * jamais utilisé : 848 octets au plus sont consommés par la tâche #DF,
         * mesuré), puis déclenche une vraie #DF par le même moyen que « df ».
         * Le TSS et la GDT ne sont pas touchés. Attendu : sentinelles
         * signalées CORRUPTED (mask 0x4) par la tâche #DF, puis panic. */
        volatile uint32_t *w = df_stack_sentinels();
        kprintf("[TEST] corrupting double-fault stack sentinel word 2 at %p\n", (void *)(df_stack_bottom() + 8u));
        w[2] ^= 0xFFFFFFFFu;
        idt_set_present(EXC_NP, false);
        __asm__ __volatile__("int $0x2e");
    } else if (strcmp(mode, "canary") == 0) {
        ktest_canary_smash();                  /* K0.7-R1b : doit finir en panic */
    } else if (strcmp(mode, "nested") == 0) {
        (void)interrupt_register(EXC_BP, faulting_bp_handler);
        __asm__ __volatile__("int3");
    } else if (strcmp(mode, "triple") == 0) {
        struct cpu_dtr empty = { 0, 0 };
        __asm__ __volatile__("lidt %0\n\tint3" : : "m"(empty) : "memory");
    } else {
        return false;
    }
    return true;   /* atteint seulement si la faute ne s'est pas produite */
}
