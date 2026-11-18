/*
 * SAMOS / AtlasOS — kernel/tests/test_vmmgate.c
 * ---------------------------------------------------------------------------
 * K0.7-P4e-1 — Autotests du passage d'écriture des PDE/PTE intégré au VMM
 * (paging.c : pt_gate_write). Les tables de pages ne sont PAS encore en
 * lecture seule (P4e-2) : ces tests prouvent le passage (validation,
 * fenêtre, restauration, compteurs), pas encore la protection des tables.
 *
 * Sûreté des tests de REFUS : chaque appel refusé reçoit une valeur dont
 * l'acceptation (sous une mutation qui retirerait une vérification) serait
 * immédiatement annulée par refused() : l'entrée d'origine est rétablie par
 * le passage lui-même (valeur d'origine, toujours acceptable), si bien que
 * l'échec reste une ASSERTION, pas une corruption.
 */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pmm.h>

#define K_PDE 0
#define K_PTE 1

static uint32_t entry_of(int kind, uint32_t va)
{
    const volatile uint32_t *pd = (const volatile uint32_t *)(uintptr_t)paging_directory();
    if (kind == K_PDE) {
        return pd[va >> 22];
    }
    if (!(pd[va >> 22] & PTE_PRESENT)) {
        return 0;
    }
    return ((const volatile uint32_t *)(uintptr_t)(pd[va >> 22] & PTE_FRAME))[(va >> 12) & 0x3FFu];
}

static bool wp_if_intact(uint32_t ifl)
{
    return (cpu_read_cr0() & CR0_WP) && (cpu_read_eflags() & EFLAGS_IF) == ifl;
}

/* Vrai si le passage refuse (code attendu), sans aucune écriture ; en cas
 * d'acceptation inattendue, l'entrée d'origine est restaurée. */
static bool refused(int kind, uint32_t va, uint32_t value, samos_err_t want)
{
    uint32_t before = entry_of(kind, va);
    samos_err_t e = paging_gate_test_write(kind, va, value);
    if (e == SAMOS_OK) {
        (void)paging_gate_test_write(kind, va, before);
        cpu_invlpg((const void *)(uintptr_t)va);
    }
    return e == want && entry_of(kind, va) == before;
}

