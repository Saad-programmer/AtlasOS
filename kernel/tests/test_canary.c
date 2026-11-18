/*
 * SAMOS / AtlasOS — kernel/tests/test_canary.c
 * ---------------------------------------------------------------------------
 * K0.7-R1b — Autotests des canaris de pile.
 *
 * CE FICHIER EST LE SEUL compilé avec -fstack-protector-strong (Makefile,
 * image de test uniquement) : c'est le moyen de prouver toute la chaîne
 * (prologue, épilogue, __stack_chk_fail, panic) sans instrumenter le
 * démarrage. Clang 18 n'offre pas d'activation fonction par fonction
 * (l'attribut « stack_protect » lui est inconnu).
 *
 * Localisation de la copie du canari, sans supposer de disposition : la
 * fonction instrumentée parcourt SON PROPRE cadre, du début de son tableau
 * local jusqu'à l'EBP sauvegardé (exclu), et compte les mots égaux à
 * __stack_chk_guard. Clang range la copie juste sous l'EBP sauvegardé
 * (-4(%ebp)), au-dessus des tableaux : on doit en trouver exactement une.
 * Zéro signifie « fonction non instrumentée » ; plus d'une, une ambiguïté
 * (dans ce cas, on ne corrompt rien).
 */
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/paging.h>
#include <samos/kernel.h>
#include <samos/layout.h>
#include <samos/ktest.h>
#include <samos/log.h>
#include <samos/pmm.h>
#include <samos/stack_protector.h>

/*
 * Retourne le nombre de copies du canari trouvées dans le cadre courant.
 * corrupt = true : modifie UNIQUEMENT cette copie (un bit), si elle est
 * unique. Aucune autre donnée n'est écrite : ni l'EBP sauvegardé, ni
 * l'adresse de retour, qui sont AU-DESSUS de la zone parcourue. L'épilogue
 * détecte alors l'écart et appelle __stack_chk_fail : on ne revient pas.
 */
static SAMOS_NOINLINE unsigned canary_frame_probe(bool corrupt)
{
    volatile uint8_t buf[16];                  /* tableau => instrumentée (-strong) */
    for (unsigned i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)i;
    }
    volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)buf & ~(uintptr_t)3u);
    volatile uint32_t *fp = (volatile uint32_t *)__builtin_frame_address(0);
    volatile uint32_t *slot = 0;
    unsigned copies = 0;
    for (; p < fp; p++) {
        if (*p == __stack_chk_guard) {
            copies++;
            slot = p;
        }
    }
    if (corrupt && copies == 1) {
        *slot ^= 1u;                           /* corruption contrôlée et minimale */
    }
    return copies + (buf[15] == 15u ? 0u : 100u);
}

void ktest_canary_smash(void)
{
    kprintf("[TEST] corrupting the local canary copy of canary_frame_probe\n");
    unsigned copies = canary_frame_probe(true);
    /* Atteint seulement si la corruption n'a pas été détectée. */
    kprintf("[TEST] canary corruption NOT detected (copies found: %u)\n", copies);
}

/* Valeur de référence en mémoire, PAS dans une variable locale : la zone
 * parcourue par canary_frame_probe contient les registres EBX/ESI/EDI que
 * l'appelant lui confie ; si l'appelant gardait la valeur du canari dans l'un
 * d'eux, on trouverait deux copies (constaté à la première exécution). */
static volatile uint32_t g_guard_ref;

void ktest_suite_canary(void)
{
    ktest_suite("K0.7 stack canary");
    g_guard_ref = __stack_chk_guard;
    uint32_t ga = (uint32_t)(uintptr_t)&__stack_chk_guard;

    /* Valeur : écrite par _start (différente de celle de l'image), non nulle,
     * octet faible nul. (Probabilité qu'un tirage RDTSC redonne exactement
     * la valeur par défaut : 2^-24, acceptée.) */
    KTEST_CHECK(g_guard_ref != 0);
    KTEST_CHECK((g_guard_ref & 0xFFu) == 0);
    KTEST_CHECK(g_guard_ref != STACK_GUARD_DEFAULT);

    /* Appel instrumenté normal : exactement une copie dans le cadre, retour
     * normal, contenu local intact. Zéro copie = instrumentation absente. */
    KTEST_CHECK(canary_frame_probe(false) == 1);
    KTEST_CHECK(canary_frame_probe(false) == 1);
    KTEST_CHECK(__stack_chk_guard == g_guard_ref);                     /* inchangé */

    /* Emplacement : dans .rodata, page en lecture seule et superviseur. */
    KTEST_CHECK(ga >= (uint32_t)(uintptr_t)__rodata_start &&
                ga + 4u <= (uint32_t)(uintptr_t)__rodata_end);
    uint32_t pa = 0, fl = 0;
    KTEST_CHECK(vmm_translate(ga & PTE_FRAME, &pa, &fl) && pa == (ga & PTE_FRAME));
    KTEST_CHECK((fl & PTE_PRESENT) && !(fl & PTE_WRITE) && !(fl & PTE_USER));
    KTEST_CHECK((cpu_read_cr0() & CR0_WP) != 0);

    /* Écriture refusée par le MMU : faute de protection en écriture à
     * l'adresse exacte du canari, valeur inchangée. */
    uint32_t cr2 = 0, err = 0;
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)ga, g_guard_ref ^ 0xFFFFFF00u) == -1);
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(cr2 == ga && (err & 3u) == 3u);               /* P = 1, W = 1 */
    KTEST_CHECK(__stack_chk_guard == g_guard_ref);
}

