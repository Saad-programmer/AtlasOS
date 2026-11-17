/*
 * SAMOS / AtlasOS — kernel/tests/test_mm.c
 * ---------------------------------------------------------------------------
 * Autotests du jalon K0.6 : carte mémoire, allocateur physique, pagination,
 * protections. Les violations de protection sont provoquées par des SONDES
 * (probe.asm) : la faute de page est interceptée, et l'on vérifie l'adresse
 * (CR2) et le code d'erreur rapportés par le processeur.
 */
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/paging.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/mm.h>
#include <samos/multiboot.h>
#include <samos/pit.h>
#include <samos/pmm.h>

static volatile uint32_t g_rw_var = 0x11111111u;          /* .data : RW */
static const uint32_t g_ro_var = 0x22222222u;             /* .rodata : R */

/* Code d'erreur #PF : bit 0 P (1 = violation de protection, 0 = non présent),
 * bit 1 W/R (1 = écriture), bit 2 U/S (1 = mode utilisateur). */
#define PF_P 1u
#define PF_W 2u

static void test_memmap(void)
{
    const struct memmap *m = mm_memmap();
    KTEST_CHECK(m->count > 0 && !m->malformed);
    bool sorted = true;
    for (unsigned i = 1; i < m->count; i++) {
        sorted = sorted && m->r[i - 1].base <= m->r[i].base;
    }
    KTEST_CHECK(sorted);
    KTEST_CHECK(memmap_total(m, MULTIBOOT_MEMORY_AVAILABLE) > 100u * 1024 * 1024);  /* -m 128M */
}

static bool frame_ok(uint32_t phys)
{
    uint32_t ks = (uint32_t)(uintptr_t)__kernel_start, ke = (uint32_t)(uintptr_t)__kernel_end;
    return phys != 0 && (phys & 0xFFFu) == 0 && phys >= 0x100000u && !(phys >= ks && phys < ke);
}

static void test_pmm(void)
{
    const struct pmm *p = pmm_state();
    uint32_t free0 = p->free_count;
    KTEST_CHECK(p->nframes > 0 && free0 > 0 && free0 < p->nframes);

    uint32_t a = pmm_alloc_frame();
    KTEST_CHECK(frame_ok(a));
    KTEST_CHECK(p->free_count == free0 - 1);
    /* Salir le cadre, le libérer, réallouer : le cadre rendu est remis à zéro. */
    volatile uint32_t *pa = (volatile uint32_t *)(uintptr_t)a;
    for (unsigned i = 0; i < 1024; i++) pa[i] = 0xDEADBEEFu;
    KTEST_CHECK(pmm_free_frame(a) == SAMOS_OK);
    uint32_t b = pmm_alloc_frame();
    volatile uint32_t *pb = (volatile uint32_t *)(uintptr_t)b;
    bool zero = true;
    for (unsigned i = 0; i < 1024; i++) zero = zero && pb[i] == 0;
    KTEST_CHECK(b == a);                                  /* next-fit : même cadre */
    KTEST_CHECK(zero);                                    /* aucune fuite d'ancien contenu */
    KTEST_CHECK(pmm_free_frame(b) == SAMOS_OK);

    /* Libérations interdites */
    KTEST_CHECK(pmm_free_frame(b) == SAMOS_ENOENT);       /* double libération */
    KTEST_CHECK(pmm_free_frame((uint32_t)(uintptr_t)__text_start) == SAMOS_EINVAL);  /* noyau */
    KTEST_CHECK(pmm_free_frame(0xB8000) == SAMOS_EINVAL); /* mémoire basse (VGA) */
    KTEST_CHECK(pmm_free_frame(0) == SAMOS_EINVAL);
    KTEST_CHECK(pmm_free_frame(b + 4) == SAMOS_EINVAL);   /* non aligné */
    KTEST_CHECK(pmm_free_frame(0xF0000000u) == SAMOS_EINVAL);  /* hors mémoire gérée */
    KTEST_CHECK(p->free_count == free0);

    /* Épuisement complet : chaque cadre est valide, puis tout est rendu.
     * Interruptions actives pendant la mesure : sinon le PIT ne compte pas
     * et la durée affichée serait fausse (0 ms, constaté à la 1re exécution). */
    cpu_sti();
    uint64_t t0 = pit_uptime_ms();
    uint32_t n = 0, bad = 0, f, first = 0, last = 0;
    while ((f = pmm_alloc_frame()) != 0) {
        if (!frame_ok(f)) bad++;
        if (n == 0) first = f;
        last = f;
        n++;
    }
    KTEST_CHECK(n == free0 && bad == 0 && p->free_count == 0);
    KTEST_CHECK(pmm_alloc_frame() == 0);                  /* épuisé : échec propre */
    uint32_t freed = 0;
    for (uint32_t ph = 0x100000; ph < (p->nframes << PAGE_SHIFT); ph += PAGE_SIZE) {
        if (!pmm_core_is_reserved(p, ph >> PAGE_SHIFT) && pmm_core_is_used(p, ph >> PAGE_SHIFT)) {
            /* les cadres des tables de pages restent alloués : on ne rend que
             * ceux pris par ce test (ils sont tous dans [first, last]) */
            if (ph >= first && ph <= last && pmm_free_frame(ph) == SAMOS_OK) freed++;
        }
    }
    KTEST_CHECK(freed == n && p->free_count == free0);
    kprintf("[TEST] pmm exhaustion: %u frames (%u KiB) allocated+zeroed and freed in %llu ms\n",
            n, n * 4u, (unsigned long long)(pit_uptime_ms() - t0));
    cpu_cli();
}