void ktest_suite_vmm_gate(void)
{
    ktest_suite("K0.7 P4e-1 VMM write gate");
    const struct pmm *p = pmm_state();
    uint32_t ifl = cpu_read_eflags() & EFLAGS_IF;
    uint32_t w0, r0, w1, r1;
    paging_gate_stats(&w0, &r0);
    KTEST_CHECK(w0 >= 3u);       /* au moins le scellement P4d est passé par la fenêtre */

    /* --- Écritures autorisées, par les chemins du VMM --- */
    uint32_t f = pmm_alloc_frame();
    KTEST_CHECK(f != 0);
    const uint32_t va_old = 0xD00A0000u;                   /* région à table existante */
    bool mapped = vmm_map(va_old, f, PTE_WRITE) == SAMOS_OK;
    KTEST_CHECK(mapped && pmm_refs(f) == 1);
    paging_gate_stats(&w1, &r1);
    KTEST_CHECK(w1 == w0 + 1u && r1 == r0);                /* une fenêtre : la PTE */
    KTEST_CHECK((entry_of(K_PTE, va_old) & ~(uint32_t)(PTE_ACCESSED | PTE_DIRTY)) ==
                (f | PTE_PRESENT | PTE_WRITE));
    KTEST_CHECK(wp_if_intact(ifl));
    /* A/D posés par le CPU (écriture physique), non par le passage. Accès
     * seulement si la projection existe : sous une mutation qui la ferait
     * échouer, l'échec reste une assertion, pas une faute de page. */
    if (mapped) {
        *(volatile uint32_t *)(uintptr_t)va_old = 0xA0D0A0D0u;
    }
    KTEST_CHECK((entry_of(K_PTE, va_old) & (PTE_ACCESSED | PTE_DIRTY)) == (PTE_ACCESSED | PTE_DIRTY));
    KTEST_CHECK(vmm_unmap(va_old, 0) == SAMOS_OK && pmm_refs(f) == 0);   /* efface aussi A/D */
    paging_gate_stats(&w1, &r1);
    KTEST_CHECK(w1 == w0 + 2u && entry_of(K_PTE, va_old) == 0);

    /* Nouvelle table : PDE + PTE par le passage, table PAGETABLE, remise à
     * zéro, PDE exactement P | W, compteur de tables +1. */
    const uint32_t va_new = 0xC8000000u;                   /* région sans table */
    KTEST_CHECK(!(entry_of(K_PDE, va_new) & PTE_PRESENT));
    uint32_t tables0 = paging_table_count();
    KTEST_CHECK(vmm_map(va_new, f, 0) == SAMOS_OK);
    paging_gate_stats(&w1, &r1);
    KTEST_CHECK(w1 == w0 + 4u && paging_table_count() == tables0 + 1u);
    uint32_t pde = entry_of(K_PDE, va_new), nt = pde & PTE_FRAME;
    KTEST_CHECK((pde & 0xFFFu & ~(uint32_t)PTE_ACCESSED) == (PTE_PRESENT | PTE_WRITE));
    KTEST_CHECK(pmm_core_type(p, nt >> 12) == FRAME_PAGETABLE && pmm_free_frame(nt) == SAMOS_EINVAL);
    uint32_t nonzero = 0;
    for (uint32_t i = 1; nt != 0 && i < 1024u; i++) {     /* nt == 0 : PDE absente, rien à lire */
        nonzero += ((const volatile uint32_t *)(uintptr_t)nt)[i] != 0 ? 1u : 0u;
    }
    KTEST_CHECK(nonzero == 0);                             /* table remise à zéro */
    KTEST_CHECK(vmm_unmap(va_new, 0) == SAMOS_OK && pmm_free_frame(f) == SAMOS_OK);

    /* --- Refus (validation AVANT toute écriture) --- */
    uint32_t a = pmm_alloc_frame();
    KTEST_CHECK(a != 0);
    uint32_t kpage = (uint32_t)(uintptr_t)&kmain & PTE_FRAME;
    uint32_t kpte = entry_of(K_PTE, kpage);
    KTEST_CHECK(refused(K_PTE, kpage, kpte | PTE_USER, SAMOS_EPERM));             /* bit U */
    KTEST_CHECK(refused(K_PTE, kpage, kpte | PTE_WRITE, SAMOS_EPERM));            /* W sur .text */
    KTEST_CHECK(refused(K_PTE, (uint32_t)(uintptr_t)__kro_idt_start,
                        entry_of(K_PTE, (uint32_t)(uintptr_t)__kro_idt_start) | PTE_WRITE, SAMOS_EPERM)); /* P4d */
    KTEST_CHECK(refused(K_PTE, kpage, 0, SAMOS_EPERM));                           /* effacer l'identité */
    KTEST_CHECK(refused(K_PTE, kpage, (kpage + 4096u) | PTE_PRESENT, SAMOS_EPERM)); /* identité inexacte */
    KTEST_CHECK(refused(K_PTE, kpage, kpte | 0x010u, SAMOS_EINVAL));              /* PCD */
    KTEST_CHECK(refused(K_PTE, (uint32_t)(uintptr_t)__stack_top_guard_start,
                        (uint32_t)(uintptr_t)__stack_top_guard_start | PTE_PRESENT, SAMOS_EPERM)); /* garde */
    KTEST_CHECK(refused(K_PTE, (uint32_t)(uintptr_t)__df_guard_start,
                        (uint32_t)(uintptr_t)__df_guard_start | PTE_PRESENT, SAMOS_EPERM));
    KTEST_CHECK(refused(K_PTE, 0xD00A1000u, kpage | PTE_PRESENT, SAMOS_EPERM));   /* hors identité : cadre KERNEL */
    KTEST_CHECK(refused(K_PTE, 0xF4000000u, a | PTE_PRESENT, SAMOS_ENOENT));      /* sans table */
    KTEST_CHECK(refused(K_PTE, 0xD00A1004u, a | PTE_PRESENT, SAMOS_EINVAL));      /* non alignée */
    KTEST_CHECK(refused(K_PDE, 0xF4000000u, a | PTE_PRESENT | PTE_WRITE, SAMOS_EPERM));   /* table ALLOCATED */
    KTEST_CHECK(refused(K_PDE, 0xF4000000u, paging_directory() | PTE_PRESENT | PTE_WRITE, SAMOS_EPERM)); /* répertoire */
    KTEST_CHECK(refused(K_PDE, 0xF4000000u, nt | PTE_PRESENT | PTE_WRITE | PTE_USER, SAMOS_EPERM));      /* PDE U */
    KTEST_CHECK(wp_if_intact(ifl));

    /* Imbrication : fenêtre simulée en cours. */
    paging_gate_test_set_active(true);
    samos_err_t en = paging_gate_test_write(K_PTE, kpage, kpte);
    paging_gate_test_set_active(false);
    KTEST_CHECK(en == SAMOS_EBUSY && entry_of(K_PTE, kpage) == kpte);

    /* IF = 1 à l'entrée : restauré exactement. Écriture autorisée (réécriture
     * de la PTE de .text avec sa propre valeur, sans W). */
    uint32_t fl = irq_save();
    cpu_sti();
    samos_err_t ei = paging_gate_test_write(K_PTE, kpage, kpte);
    bool if_kept = (cpu_read_eflags() & EFLAGS_IF) != 0;
    cpu_cli();
    irq_restore(fl);
    KTEST_CHECK(ei == SAMOS_OK && if_kept && (cpu_read_cr0() & CR0_WP));
    paging_gate_stats(&w1, &r1);
    KTEST_CHECK(r1 >= r0 + 14u);                           /* refus comptés */

    /* Pages P4d intactes ; .text toujours RO ; PMM cohérent. */
    uint32_t pa = 0, fla = 0;
    KTEST_CHECK(vmm_translate((uint32_t)(uintptr_t)__kro_idt_start, &pa, &fla) && !(fla & PTE_WRITE));
    KTEST_CHECK(vmm_translate((uint32_t)(uintptr_t)__kro_vmm_start, &pa, &fla) && !(fla & PTE_WRITE));
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)&kmain, 0) == -1);
    KTEST_CHECK(pmm_free_frame(a) == SAMOS_OK && pmm_core_check(p) == 0);
}