/*
 * K0.7-R1d — Sentinelles de la pile de double faute (S2). Tests NON
 * destructifs : S2 n'est utilisée que par la tâche #DF, absente ici ; la
 * seule écriture (test de détection) porte sur un mot sentinelle et est
 * annulée immédiatement. Le scénario destructif est le mode test=dfsentinel.
 */
void ktest_suite_df_sentinel(void)
{
    ktest_suite("K0.7 double-fault stack sentinels");
    volatile uint32_t *w = df_stack_sentinels();
    uintptr_t lo = df_stack_bottom(), hi = df_stack_top();
    const struct tss *df = tss_double_fault();

    /* Disposition : 4 mots au bas exact de S2, S2 alignée et de 4 KiB, ESP
     * initial de la tâche #DF au sommet, TSS #DF entièrement sous la zone. */
    KTEST_CHECK((uintptr_t)w == lo && (lo & 15u) == 0 && hi - lo == 4096u);
    KTEST_CHECK(df->esp == (uint32_t)hi);
    /* La zone sentinelle ne recouvre ni les TSS ni la GDT, où qu'ils soient
     * placés (l'ordre des objets de .bss n'est pas garanti). */
    uintptr_t zlo = lo, zhi = lo + DF_SENTINEL_WORDS * 4u;
    uintptr_t tm = (uintptr_t)tss_main(), gd = (uintptr_t)gdt_table();
    KTEST_CHECK((uintptr_t)df + sizeof(struct tss) <= zlo || (uintptr_t)df >= zhi);
    KTEST_CHECK(tm + sizeof(struct tss) <= zlo || tm >= zhi);
    KTEST_CHECK(gd + GDT_ENTRIES * sizeof(struct gdt_entry) <= zlo || gd >= zhi);
    KTEST_CHECK(lo + DF_SENTINEL_WORDS * 4u < hi);

    /* Initialisation : chaque mot a sa valeur, et elles sont distinctes. */
    bool values_ok = true;
    for (uint32_t i = 0; i < DF_SENTINEL_WORDS; i++) {
        values_ok = values_ok && w[i] == DF_SENTINEL_VALUE(i);
    }
    KTEST_CHECK(values_ok);
    KTEST_CHECK(DF_SENTINEL_VALUE(0) != DF_SENTINEL_VALUE(1) &&
                DF_SENTINEL_VALUE(2) != DF_SENTINEL_VALUE(3));

    /* Intactes après toutes les suites précédentes (exceptions, fautes de
     * page attendues, IRQ, PMM/VMM : tout cela s'exécute sur S1). */
    KTEST_CHECK(df_stack_sentinel_damage() == 0);

    /* Détection : un mot altéré est signalé par son bit, puis restauré. */
    uint32_t saved = w[1];
    w[1] = saved ^ 0x00010000u;
    uint32_t mask = df_stack_sentinel_damage();
    w[1] = saved;
    KTEST_CHECK(mask == (1u << 1));
    KTEST_CHECK(df_stack_sentinel_damage() == 0);

    /* S1 inchangée : page de garde toujours non projetée. */
    KTEST_CHECK(boot_stack_guard_intact());
}

/*
 * K0.7-R1f-1 — Disposition explicite des piles (section .stacks, linker.ld).
 * Les deux pages réservées aux futures gardes sont ENCORE projetées dans ce
 * jalon ; seule la garde basse de S1 est non présente (depuis K0.6).
 */
static bool overlaps(uintptr_t a, uintptr_t alen, uintptr_t b, uintptr_t blen)
{
    return a < b + blen && b < a + alen;
}