static void test_paging_state(void)
{
    uint32_t cr0 = cpu_read_cr0();
    KTEST_CHECK((cr0 & CR0_PG) && (cr0 & CR0_WP));
    KTEST_CHECK(cpu_read_cr3() == paging_directory());
    KTEST_CHECK(tss_double_fault()->cr3 == paging_directory());   /* la tâche #DF voit les mêmes pages */
    uint32_t pa, fl;
    uint32_t text = (uint32_t)(uintptr_t)&kmain;
    KTEST_CHECK(vmm_translate(text, &pa, &fl) && pa == text && !(fl & PTE_WRITE));   /* identité, R */
    uint32_t ro = (uint32_t)(uintptr_t)&g_ro_var;
    KTEST_CHECK(vmm_translate(ro, &pa, &fl) && !(fl & PTE_WRITE));
    uint32_t rw = (uint32_t)(uintptr_t)&g_rw_var;
    KTEST_CHECK(vmm_translate(rw, &pa, &fl) && (fl & PTE_WRITE));
    KTEST_CHECK(vmm_translate(0xB8000, &pa, &fl) && (fl & PTE_WRITE));             /* VGA */
    KTEST_CHECK(!vmm_translate(0, 0, 0));                                          /* page nulle */
    KTEST_CHECK(!vmm_translate((uint32_t)(uintptr_t)__stack_guard_start, 0, 0));   /* garde */
    KTEST_CHECK(vmm_translate((uint32_t)(uintptr_t)boot_stack_bottom, 0, 0));      /* pile */
    KTEST_CHECK(!(fl & PTE_USER));                                                 /* superviseur */
}

static void test_protections(void)
{
    uint32_t v = 0, cr2 = 0, err = 0;
    /* Accès légitimes */
    KTEST_CHECK(mem_probe_read((const void *)&kmain, &v) == 0);        /* lire .text : permis */
    KTEST_CHECK(mem_probe_write(&g_rw_var, 0x33333333u) == 0 && g_rw_var == 0x33333333u);
    KTEST_CHECK(mem_probe_read(&g_ro_var, &v) == 0 && v == 0x22222222u);

    /* NULL : page non présente, lecture */
    KTEST_CHECK(mem_probe_read((const void *)0, &v) == -1);
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(cr2 == 0 && (err & (PF_P | PF_W)) == 0);

    /* Écriture dans .text : présente mais en lecture seule => P=1, W=1 */
    uint32_t text = (uint32_t)(uintptr_t)&kmain;
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)text, 0x90909090u) == -1);
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(cr2 == text && (err & (PF_P | PF_W)) == (PF_P | PF_W));
    KTEST_CHECK(mem_probe_read((const void *)&kmain, &v) == 0);        /* .text intact et lisible */

    /* Écriture dans .rodata */
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)&g_ro_var, 0) == -1 && g_ro_var == 0x22222222u);

    /* Page de garde : non présente */
    uint32_t guard = (uint32_t)(uintptr_t)__stack_guard_start + 0x800u;
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)guard, &v) == -1);
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(cr2 == guard && (err & PF_P) == 0);

    /* Au-delà de la RAM projetée */
    KTEST_CHECK(mem_probe_read((const void *)0xE0000000u, &v) == -1);
}

