/*
 * SAMOS / AtlasOS — kernel/tests/test_sealed.c
 * ---------------------------------------------------------------------------
 * K0.7-P4d — Autotests de l'état SCELLÉ (exécutés après paging_seal_critical,
 * voir ktest_run_all) : kro_idt, kro_handlers et kro_vmm présentes, lisibles,
 * sans PTE_WRITE ni PTE_USER, sans alias inscriptible ; le noyau fonctionne
 * (IRQ du PIT, #PF, VMM) ; les autres pages critiques restent RW.
 * Aucune écriture dans une page scellée n'est tentée hors des sondes
 * (mem_probe_write, qui faute sans écrire) ; les écritures DIRECTES sont dans
 * les modes destructifs test=sealidt|sealhandlers|sealvmm.
 */
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/paging.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pit.h>
#include <samos/pmm.h>
#include <samos/string.h>

static bool page_sealed(const char *page)
{
    uint32_t va = (uint32_t)(uintptr_t)page, pa = 0, fl = 0;
    return vmm_translate(va, &pa, &fl) && pa == va && (fl & PTE_PRESENT) &&
           !(fl & PTE_WRITE) && !(fl & PTE_USER);
}

static bool page_writable(uint32_t va)
{
    uint32_t pa = 0, fl = 0;
    return vmm_translate(va, &pa, &fl) && (fl & PTE_WRITE);
}

/* Sonde d'écriture : faute de protection (P = 1, W = 1, U = 0 : code 3) à
 * l'adresse exacte, contenu inchangé. */
static bool write_refused(const char *page)
{
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)page;
    uint32_t before = *w, cr2 = 0, err = 0;
    int r = mem_probe_write((void *)(uintptr_t)w, before ^ 0xA5A5A5A5u);
    mem_probe_last_fault(&cr2, &err);
    if (r == 0) {
        /* Écriture ACCEPTÉE (page non scellée, par exemple sous mutation) :
         * restauration immédiate, pour que l'échec reste une assertion et ne
         * devienne pas une corruption de l'IDT ou des racines du VMM. */
        (void)mem_probe_write((void *)(uintptr_t)w, before);
        return false;
    }
    return r == -1 && cr2 == (uint32_t)(uintptr_t)w && err == 3u && *w == before;
}

/* Parcours de TOUTES les PTE présentes : chaque entrée qui désigne un cadre
 * scellé doit être l'identité, sans W. Renvoie le nombre d'entrées fautives. */
static uint32_t writable_aliases_of(const char *page)
{
    uint32_t frame = (uint32_t)(uintptr_t)page, bad = 0, seen = 0;
    const volatile uint32_t *pd = (const volatile uint32_t *)(uintptr_t)paging_directory();
    for (uint32_t i = 0; i < 1024u; i++) {
        if (!(pd[i] & PTE_PRESENT)) {
            continue;
        }
        const volatile uint32_t *pt = (const volatile uint32_t *)(uintptr_t)(pd[i] & PTE_FRAME);
        for (uint32_t j = 0; j < 1024u; j++) {
            uint32_t e = pt[j];
            if ((e & PTE_PRESENT) && (e & PTE_FRAME) == frame) {
                seen++;
                uint32_t va = (i << 22) | (j << 12);
                if ((e & PTE_WRITE) || va != frame) {
                    bad++;
                }
            }
        }
    }
    return bad + (seen == 1u ? 0u : 100u);        /* exactement une projection : l'identité */
}

