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
    /* M2.4d : 2 projections réussies puis 2 retraits => 0 ; EEXIST et EINVAL n'ont rien compté. */
    KTEST_CHECK(pmm_refs(frame) == 0);
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
    KTEST_CHECK(pmm_refs(frame) == 0);   /* M2.4d : 1 projection, 1 retrait ; 4 refus E1/E2 sans référence */
    KTEST_CHECK(pmm_free_frame(frame) == SAMOS_OK);
}

/* K0.7-M2.2 — Types réels des cadres et protection de la table elle-même. */
static void test_frame_types(void)
{
    const struct pmm *p = pmm_state();
    uint32_t meta_phys = (uint32_t)(uintptr_t)p->meta;       /* identité : va == pa */
    KTEST_CHECK(pmm_core_check(p) == 0);
    KTEST_CHECK(pmm_core_type(p, meta_phys >> PAGE_SHIFT) == FRAME_KERNEL);
    KTEST_CHECK(pmm_free_frame(meta_phys & ~0xFFFu) == SAMOS_EINVAL);   /* métadonnées protégées */
    /* Dernier octet de la table (128 cadres pour 2^18 descripteurs) : aussi KERNEL. */
    uint32_t meta_last = meta_phys + (uint32_t)(sizeof(frame_desc_t) << 18) - 1u;
    KTEST_CHECK(pmm_core_type(p, meta_last >> PAGE_SHIFT) == FRAME_KERNEL);
    KTEST_CHECK(pmm_free_frame(meta_last & ~0xFFFu) == SAMOS_EINVAL);
    KTEST_CHECK(pmm_core_type(p, ((uint32_t)(uintptr_t)&kmain) >> PAGE_SHIFT) == FRAME_KERNEL);
    KTEST_CHECK(pmm_core_type(p, 0xB8) == FRAME_RESERVED);              /* VGA, mémoire basse */
    KTEST_CHECK(pmm_core_type(p, 0) == FRAME_RESERVED);
    uint32_t f = pmm_alloc_frame();
    KTEST_CHECK(f != 0 && pmm_core_type(p, f >> PAGE_SHIFT) == FRAME_ALLOCATED);
    KTEST_CHECK(pmm_free_frame(f) == SAMOS_OK && pmm_core_type(p, f >> PAGE_SHIFT) == FRAME_FREE);
    KTEST_CHECK(pmm_core_check(p) == 0);                               /* G1 après les tests */
}

/* K0.7-M2.3 — E4 : cadres des structures de pagination.
 * paging_directory() renvoie l'adresse PHYSIQUE du répertoire (valeur de CR3). */