void ktest_suite_stack_layout(void)
{
    ktest_suite("K0.7 stack layout");
    const uintptr_t pg = 4096u;
    uintptr_t dg = (uintptr_t)__df_guard_start, s2 = (uintptr_t)__df_stack_start;
    uintptr_t g1 = (uintptr_t)__stack_guard_start, s1 = (uintptr_t)boot_stack_bottom;
    uintptr_t tg = (uintptr_t)__stack_top_guard_start;

    /* Bornes, tailles et contiguïté (doublent les assertions du linker). */
    KTEST_CHECK((uintptr_t)__stacks_start == dg && (dg & (pg - 1u)) == 0);
    KTEST_CHECK((uintptr_t)__df_guard_end == s2 && s2 - dg == pg);
    KTEST_CHECK((uintptr_t)__df_stack_end == g1 && g1 - s2 == 4096u);
    KTEST_CHECK((uintptr_t)__stack_guard_end == s1 && s1 - g1 == pg);
    KTEST_CHECK((uintptr_t)boot_stack_top == tg && tg - s1 == 16384u);
    KTEST_CHECK((uintptr_t)__stack_top_guard_end == tg + pg);
    KTEST_CHECK((uintptr_t)__stacks_end == tg + pg && (uintptr_t)__kernel_end == tg + pg);
    KTEST_CHECK((s2 & (pg - 1u)) == 0 && (s1 & (pg - 1u)) == 0);

    /* Cohérence symboles / adresses utilisées par le code. */
    KTEST_CHECK((uintptr_t)df_stack_area == s2);
    KTEST_CHECK(df_stack_bottom() == s2 && df_stack_top() == g1);
    KTEST_CHECK((uintptr_t)df_stack_sentinels() == s2);
    KTEST_CHECK(tss_double_fault()->esp == (uint32_t)g1);          /* sommet de S2 */
    KTEST_CHECK(tss_main()->esp0 == (uint32_t)tg);                 /* sommet de S1 */
    uint32_t esp = cpu_read_esp();
    KTEST_CHECK(esp > (uint32_t)s1 && esp < (uint32_t)tg);         /* on s'exécute sur S1 */
    KTEST_CHECK(df_stack_sentinel_damage() == 0);

    /* Droits effectifs : garde S2 réservée, encore projetée (R1f-3) ; gardes
     * basse et haute (R1f-2) de S1 non présentes. */
    uint32_t pa = 0, fl = 0;
    KTEST_CHECK(vmm_translate(dg, &pa, &fl) && pa == dg && (fl & PTE_WRITE));
    KTEST_CHECK(!vmm_translate(tg, 0, 0));
    KTEST_CHECK(!vmm_translate(g1, 0, 0));
    KTEST_CHECK(vmm_translate(s2, 0, 0) && vmm_translate(s1, 0, 0));

    /* PMM : cadres de l'image, jamais distribués ni projetables dynamiquement. */
    const struct pmm *p = pmm_state();
    KTEST_CHECK(pmm_core_type(p, dg >> 12) == FRAME_KERNEL && pmm_core_type(p, tg >> 12) == FRAME_KERNEL);
    KTEST_CHECK(pmm_core_type(p, s2 >> 12) == FRAME_KERNEL && pmm_core_type(p, g1 >> 12) == FRAME_KERNEL);
    KTEST_CHECK(pmm_free_frame((uint32_t)dg) == SAMOS_EINVAL && pmm_free_frame((uint32_t)tg) == SAMOS_EINVAL);
    KTEST_CHECK(vmm_map(0xD0070000u, (uint32_t)dg, PTE_WRITE) == SAMOS_EPERM);
    KTEST_CHECK(vmm_map(0xD0070000u, (uint32_t)tg, PTE_WRITE) == SAMOS_EPERM);

    /* Aucune structure critique dans une page réservée ; répertoire de pages
     * juste après l'image (premier cadre géré). */
    uintptr_t crit[][2] = {
        { (uintptr_t)gdt_table(), GDT_ENTRIES * sizeof(struct gdt_entry) },
        { (uintptr_t)tss_main(), sizeof(struct tss) },
        { (uintptr_t)tss_double_fault(), sizeof(struct tss) },
        { (uintptr_t)idt_table(), 256u * sizeof(struct idt_entry) },
        { (uintptr_t)p->meta, p->nframes * sizeof(frame_desc_t) },
        { (uintptr_t)paging_directory(), pg },
    };
    bool clear = true;
    for (unsigned i = 0; i < sizeof(crit) / sizeof(crit[0]); i++) {
        clear = clear && !overlaps(crit[i][0], crit[i][1], dg, pg) && !overlaps(crit[i][0], crit[i][1], tg, pg);
    }
    KTEST_CHECK(clear);
    KTEST_CHECK(paging_directory() == (uint32_t)(uintptr_t)__kernel_end);
}

/*
 * K0.7-R1f-2 — Garde haute de S1 : [__stack_top_guard_start, +4 KiB).
 * Preuves sans destruction : PTE nulle (lue dans la table), traduction
 * absente, sondes en faute sur le premier et le dernier mot (CR2 exact, code
 * d'erreur « non présente, écriture »), refus de vmm_map sans effet de bord,
 * voisins intacts. Le scénario destructif est test=topguard / topguard_end.
 */
