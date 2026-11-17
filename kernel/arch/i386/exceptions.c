/*
 * SAMOS / AtlasOS — kernel/arch/i386/exceptions.c
 * ---------------------------------------------------------------------------
 * Répartition des interruptions et traitement des exceptions processeur
 * (K0.4). Voir Details.md §24.
 *
 * Toute exception sans gestionnaire enregistré est FATALE : on affiche un
 * rapport complet (nom, code d'erreur décodé, registres, CR0/CR2/CR3) puis
 * panic(). Aucune exception n'est ignorée silencieusement.
 */
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/interrupt.h>
#include <arch/irq.h>
#include <arch/pic.h>
#include <arch/paging.h>
#include <samos/log.h>
#include <samos/panic.h>

static interrupt_handler_t g_handlers[IDT_ENTRIES];

static const char *const g_exc_names[EXC_COUNT] = {
    "Divide Error (#DE)", "Debug (#DB)", "Non-Maskable Interrupt (NMI)", "Breakpoint (#BP)",
    "Overflow (#OF)", "BOUND Range Exceeded (#BR)", "Invalid Opcode (#UD)", "Device Not Available (#NM)",
    "Double Fault (#DF)", "Coprocessor Segment Overrun", "Invalid TSS (#TS)", "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)", "General Protection (#GP)", "Page Fault (#PF)", "Reserved (15)",
    "x87 Floating-Point Error (#MF)", "Alignment Check (#AC)", "Machine Check (#MC)", "SIMD Floating-Point (#XM)",
    "Virtualization (#VE)", "Control Protection (#CP)", "Reserved (22)", "Reserved (23)",
    "Reserved (24)", "Reserved (25)", "Reserved (26)", "Reserved (27)",
    "Hypervisor Injection (#HV)", "VMM Communication (#VC)", "Security (#SX)", "Reserved (31)",
};

const char *exception_name(uint32_t vector)
{
    return (vector < EXC_COUNT) ? g_exc_names[vector] : "Interrupt";
}

bool exception_has_error_code(uint32_t vector)
{
    switch (vector) {
    case 8: case 10: case 11: case 12: case 13: case 14: case 17: case 21: case 29: case 30:
        return true;
    default:
        return false;
    }
}

bool interrupt_register(uint8_t vector, interrupt_handler_t handler)
{
    if (handler == 0 || g_handlers[vector] != 0) {
        return false;
    }
    g_handlers[vector] = handler;
    return true;
}

void interrupt_unregister(uint8_t vector)
{
    g_handlers[vector] = 0;
}

/*
 * Code d'erreur « sélecteur » (#TS, #NP, #SS, #GP ; SDM vol. 3A §6.13) :
 *   bit 0 EXT : l'événement est d'origine externe (interruption matérielle)
 *   bit 1 IDT : l'index désigne une entrée de l'IDT
 *   bit 2 TI  : (si IDT = 0) 0 = GDT, 1 = LDT
 *   bits 3..15 : index
 */
static void decode_selector_error(uint32_t err)
{
    if (err == 0) {
        kprintf("    error code 0 (no selector involved)\n");
        return;
    }
    const char *table = (err & 2u) ? "IDT" : ((err & 4u) ? "LDT" : "GDT");
    kprintf("    selector error: %s index %u%s\n", table, (unsigned)((err >> 3) & 0x1FFFu),
            (err & 1u) ? ", external event" : "");
}

/* Code d'erreur de #PF (SDM vol. 3A §4.7) : P, W/R, U/S, RSVD, I/D. */
static void decode_page_fault_error(uint32_t err)
{
    kprintf("    page fault: %s, %s access, %s mode%s%s, address CR2=%p\n",
            (err & 1u) ? "protection violation" : "page not present",
            (err & 2u) ? "write" : "read",
            (err & 4u) ? "user" : "supervisor",
            (err & 8u) ? ", reserved bit set" : "",
            (err & 16u) ? ", instruction fetch" : "",
            (void *)(uintptr_t)cpu_read_cr2());
}