static void test_pagetable_frames(void)
{
    const struct pmm *p = pmm_state();
    uint32_t pd_frame = paging_directory() >> PAGE_SHIFT;
    KTEST_CHECK(pmm_core_type(p, pd_frame) == FRAME_PAGETABLE);

    /* Refus de libération du répertoire actif. On lit (identité) l'entrée du
     * répertoire qui couvre l'image du noyau, avant et après la tentative. */
    const volatile uint32_t *pd = (const volatile uint32_t *)(uintptr_t)paging_directory();
    uint32_t pde_before = pd[0x100000u >> 22];
    uint32_t free_before = p->free_count;
    uint32_t invalid_before = p->invalid_frees;
    KTEST_CHECK(pmm_free_frame(paging_directory()) == SAMOS_EINVAL);
    KTEST_CHECK(pmm_core_type(p, pd_frame) == FRAME_PAGETABLE);
    KTEST_CHECK(p->free_count == free_before);
    KTEST_CHECK(p->invalid_frees == invalid_before + 1);
    KTEST_CHECK(pd[0x100000u >> 22] == pde_before);                 /* contenu intact */
    KTEST_CHECK(cpu_read_cr3() == paging_directory());              /* toujours actif */
    uint32_t pa = 0, v = 0;
    KTEST_CHECK(vmm_translate((uint32_t)(uintptr_t)&kmain, &pa, 0) &&
                pa == (uint32_t)(uintptr_t)&kmain);                 /* traduction intacte */
    KTEST_CHECK(mem_probe_read((const void *)&kmain, &v) == 0);     /* accès réel intact */

    /* Table de pages active, distincte du répertoire : celle qui projette kmain.
     * Vérifier d'abord que c'est bien la structure attendue. */
    uint32_t kva = (uint32_t)(uintptr_t)&kmain;
    uint32_t pt_phys = pd[kva >> 22] & PTE_FRAME;
    const volatile uint32_t *pt = (const volatile uint32_t *)(uintptr_t)pt_phys;
    uint32_t pte_before = pt[(kva >> 12) & 0x3FFu];
    KTEST_CHECK(pt_phys != 0 && pt_phys != paging_directory());
    KTEST_CHECK((pte_before & PTE_PRESENT) && (pte_before & PTE_FRAME) == (kva & PTE_FRAME));
    KTEST_CHECK(pmm_core_type(p, pt_phys >> PAGE_SHIFT) == FRAME_PAGETABLE);
    free_before = p->free_count;
    KTEST_CHECK(pmm_free_frame(pt_phys) == SAMOS_EINVAL);           /* E4 : refusé */
    KTEST_CHECK(pmm_core_type(p, pt_phys >> PAGE_SHIFT) == FRAME_PAGETABLE);
    KTEST_CHECK(p->free_count == free_before);
    KTEST_CHECK(pt[(kva >> 12) & 0x3FFu] == pte_before);           /* contenu intact */
    KTEST_CHECK(mem_probe_read((const void *)&kmain, &v) == 0);

    /* Un cadre ordinaire reste allouable, libérable et réallouable. */
    uint32_t f = pmm_alloc_frame();
    KTEST_CHECK(f != 0 && pmm_core_type(p, f >> PAGE_SHIFT) == FRAME_ALLOCATED);
    KTEST_CHECK(pmm_free_frame(f) == SAMOS_OK && pmm_core_type(p, f >> PAGE_SHIFT) == FRAME_FREE);
    uint32_t g = pmm_alloc_frame();
    KTEST_CHECK(g != 0 && pmm_core_type(p, g >> PAGE_SHIFT) == FRAME_ALLOCATED);
    KTEST_CHECK(pmm_free_frame(g) == SAMOS_OK);
    /* E1/E2 toujours actifs après les tentatives interdites. */
    KTEST_CHECK(vmm_map(0, g, PTE_WRITE) == SAMOS_EPERM);
    KTEST_CHECK(vmm_map((uint32_t)(uintptr_t)__stack_guard_start, g, PTE_WRITE) == SAMOS_EPERM);
    KTEST_CHECK(pmm_core_check(p) == 0);
}

/* K0.7-M2.4b — Interfaces de références du PMM noyau (pas encore utilisées
 * par le VMM : E5 n'est PAS corrigé à ce stade). */
static void test_kernel_refs(void)
{
    const struct pmm *p = pmm_state();
    uint32_t f = pmm_alloc_frame();
    KTEST_CHECK(f != 0 && pmm_refs(f) == 0);
    KTEST_CHECK(pmm_ref(f) == SAMOS_OK && pmm_refs(f) == 1);
    uint32_t fc = p->free_count;
    KTEST_CHECK(pmm_free_frame(f) == SAMOS_EBUSY);
    KTEST_CHECK(pmm_core_type(p, f >> PAGE_SHIFT) == FRAME_ALLOCATED && pmm_refs(f) == 1);
    KTEST_CHECK(p->free_count == fc && pmm_core_check(p) == 0);
    KTEST_CHECK(pmm_unref(f) == SAMOS_OK && pmm_free_frame(f) == SAMOS_OK);
    KTEST_CHECK(pmm_ref(f + 4) == SAMOS_EINVAL);                         /* non aligné */
    KTEST_CHECK(pmm_ref((uint32_t)(uintptr_t)&kmain & PTE_FRAME) == SAMOS_EPERM);
    KTEST_CHECK(pmm_ref(paging_directory()) == SAMOS_EPERM);             /* E4 : table */
    KTEST_CHECK(pmm_ref(f) == SAMOS_EPERM);                              /* cadre libre */
}

/* K0.7-M2.4c — vmm_map() prend exactement une référence par projection
 * dynamique réussie d'un cadre ALLOCATED, et aucune sinon. E5 reste ouvert :
 * vmm_unmap() ne relâche pas encore la référence (M2.4d). */
static uint32_t total_refs(void)
{
    const struct pmm *p = pmm_state();
    uint32_t sum = 0;
    for (uint32_t f = 0; f < p->nframes; f++) {
        sum += pmm_core_refs(p, f);
    }
    return sum;
}

