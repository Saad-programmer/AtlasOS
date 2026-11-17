/*
 * SAMOS / AtlasOS — kernel/tests/test_cpu.c
 * ---------------------------------------------------------------------------
 * Autotests du jalon K0.4 : GDT, TSS, IDT et exceptions RÉCUPÉRABLES.
 *
 * Chaque test enregistre temporairement un gestionnaire, déclenche
 * l'exception, puis vérifie ce que le gestionnaire a observé dans le cadre
 * d'interruption. Les observations sont mémorisées dans des variables
 * globales : le gestionnaire ne journalise pas (règle des contextes
 * d'interruption, Details.md §18.5).
 */
#include <stdint.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/interrupt.h>
#include <samos/ktest.h>

static volatile struct {
    unsigned calls;
    uint32_t vector, err, eip, eax, frame_eflags, cs;
    uint32_t handler_eflags;
    uintptr_t handler_fp;
    uint8_t insn0, insn1;
} g_obs;

static void reset_obs(void)
{
    g_obs.calls = 0;
    g_obs.vector = g_obs.err = g_obs.eip = g_obs.eax = 0xFFFFFFFFu;
}

static void record(struct interrupt_frame *f)
{
    g_obs.calls++;
    g_obs.vector = f->vector;
    g_obs.err = f->err_code;
    g_obs.eip = f->eip;
    g_obs.eax = f->eax;
    g_obs.cs = f->cs;
    g_obs.frame_eflags = f->eflags;
    g_obs.handler_eflags = cpu_read_eflags();
    g_obs.handler_fp = (uintptr_t)__builtin_frame_address(0);
    const volatile uint8_t *ip = (const volatile uint8_t *)(uintptr_t)f->eip;
    g_obs.insn0 = ip[0];
    g_obs.insn1 = ip[1];
}

/* #BP est une TRAPPE : EIP sauvegardé = instruction SUIVANT int3. Rien à corriger. */
static void bp_handler(struct interrupt_frame *f) { record(f); }

/* #UD et #DE sont des FAUTES : EIP sauvegardé = instruction fautive.
 * Pour reprendre, le gestionnaire avance EIP de la longueur de l'instruction
 * (ud2 = 0F 0B ; div ecx = F7 F1 : 2 octets chacune). */
static void skip2_handler(struct interrupt_frame *f) { record(f); f->eip += 2; }

static void test_gdt(void)
{
    struct cpu_dtr gdtr;
    cpu_sgdt(&gdtr);
    KTEST_CHECK(gdtr.base == (uint32_t)(uintptr_t)gdt_table());
    KTEST_CHECK(gdtr.limit == GDT_ENTRIES * 8 - 1);
    KTEST_CHECK(cpu_read_cs() == GDT_KERNEL_CODE_SEL);
    KTEST_CHECK(cpu_read_ds() == GDT_KERNEL_DATA_SEL && cpu_read_es() == GDT_KERNEL_DATA_SEL);
    KTEST_CHECK(cpu_read_ss() == GDT_KERNEL_DATA_SEL && cpu_read_fs() == GDT_KERNEL_DATA_SEL);
    KTEST_CHECK(cpu_str() == GDT_TSS_MAIN_SEL);
    const struct gdt_entry *g = gdt_table();
    KTEST_CHECK(g[0].access == 0 && g[0].limit_low == 0);          /* descripteur nul */
    KTEST_CHECK(g[1].access == 0x9B && g[1].limit_flags == 0xCF);   /* code : P DPL0 S X R A, G D */
    KTEST_CHECK(g[2].access == 0x93 && g[2].limit_flags == 0xCF);   /* données : P DPL0 S W A */
    KTEST_CHECK(g[3].access == 0x8B);   /* TSS principal : LTR l'a marqué occupé (type 1011) */
    KTEST_CHECK(g[4].access == 0x89);   /* TSS #DF : disponible (type 1001) */
    KTEST_CHECK(tss_double_fault()->eip == (uint32_t)(uintptr_t)&double_fault_task);
    KTEST_CHECK((tss_double_fault()->esp & 0xFu) == 0);
}