void ktest_suite_sealed(void)
{
    ktest_suite("K0.7 sealed structures (P4d)");
    const struct pmm *p = pmm_state();

    /* 1. Les trois pages : présentes, lisibles, sans W ni U. */
    bool idt_ro = page_sealed(__kro_idt_start);
    bool handlers_ro = page_sealed(__kro_handlers_start);
    bool vmm_ro = page_sealed(__kro_vmm_start);
    KTEST_CHECK(idt_ro);
    KTEST_CHECK(handlers_ro);
    KTEST_CHECK(vmm_ro);
    KTEST_CHECK((cpu_read_cr0() & CR0_WP) != 0);

    /* 2. Écriture refusée par le MMU (CR2 exact, code 3, contenu intact). */
    KTEST_CHECK(write_refused(__kro_idt_start));
    KTEST_CHECK(write_refused(__kro_handlers_start));
    KTEST_CHECK(write_refused(__kro_vmm_start));
    KTEST_CHECK(write_refused(__kro_vmm_end - 4));

    /* 3. Aucun alias : une seule projection de chaque cadre, l'identité, sans W. */
    KTEST_CHECK(writable_aliases_of(__kro_idt_start) == 0);
    KTEST_CHECK(writable_aliases_of(__kro_handlers_start) == 0);
    KTEST_CHECK(writable_aliases_of(__kro_vmm_start) == 0);
    uint32_t f = pmm_alloc_frame();
    KTEST_CHECK(f != 0);
    KTEST_CHECK(vmm_map(0xD0090000u, (uint32_t)(uintptr_t)__kro_idt_start, 0) == SAMOS_EPERM);
    KTEST_CHECK(vmm_map(0xD0090000u, (uint32_t)(uintptr_t)__kro_vmm_start, PTE_WRITE) == SAMOS_EPERM);

    /* 4. Le noyau scellé fonctionne : IDT chargée, IRQ du PIT, #PF, VMM. */
    struct cpu_dtr d = { 0, 0 };
    cpu_sidt(&d);
    KTEST_CHECK(d.base == (uint32_t)(uintptr_t)idt_table() && d.base == (uint32_t)(uintptr_t)__kro_idt_start);
    uint32_t fl = irq_save();
    cpu_sti();
    uint64_t t0 = pit_ticks();
    for (uint32_t spin = 0; spin < 50000000u && pit_ticks() < t0 + 3u; spin++) {
        __asm__ __volatile__("pause");
    }
    uint64_t t1 = pit_ticks();
    cpu_cli();
    irq_restore(fl);
    KTEST_CHECK(t1 >= t0 + 3u);                                 /* IRQ0 livrées */
    uint32_t v = 0;
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)0xE0000000u, &v) == -1);  /* #PF traitée */
    KTEST_CHECK(vmm_map(0xD0090000u, f, PTE_WRITE) == SAMOS_OK && pmm_refs(f) == 1);
    KTEST_CHECK(vmm_unmap(0xD0090000u, 0) == SAMOS_OK && pmm_refs(f) == 0);

    /* 5. paging_seal_identity : refus sans effet ; idempotence. */
    KTEST_CHECK(paging_seal_identity(0xD0000000u, 0xD0001000u) == SAMOS_EINVAL);  /* hors identité */
    KTEST_CHECK(paging_seal_identity(0x00100004u, 0x00101000u) == SAMOS_EINVAL);  /* non alignée */
    KTEST_CHECK(paging_seal_identity(f, f + 4096u) == SAMOS_EPERM && page_writable(f)); /* ALLOCATED */
    KTEST_CHECK(paging_seal_identity(0, 4096u) == SAMOS_EPERM);                   /* page nulle */
    KTEST_CHECK(paging_seal_critical() == SAMOS_OK && page_sealed(__kro_idt_start));
    KTEST_CHECK(pmm_free_frame(f) == SAMOS_OK);

    /* 6. Ce qui reste RW après P4d : GDT/TSS, compteurs/PMM. Le répertoire et
     * les tables, RW en P4d, sont scellés depuis K0.7-P4e-2. */
    KTEST_CHECK(page_writable((uint32_t)(uintptr_t)__kcpu_start));
    KTEST_CHECK(page_writable((uint32_t)(uintptr_t)p->meta) && page_writable((uint32_t)(uintptr_t)p));
    KTEST_CHECK(!page_writable(paging_directory()));
    KTEST_CHECK(!page_writable(paging_directory() + 4096u));    /* première table */
    /* Gardes toujours absentes. */
    KTEST_CHECK(!vmm_translate((uint32_t)(uintptr_t)__df_guard_start, 0, 0) &&
                !vmm_translate((uint32_t)(uintptr_t)__stack_guard_start, 0, 0) &&
                !vmm_translate((uint32_t)(uintptr_t)__stack_top_guard_start, 0, 0));
    KTEST_CHECK(pmm_core_check(p) == 0);
}

/* Modes destructifs : écriture DIRECTE (pas une sonde) dans une page scellée,
 * depuis S1. Attendu : #PF code 3, panic, statut 37. */
void ktest_sealed_write(const char *what)
{
    const char *target = strcmp(what, "sealidt") == 0      ? __kro_idt_start
                       : strcmp(what, "sealhandlers") == 0 ? __kro_handlers_start
                                                           : __kro_vmm_start;
    kprintf("[TEST] writing to sealed page (%s) at %p\n", what, (const void *)target);
    *(volatile uint32_t *)(uintptr_t)target = 0x5EA1ED00u;
    kprintf("[TEST] sealed write NOT trapped\n");
}