static void test_vmm_refs(void)
{
    /* 7. L'identité du démarrage n'a créé aucune référence dynamique. */
    KTEST_CHECK(total_refs() == 0);

    uint32_t f = pmm_alloc_frame();
    const uint32_t va1 = 0xD0010000u, va2 = 0xD0011000u;
    KTEST_CHECK(f != 0 && pmm_refs(f) == 0);
    /* 1. +1 exactement ; 2. deux projections distinctes => 2 */
    KTEST_CHECK(vmm_map(va1, f, PTE_WRITE) == SAMOS_OK && pmm_refs(f) == 1);
    KTEST_CHECK(vmm_map(va2, f, 0) == SAMOS_OK && pmm_refs(f) == 2);
    uint32_t pa = 0;
    KTEST_CHECK(vmm_translate(va2 + 0x10, &pa, 0) && pa == f + 0x10);
    /* 3, 4, 5. Refus sans effet sur le compteur */
    KTEST_CHECK(vmm_map(0, f, PTE_WRITE) == SAMOS_EPERM);                         /* E1 */
    KTEST_CHECK(vmm_map((uint32_t)(uintptr_t)__stack_guard_start, f, 0) == SAMOS_EPERM); /* E2 */
    KTEST_CHECK(vmm_map(va1 + 4, f, 0) == SAMOS_EINVAL);                          /* va non alignée */
    KTEST_CHECK(vmm_map(0xD0012000u, f + 4, 0) == SAMOS_EINVAL);                  /* pa non alignée */
    KTEST_CHECK(vmm_map(0xD0012000u, f, 0x1000u) == SAMOS_EINVAL);                /* droits invalides */
    KTEST_CHECK(vmm_map(va1, f, PTE_WRITE) == SAMOS_EEXIST);                      /* déjà présente */
    KTEST_CHECK(pmm_refs(f) == 2);
    uint32_t g = pmm_alloc_frame();
    KTEST_CHECK(pmm_free_frame(g) == SAMOS_OK);
    KTEST_CHECK(vmm_map(0xD0012000u, g, PTE_WRITE) == SAMOS_EPERM);               /* cadre FREE */
    KTEST_CHECK(!vmm_translate(0xD0012000u, 0, 0) && pmm_refs(g) == 0);
    KTEST_CHECK(pmm_free_frame(f) == SAMOS_EBUSY);                                /* encore projeté */
    KTEST_CHECK(total_refs() == 2);

    /* 6a. Échec de prise de référence (compteur plein) : ni PTE, ni référence. */
    uint32_t added = 0;
    while (pmm_refs(f) < FRAME_REFS_MAX && pmm_ref(f) == SAMOS_OK) {
        added++;
    }
    KTEST_CHECK(pmm_refs(f) == FRAME_REFS_MAX);
    KTEST_CHECK(vmm_map(0xD0013000u, f, 0) == SAMOS_ERANGE);
    KTEST_CHECK(!vmm_translate(0xD0013000u, 0, 0) && pmm_refs(f) == FRAME_REFS_MAX);
    while (added-- > 0) {
        (void)pmm_unref(f);
    }
    KTEST_CHECK(pmm_refs(f) == 2);

    /* 6b. Échec d'allocation de table (mémoire épuisée) : aucune référence. */
    const struct pmm *p = pmm_state();
    uint32_t first = 0, last = 0, n = 0, h;
    while ((h = pmm_alloc_frame()) != 0) {
        if (n == 0) first = h;
        last = h;
        n++;
    }
    KTEST_CHECK(vmm_map(0xE0000000u, f, 0) == SAMOS_ENOMEM);          /* pas de table pour 0xE0000000 */
    KTEST_CHECK(!vmm_translate(0xE0000000u, 0, 0) && pmm_refs(f) == 2);
    uint32_t lo = (first < last) ? first : last, hi = (first < last) ? last : first;
    for (uint32_t ph = lo; ph <= hi && n > 0; ph += PAGE_SIZE) {
        if (ph != f && pmm_core_type(p, ph >> PAGE_SHIFT) == FRAME_ALLOCATED &&
            pmm_refs(ph) == 0 && pmm_free_frame(ph) == SAMOS_OK) {
            n--;
        }
    }
    KTEST_CHECK(n == 0);

    /* Nettoyage : vmm_unmap relâche lui-même les références (M2.4d). */
    KTEST_CHECK(vmm_unmap(va1, 0) == SAMOS_OK && vmm_unmap(va2, 0) == SAMOS_OK);
    KTEST_CHECK(pmm_refs(f) == 0);
    KTEST_CHECK(pmm_free_frame(f) == SAMOS_OK && total_refs() == 0);
    KTEST_CHECK(pmm_free_frame(paging_directory()) == SAMOS_EINVAL);   /* E4 toujours refusé */
    KTEST_CHECK(pmm_core_check(p) == 0);
}