void exception_report(const struct interrupt_frame *f)
{
    kprintf("*** CPU exception %u: %s, error code %p\n", (unsigned)f->vector,
            exception_name(f->vector), (void *)(uintptr_t)f->err_code);
    switch (f->vector) {
    case EXC_TS: case EXC_NP: case EXC_SS: case EXC_GP:
        decode_selector_error(f->err_code);
        break;
    case EXC_PF:
        decode_page_fault_error(f->err_code);
        break;
    default:
        break;
    }
    kprintf("    EAX=%p EBX=%p ECX=%p EDX=%p\n", (void *)f->eax, (void *)f->ebx, (void *)f->ecx, (void *)f->edx);
    kprintf("    ESI=%p EDI=%p EBP=%p ESP=%p\n", (void *)f->esi, (void *)f->edi, (void *)f->ebp,
            (void *)(uintptr_t)interrupt_frame_esp(f));
    kprintf("    EIP=%p CS=%04x EFLAGS=%p\n", (void *)f->eip, (unsigned)f->cs, (void *)f->eflags);
    kprintf("    DS=%04x ES=%04x FS=%04x GS=%04x\n", (unsigned)f->ds, (unsigned)f->es,
            (unsigned)f->fs, (unsigned)f->gs);
    kprintf("    CR0=%p CR2=%p CR3=%p\n", (void *)(uintptr_t)cpu_read_cr0(),
            (void *)(uintptr_t)cpu_read_cr2(), (void *)(uintptr_t)cpu_read_cr3());
}

void isr_dispatch(struct interrupt_frame *frame)
{
    uint32_t v = frame->vector;
    if (v >= PIC_IRQ_BASE && v < PIC_IRQ_BASE + PIC_IRQ_COUNT) {
        irq_dispatch(frame);           /* K0.5 : IRQ matérielles */
        return;
    }
    interrupt_handler_t h = (v < IDT_ENTRIES) ? g_handlers[v] : 0;
    if (h != 0) {
        h(frame);
        return;
    }
    exception_report(frame);
    panic("unhandled %s at EIP %p", exception_name(v), (void *)frame->eip);
}

/*
 * Tâche de double faute (atteinte par la porte de tâche du vecteur 8).
 * Elle s'exécute sur sa PROPRE pile (gdt.c), avec un état chargé depuis
 * g_tss_df. Le processeur a sauvegardé l'état de la tâche interrompue dans le
 * TSS principal (registre TR au moment de la faute) : on l'y relit.
 * Une double faute n'est jamais reprise (SDM : « abort ») : on panique.
 */
void double_fault_task(void)
{
    const struct tss *t = tss_main();
    kprintf("\n*** DOUBLE FAULT (#DF) caught by task gate, running on dedicated stack\n");
    kprintf("    interrupted task: EIP=%p ESP=%p EBP=%p EFLAGS=%p\n",
            (void *)(uintptr_t)t->eip, (void *)(uintptr_t)t->esp,
            (void *)(uintptr_t)t->ebp, (void *)(uintptr_t)t->eflags);
    kprintf("    EAX=%p EBX=%p ECX=%p EDX=%p CR3=%p\n", (void *)(uintptr_t)t->eax,
            (void *)(uintptr_t)t->ebx, (void *)(uintptr_t)t->ecx, (void *)(uintptr_t)t->edx,
            (void *)(uintptr_t)t->cr3);
    /* K0.6 : CR2 conserve l'adresse de la dernière faute de page. Si elle se
     * trouve dans la page de garde, la cause la plus probable est une pile
     * épuisée : la #PF n'a pas pu être livrée (pile non projetée) => #DF.
     * (CR2 peut être ancien si la #DF n'a pas pour origine une #PF.) */
    uint32_t cr2 = cpu_read_cr2();
    kprintf("    last page-fault address CR2=%p: %s\n", (void *)(uintptr_t)cr2,
            paging_classify(cr2, 0));
    panic("double fault");
}