static void test_map_unmap(void)
{
    const uint32_t va = 0xD0000000u;                     /* hors identité : jamais projetée */
    uint32_t frame = pmm_alloc_frame();
    uint32_t tables0 = paging_table_count();
    KTEST_CHECK(frame != 0);
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)va, &(uint32_t){0}) == -1);
    KTEST_CHECK(vmm_map(va, frame, PTE_WRITE) == SAMOS_OK);
    KTEST_CHECK(paging_table_count() == tables0 + 1);    /* nouvelle table pour 0xD0000000 */
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)va, 0xA5A5A5A5u) == 0);
    /* Alias : la même mémoire physique, vue par l'identité, contient la valeur. */
    KTEST_CHECK(*(volatile uint32_t *)(uintptr_t)frame == 0xA5A5A5A5u);
    uint32_t pa;
    KTEST_CHECK(vmm_translate(va + 0x123, &pa, 0) && pa == frame + 0x123);
    KTEST_CHECK(vmm_map(va, frame, PTE_WRITE) == SAMOS_EEXIST);         /* pas d'écrasement */
    KTEST_CHECK(vmm_map(va + 1, frame, 0) == SAMOS_EINVAL);             /* non aligné */
    /* L'accès précédent a chargé la traduction dans le TLB : si vmm_unmap
     * oubliait INVLPG, la lecture suivante réussirait encore. */
    uint32_t out = 0;
    KTEST_CHECK(vmm_unmap(va, &out) == SAMOS_OK && out == frame);
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)va, &(uint32_t){0}) == -1);
    KTEST_CHECK(vmm_unmap(va, 0) == SAMOS_ENOENT);
    /* Projection en lecture seule : l'écriture doit fauter. */
    KTEST_CHECK(vmm_map(va, frame, 0) == SAMOS_OK);
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)va, 1) == -1);
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)va, &out) == 0 && out == 0xA5A5A5A5u);
    KTEST_CHECK(vmm_unmap(va, 0) == SAMOS_OK);
    KTEST_CHECK(pmm_free_frame(frame) == SAMOS_OK);
}

/*
 * K0.7-M1 — E1 et E2 : la page nulle et la page de garde ne doivent jamais
 * pouvoir être projetées, quels que soient le cadre et les droits demandés,
 * et la protection doit rester effective après les tentatives.
 */
static void test_forbidden_mappings(void)
{
    uint32_t frame = pmm_alloc_frame();
    uint32_t guard = (uint32_t)(uintptr_t)__stack_guard_start;
    uint32_t v = 0;
    KTEST_CHECK(frame != 0);
    KTEST_CHECK(vmm_map(0, frame, PTE_WRITE) == SAMOS_EPERM);        /* E1 */
    KTEST_CHECK(vmm_map(0, frame, 0) == SAMOS_EPERM);                /* E1, lecture seule */
    KTEST_CHECK(vmm_map(guard, frame, PTE_WRITE) == SAMOS_EPERM);    /* E2 */
    KTEST_CHECK(vmm_map(guard, frame, 0) == SAMOS_EPERM);            /* E2, lecture seule */
    /* Les tentatives n'ont rien laissé : NULL et la garde fautent toujours. */
    KTEST_CHECK(!vmm_translate(0, 0, 0));
    KTEST_CHECK(!vmm_translate(guard, 0, 0));
    KTEST_CHECK(mem_probe_read((const void *)0, &v) == -1);
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)guard, &v) == -1);
    /* Une adresse voisine légitime reste projetable (pas de refus excessif). */
    KTEST_CHECK(vmm_map(0xD0002000u, frame, PTE_WRITE) == SAMOS_OK);
    KTEST_CHECK(vmm_unmap(0xD0002000u, 0) == SAMOS_OK);
    KTEST_CHECK(pmm_free_frame(frame) == SAMOS_OK);
}

void ktest_suite_mm(void)
{
    ktest_suite("K0.6 mm");
    test_memmap();
    test_pmm();
    test_paging_state();
    test_protections();
    test_map_unmap();
    test_forbidden_mappings();
}