/* K0.7-M2.4d — vmm_unmap() relâche exactement une référence par retrait
 * réussi. E5 reste ouvert jusqu'à la démonstration de bout en bout (M2.4e). */
static void test_vmm_unmap_refs(void)
{
    const struct pmm *p = pmm_state();
    const uint32_t a = 0xD0020000u, b = 0xD0021000u;
    uint32_t f = pmm_alloc_frame(), out = 0, v = 0;
    KTEST_CHECK(f != 0);
    /* 1. map -> 1 -> unmap -> 0 */
    KTEST_CHECK(vmm_map(a, f, PTE_WRITE) == SAMOS_OK && pmm_refs(f) == 1);
    KTEST_CHECK(vmm_unmap(a, &out) == SAMOS_OK && out == f && pmm_refs(f) == 0);
    /* 9. traduction et TLB cohérents après suppression */
    KTEST_CHECK(!vmm_translate(a, 0, 0));
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)a, &v) == -1);
    /* 2, 3, 4, 5. deux adresses */
    KTEST_CHECK(vmm_map(a, f, PTE_WRITE) == SAMOS_OK && vmm_map(b, f, 0) == SAMOS_OK);
    KTEST_CHECK(pmm_refs(f) == 2);
    KTEST_CHECK(vmm_unmap(a, 0) == SAMOS_OK && pmm_refs(f) == 1);
    KTEST_CHECK(pmm_free_frame(f) == SAMOS_EBUSY);                 /* 4 */
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)b, &v) == 0); /* b toujours projetée */
    KTEST_CHECK(vmm_unmap(b, 0) == SAMOS_OK && pmm_refs(f) == 0);
    /* 6. double retrait : refusé, pas de sous-dépassement */
    KTEST_CHECK(vmm_unmap(b, 0) == SAMOS_ENOENT && pmm_refs(f) == 0);
    /* 7. adresses invalides : refus sans effet */
    KTEST_CHECK(vmm_map(a, f, 0) == SAMOS_OK);
    KTEST_CHECK(vmm_unmap(a + 4, 0) == SAMOS_EINVAL && pmm_refs(f) == 1);
    KTEST_CHECK(vmm_translate(a, 0, 0));                           /* projection intacte */
    KTEST_CHECK(vmm_unmap(0xD0022000u, 0) == SAMOS_ENOENT && pmm_refs(f) == 1);
    KTEST_CHECK(vmm_unmap(a, 0) == SAMOS_OK && pmm_refs(f) == 0);
    KTEST_CHECK(pmm_free_frame(f) == SAMOS_OK);                    /* 5 */
    /* 8. identité permanente jamais supprimée */
    uint32_t kpage = (uint32_t)(uintptr_t)&kmain & PTE_FRAME;
    KTEST_CHECK(vmm_unmap(kpage, 0) == SAMOS_EPERM);
    KTEST_CHECK(vmm_unmap(0xB8000u, 0) == SAMOS_EPERM);
    KTEST_CHECK(vmm_translate(kpage, 0, 0) && mem_probe_read((const void *)&kmain, &v) == 0);
    /* 10. E1, E2, E4 */
    KTEST_CHECK(vmm_unmap(0, 0) == SAMOS_EPERM);
    KTEST_CHECK(mem_probe_read((const void *)0, &v) == -1);
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)__stack_guard_start, &v) == -1);
    KTEST_CHECK(pmm_free_frame(paging_directory()) == SAMOS_EINVAL);
    KTEST_CHECK(total_refs() == 0 && pmm_core_check(p) == 0);
}

/* K0.7-M2.4e — Scénario E5 d'origine (docs/review/evidence, E5) rejoué :
 * en K0.6, pmm_free_frame() acceptait un cadre encore projeté, qui était
 * ensuite redistribué tout en restant projeté. */