static uint32_t raw_pte(uint32_t va)
{
    const volatile uint32_t *pd = (const volatile uint32_t *)(uintptr_t)paging_directory();
    uint32_t pde = pd[va >> 22];
    if (!(pde & PTE_PRESENT)) {
        return 0;
    }
    return ((const volatile uint32_t *)(uintptr_t)(pde & PTE_FRAME))[(va >> 12) & 0x3FFu];
}

void ktest_suite_top_guard(void)
{
    ktest_suite("K0.7 S1 top guard");
    uint32_t tg = (uint32_t)(uintptr_t)__stack_top_guard_start;
    uint32_t below = tg - 4096u, above = tg + 4096u;       /* dernière page de S1, CR3 */
    const struct pmm *p = pmm_state();

    /* 1-3. Non présente : PTE nulle, aucune traduction. */
    KTEST_CHECK(tg == (uint32_t)(uintptr_t)boot_stack_top && (tg & 0xFFFu) == 0);
    KTEST_CHECK(raw_pte(tg) == 0);
    KTEST_CHECK(!vmm_translate(tg, 0, 0) && !vmm_translate(tg + 4092u, 0, 0));
    /* 4. CR3 juste au-dessus, toujours projeté et inscriptible ; dernière page
     * de S1 projetée en écriture. */
    uint32_t pa = 0, fl = 0;
    KTEST_CHECK(paging_directory() == above);
    KTEST_CHECK(vmm_translate(above, &pa, &fl) && pa == above && (fl & PTE_WRITE));
    KTEST_CHECK(vmm_translate(below, &pa, &fl) && pa == below && (fl & PTE_WRITE));
    uint32_t pte_below = raw_pte(below), pte_above = raw_pte(above);

    /* Sondes : premier et dernier mot. Code d'erreur 2 = P=0, W=1, U=0. */
    uint32_t cr2 = 0, err = 0, v = 0;
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)tg, 0xA5A5A5A5u) == -1);
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(cr2 == tg && err == 2u);
    KTEST_CHECK(mem_probe_write((void *)(uintptr_t)(tg + 4092u), 0xA5A5A5A5u) == -1);
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(cr2 == tg + 4092u && err == 2u);
    KTEST_CHECK(mem_probe_read((const void *)(uintptr_t)tg, &v) == -1);
    mem_probe_last_fault(&cr2, &err);
    KTEST_CHECK(cr2 == tg && err == 0u);

    /* 5-6. vmm_map refusé (EPERM) avec un cadre ALLOCATED valide, en écriture
     * et en lecture seule : ni projection, ni référence, ni table, ni cadre
     * consommé ; voisins inchangés. La règle précède la recherche de table. */
    uint32_t f = pmm_alloc_frame();
    uint32_t tables0 = paging_table_count(), free0 = p->free_count;
    KTEST_CHECK(f != 0);
    KTEST_CHECK(vmm_map(tg, f, PTE_WRITE) == SAMOS_EPERM);
    KTEST_CHECK(vmm_map(tg, f, 0) == SAMOS_EPERM);
    KTEST_CHECK(vmm_map((tg + 4092u) & ~0xFFFu, f, 0) == SAMOS_EPERM);
    KTEST_CHECK(pmm_refs(f) == 0 && !vmm_translate(tg, 0, 0) && raw_pte(tg) == 0);
    KTEST_CHECK(paging_table_count() == tables0 && p->free_count == free0);
    KTEST_CHECK(raw_pte(below) == pte_below && raw_pte(above) == pte_above);
    KTEST_CHECK(vmm_unmap(tg, 0) == SAMOS_EPERM);                   /* identité : jamais retirée */
    KTEST_CHECK(pmm_free_frame(f) == SAMOS_OK);

    /* 7-8. Garde basse de S1 toujours absente ; garde S2 encore présente. */
    KTEST_CHECK(raw_pte((uint32_t)(uintptr_t)__stack_guard_start) == 0);
    KTEST_CHECK(raw_pte((uint32_t)(uintptr_t)__df_guard_start) != 0);
    KTEST_CHECK(pmm_core_check(p) == 0);
}

/* Mode destructif test=topguard (premier mot) / topguard_end (dernier mot) :
 * écriture DIRECTE (pas une sonde), depuis ce cadre sur S1 valide. Le
 * gestionnaire de #PF s'exécute sur S1 et appelle panic. */
void ktest_top_guard_write(bool last_word)
{
    uint32_t target = (uint32_t)(uintptr_t)__stack_top_guard_start + (last_word ? 4092u : 0u);
    kprintf("[TEST] writing to the S1 top guard at %p\n", (void *)(uintptr_t)target);
    *(volatile uint32_t *)(uintptr_t)target = 0x70F6A7D0u;
    kprintf("[TEST] top guard write NOT trapped\n");
}