static void test_idt(void)
{
    struct cpu_dtr idtr;
    cpu_sidt(&idtr);
    const struct idt_entry *t = idt_table();
    KTEST_CHECK(idtr.base == (uint32_t)(uintptr_t)t && idtr.limit == IDT_ENTRIES * 8 - 1);
    bool exc_ok = true;
    for (unsigned v = 0; v < EXC_COUNT; v++) {
        if (v == EXC_DF) {
            continue;
        }
        if (t[v].type_attr != 0x8E || t[v].selector != GDT_KERNEL_CODE_SEL) {
            exc_ok = false;
        }
    }
    KTEST_CHECK(exc_ok);                                            /* portes d'interruption DPL0 */
    KTEST_CHECK(t[EXC_DF].type_attr == 0x85 && t[EXC_DF].selector == GDT_TSS_DF_SEL);  /* tâche */
    KTEST_CHECK((t[32].type_attr & IDT_PRESENT) == 0);              /* IRQ pas encore activées */
    KTEST_CHECK((t[255].type_attr & IDT_PRESENT) == 0);
    KTEST_CHECK(!interrupt_register(EXC_BP, 0));                    /* gestionnaire nul refusé */
}

static void test_breakpoint(void)
{
    reset_obs();
    KTEST_CHECK(interrupt_register(EXC_BP, bp_handler));
    KTEST_CHECK(!interrupt_register(EXC_BP, bp_handler));           /* pas d'écrasement */
    uint32_t after_eflags;
    uintptr_t expected_eip;
    /* EAX = valeur témoin ; DF = 1 avant int3 ; on relit EFLAGS au retour. */
    __asm__ __volatile__(
        "movl $0xCAFEBABE, %%eax\n\t"
        "std\n\t"
        "int3\n\t"
        "1:\n\t"
        "pushfl\n\t"
        "popl %0\n\t"
        "cld\n\t"
        "movl $1b, %1\n\t"
        : "=r"(after_eflags), "=r"(expected_eip) : : "eax", "memory", "cc");
    interrupt_unregister(EXC_BP);

    KTEST_CHECK(g_obs.calls == 1);
    KTEST_CHECK(g_obs.vector == EXC_BP && g_obs.err == 0);
    KTEST_CHECK(g_obs.eip == expected_eip);              /* trappe : EIP après int3 */
    KTEST_CHECK(g_obs.eax == 0xCAFEBABEu);               /* registres sauvegardés fidèlement */
    KTEST_CHECK(g_obs.cs == GDT_KERNEL_CODE_SEL);
    KTEST_CHECK((g_obs.frame_eflags & EFLAGS_DF) != 0);  /* DF du code interrompu = 1 */
    KTEST_CHECK((g_obs.handler_eflags & EFLAGS_DF) == 0);/* isr_common a fait CLD */
    KTEST_CHECK((g_obs.handler_eflags & EFLAGS_IF) == 0);/* porte d'interruption : IF = 0 */
    KTEST_CHECK((after_eflags & EFLAGS_DF) != 0);        /* IRETD a restauré DF */
    KTEST_CHECK(((g_obs.handler_fp + 8u) & 0xFu) == 0);  /* ABI : pile alignée dans le C */
}

static void test_invalid_opcode(void)
{
    reset_obs();
    KTEST_CHECK(interrupt_register(EXC_UD, skip2_handler));
    uintptr_t fault_eip;
    __asm__ __volatile__("movl $1f, %0\n\t1: ud2\n\t" : "=r"(fault_eip) : : "memory");
    interrupt_unregister(EXC_UD);
    KTEST_CHECK(g_obs.calls == 1 && g_obs.vector == EXC_UD);
    KTEST_CHECK(g_obs.eip == fault_eip);                 /* faute : EIP sur l'instruction */
    KTEST_CHECK(g_obs.insn0 == 0x0F && g_obs.insn1 == 0x0B);
}

static void test_divide_error(void)
{
    reset_obs();
    KTEST_CHECK(interrupt_register(EXC_DE, skip2_handler));
    uintptr_t fault_eip;
    __asm__ __volatile__(
        "movl $1f, %0\n\t"
        "xorl %%edx, %%edx\n\t"
        "movl $1, %%eax\n\t"
        "xorl %%ecx, %%ecx\n\t"
        "1: divl %%ecx\n\t"
        : "=r"(fault_eip) : : "eax", "ecx", "edx", "memory", "cc");
    interrupt_unregister(EXC_DE);
    KTEST_CHECK(g_obs.calls == 1 && g_obs.vector == EXC_DE);
    KTEST_CHECK(g_obs.eip == fault_eip);
    KTEST_CHECK(g_obs.insn0 == 0xF7 && g_obs.insn1 == 0xF1);  /* div ecx */
}

void ktest_suite_cpu(void)
{
    ktest_suite("K0.4 cpu");
    test_gdt();
    test_idt();
    test_breakpoint();
    test_invalid_opcode();
    test_divide_error();
    /* Après les tests : aucun gestionnaire de test ne doit rester actif. */
    KTEST_CHECK(interrupt_register(EXC_BP, bp_handler));
    interrupt_unregister(EXC_BP);
}