static void test_e5_scenario(void)
{
    const struct pmm *p = pmm_state();
    const uint32_t va = 0xD0030000u;
    uint32_t pa = 0;
    /* 1, 2. Allouer A, le projeter, y écrire par la projection dynamique */
    uint32_t A = pmm_alloc_frame();
    KTEST_CHECK(A != 0 && vmm_map(va, A, PTE_WRITE) == SAMOS_OK);
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)va, 0xE5E5E5E5u) == 0);
    /* 3, 4. Libération pendant la projection : refusée */
    uint32_t fc = p->free_count;
    KTEST_CHECK(pmm_free_frame(A) == SAMOS_EBUSY);
    /* 5. Contenu, type, compteur, traduction intacts */
    KTEST_CHECK(*(volatile uint32_t *)(uintptr_t)A == 0xE5E5E5E5u);
    KTEST_CHECK(pmm_core_type(p, A >> PAGE_SHIFT) == FRAME_ALLOCATED && pmm_refs(A) == 1);
    KTEST_CHECK(p->free_count == fc);
    KTEST_CHECK(vmm_translate(va, &pa, 0) && pa == A);
    /* 6. Une nouvelle allocation ne réutilise pas A */
    uint32_t B = pmm_alloc_frame();
    KTEST_CHECK(B != 0 && B != A);
    KTEST_CHECK(*(volatile uint32_t *)(uintptr_t)A == 0xE5E5E5E5u);   /* A non remis à zéro */
    KTEST_CHECK(pmm_free_frame(B) == SAMOS_OK);
    /* 7, 8. Retrait de la projection : compteur à 0, plus de traduction */
    KTEST_CHECK(vmm_unmap(va, 0) == SAMOS_OK && pmm_refs(A) == 0);
    KTEST_CHECK(!vmm_translate(va, 0, 0));
    /* 9. Libération de A désormais acceptée */
    KTEST_CHECK(pmm_free_frame(A) == SAMOS_OK && pmm_core_type(p, A >> PAGE_SHIFT) == FRAME_FREE);
    /* 10. Réallocation de A (next-fit : on alloue jusqu'à le retrouver) :
     *     aucune projection résiduelle, contenu remis à zéro. */
    uint32_t got = 0, n = 0, first = 0, last = 0, h;
    while ((h = pmm_alloc_frame()) != 0) {
        if (n == 0) first = h;
        last = h;
        n++;
        if (h == A) { got = h; break; }
    }
    KTEST_CHECK(got == A);
    KTEST_CHECK(*(volatile uint32_t *)(uintptr_t)A == 0 && pmm_refs(A) == 0);
    KTEST_CHECK(!vmm_translate(va, 0, 0));
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)va, &pa) == -1);
    uint32_t lo = (first < last) ? first : last, hi = (first < last) ? last : first;
    for (uint32_t ph = lo; ph <= hi && n > 0; ph += PAGE_SIZE) {
        if (pmm_core_type(p, ph >> PAGE_SHIFT) == FRAME_ALLOCATED && pmm_refs(ph) == 0 &&
            pmm_free_frame(ph) == SAMOS_OK) {
            n--;
        }
    }
    KTEST_CHECK(n == 0);

    /* Second scénario : deux projections dynamiques vers le même cadre. */
    uint32_t C = pmm_alloc_frame();
    KTEST_CHECK(vmm_map(va, C, PTE_WRITE) == SAMOS_OK && vmm_map(va + PAGE_SIZE, C, 0) == SAMOS_OK);
    KTEST_CHECK(pmm_refs(C) == 2 && pmm_free_frame(C) == SAMOS_EBUSY);
    KTEST_CHECK(vmm_unmap(va, 0) == SAMOS_OK && pmm_refs(C) == 1);
    KTEST_CHECK(pmm_free_frame(C) == SAMOS_EBUSY);                 /* une projection subsiste */
    KTEST_CHECK(vmm_translate(va + PAGE_SIZE, 0, 0));
    KTEST_CHECK(vmm_unmap(va + PAGE_SIZE, 0) == SAMOS_OK && pmm_refs(C) == 0);
    KTEST_CHECK(pmm_free_frame(C) == SAMOS_OK);
    KTEST_CHECK(total_refs() == 0 && pmm_core_check(p) == 0);
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
    test_frame_types();
    test_pagetable_frames();
    test_kernel_refs();
    test_vmm_refs();
    test_vmm_unmap_refs();
    test_e5_scenario();
}
