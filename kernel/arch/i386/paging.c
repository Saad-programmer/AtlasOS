/*
 * SAMOS / AtlasOS — kernel/arch/i386/paging.c
 * Pagination 32 bits et gestionnaire de fautes de page. Voir paging.h et
 * Details.md §35-36.
 *
 * Toutes les tables sont accédées par leur adresse physique, valable aussi
 * après activation car la RAM gérée est projetée en identité (va == pa) :
 * l'allocateur physique ne distribue que des cadres situés sous idmap_top.
 */
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/interrupt.h>
#include <arch/paging.h>
#include <samos/kernel.h>
#include <samos/log.h>
#include <samos/panic.h>
#include <samos/pmm.h>

#define PD_INDEX(va) ((va) >> 22)
#define PT_INDEX(va) (((va) >> 12) & 0x3FFu)

extern char mem_probe_read_insn[];
extern char mem_probe_write_insn[];
extern char mem_probe_fixup[];

/* K0.7-P4b — Racines du VMM écrites UNE fois (paging_init) : page dédiée
 * (.bss.p4_vmm), candidate à la lecture seule (P4d). g_tables, incrémenté à
 * chaque nouvelle table, reste dans .bss. */
static uint32_t *g_pd SAMOS_SECTION(".bss.p4_vmm");   /* répertoire de pages (physique = virtuel) */
static uint32_t g_tables;
static uint32_t g_idmap_top SAMOS_SECTION(".bss.p4_vmm");
static volatile uint32_t g_probe_cr2;
static volatile uint32_t g_probe_err;

static bool paging_enabled(void)
{
    return (cpu_read_cr0() & CR0_PG) != 0;
}

/* K0.7-P4e-1 — Passage d'écriture des PDE/PTE (défini plus bas). */
enum pt_kind { PT_KIND_PDE, PT_KIND_PTE };
static samos_err_t pt_gate_write(enum pt_kind kind, uint32_t va, uint32_t value);
static SAMOS_NOINLINE samos_err_t seal_table_frame(uint32_t table);
static bool frame_has_foreign_alias(uint32_t frame);
static bool frame_is_paging_structure(uint32_t frame);

/* Table de pages couvrant va, ou 0 si le PDE est absent. Lecture seule : les
 * tables publiées sont en lecture seule dans l'identité depuis K0.7-P4e-2. */
static uint32_t *get_table(uint32_t va)
{
    uint32_t pde = g_pd[PD_INDEX(va)];
    if (pde & PTE_PRESENT) {
        return (uint32_t *)(uintptr_t)(pde & PTE_FRAME);
    }
    return 0;
}

/*
 * Création et PUBLICATION d'une table pour la région de 4 MiB de va.
 * Cycle de vie (K0.7-P4e-2, docs/design/K0.7-P4e2-page-table-sealing.md §5) :
 *   1. allocation typée FRAME_PAGETABLE (pmm_alloc_pagetable : type fixé
 *      avant que le cadre ne quitte le PMM ; jamais libérable) ;
 *   2. remise à zéro par le PMM, à travers l'identité ENCORE inscriptible
 *      du cadre (cadre libre jusque-là : aucune traduction n'en dépend) ;
 *   3. sous pagination : SCELLEMENT de l'alias d'identité (W retiré par le
 *      passage, puis INVLPG : la remise à zéro a pu laisser une traduction
 *      inscriptible dans le TLB) ;
 *   4. seulement ensuite, publication du PDE par le passage.
 * Une table publiée n'est donc jamais inscriptible. Avant l'activation de
 * la pagination (paging_init), l'étape 3 est faite en bloc par
 * seal_paging_structures, juste après PG | WP.
 * Refus du passage (incohérence interne : le cadre vient d'être validé) :
 * l'erreur est renvoyée ; le cadre reste PAGETABLE, non publié (fuite d'un
 * cadre, jamais réutilisé) : refus fermé, plutôt qu'un cadre de table rendu
 * au PMM avec un alias dans un état incertain.
 */
static samos_err_t table_create(uint32_t va, uint32_t **out)
{
    uint32_t pt = pmm_alloc_pagetable();
    if (pt == 0) {
        return SAMOS_ENOMEM;
    }
    /* K0.7-P4f-B1 — Un cadre qui a DÉJÀ un alias hors identité ne devient
     * jamais une table : sous un PMM corrompu, un cadre projeté peut être
     * redistribué (D1, V2). Le cadre reste PAGETABLE, non publié et RW
     * (traitement des échecs internes : D4, non corrigé ici). */
    if (paging_enabled() && frame_has_foreign_alias(pt)) {
        return SAMOS_EPERM;
    }
    /* K0.7-P4f-B2 (D3) — Le cadre ne doit pas être déjà une structure active
     * (PMM falsifié distribuant une table publiée). En pratique la remise à
     * zéro par le PMM aurait déjà fauté (identité scellée) : ce contrôle
     * couvre l'appelant ; le passage le refait. */
    if (frame_is_paging_structure(pt)) {
        return SAMOS_EPERM;
    }
    if (paging_enabled()) {
        samos_err_t se = seal_table_frame(pt);
        if (se != SAMOS_OK) {
            return se;
        }
    }
    /* PDE permissive (P | W) : les droits effectifs sont l'INTERSECTION des
     * droits du PDE et du PTE (SDM vol. 3A §4.6) ; c'est le PTE qui décide. */
    samos_err_t e = pt_gate_write(PT_KIND_PDE, va, pt | PTE_PRESENT | PTE_WRITE);
    if (e != SAMOS_OK) {
        return e;
    }
    g_tables++;
    *out = (uint32_t *)(uintptr_t)pt;
    return SAMOS_OK;
}

/*
 * K0.7-M1 — Pages virtuelles qui doivent rester NON projetées en permanence :
 *   - la page nulle [0, 4 KiB) : tout déréférencement de NULL doit fauter (E1) ;
 *   - la page de garde sous la pile de démarrage : un débordement de pile doit
 *     fauter (E2).
 * Le refus est imposé ici, dans l'API, et non laissé aux appelants : en K0.6,
 * vmm_map(0, ...) et vmm_map(garde, ...) étaient acceptés (preuve :
 * docs/review/evidence/k06-memory-defects.txt, E1 et E2).
 */
static bool in_page_range(uint32_t page, const char *lo, const char *hi)
{
    return page >= (uint32_t)(uintptr_t)lo && page < (uint32_t)(uintptr_t)hi;
}

/* K0.7-R1f-2 — Ajout de la garde HAUTE de S1, [__stack_top_guard_start,
 * __stack_top_guard_end) (linker.ld, section .stacks) : page réservée juste
 * au-dessus de boot_stack_top. Même règle que la garde basse : jamais
 * projetée par paging_init (boucle d'identité ci-dessous), et toute
 * projection dynamique refusée (EPERM) AVANT la recherche de table, donc
 * indépendamment de l'état actuel de la PTE. Depuis R1f-3, la garde basse
 * de S2 (__df_guard_*) suit la même règle. */
static bool va_must_stay_unmapped(uint32_t va)
{
    uint32_t page = va & PTE_FRAME;
    if (page == 0) {
        return true;
    }
    if (in_page_range(page, __stack_guard_start, __stack_guard_end)) {
        return true;                           /* garde basse de S1 (E2) */
    }
    /* K0.7-R1f-3 — Garde basse de S2, [__df_guard_start, __df_guard_end) :
     * page juste sous la pile de la tâche #DF. Un débordement de S2 y fait une
     * #PF que le CPU ne peut pas livrer (il empilerait sur S2, dans la garde),
     * puis une #DF vers une tâche déjà occupée : triple fault, mais aucune
     * écriture sous S2 (mesuré sur le prototype R1e, §5.2). */
    if (in_page_range(page, __df_guard_start, __df_guard_end)) {
        return true;
    }
    return in_page_range(page, __stack_top_guard_start, __stack_top_guard_end);
}

/*
 * K0.7-P4e-1 — PASSAGE D'ÉCRITURE DES PDE/PTE (seule désactivation de CR0.WP
 * autorisée en production ; aucune API générale). Conception :
 * docs/design/K0.7-P4e1-vmm-write-gate.md (P4c pour l'analyse de WP).
 *
 * La CIBLE n'est jamais un pointeur fourni par l'appelant : elle est
 * calculée depuis (kind, va) — PDE : g_pd[va >> 22] ; PTE : entrée va de la
 * table désignée par cette PDE. Validation COMPLÈTE avant toute écriture :
 *   - pas de fenêtre en cours (EBUSY), va alignée (EINVAL) ;
 *   - PDE : répertoire actif (g_pd = CR3 sous pagination), type PAGETABLE ;
 *     valeur = table PAGETABLE distincte du répertoire, drapeaux EXACTEMENT
 *     P | W (EPERM) ;
 *   - PTE : table présente (ENOENT), de type PAGETABLE et distincte du
 *     répertoire (EPERM) ; drapeaux parmi P, W, A, D seulement (EINVAL :
 *     jamais U, PWT, PCD, PAT, G, bits logiciels) ; si présente : page non
 *     interdite (nulle, gardes) ; en identité (va < g_idmap_top) : cadre = va,
 *     et sous pagination jamais effacée, et jamais W sur .text/.rodata ni
 *     sur une page scellée (P4d) ; hors identité : cadre ALLOCATED (M2.5b).
 * Écriture :
 *   - pagination inactive (paging_init) : écriture directe, CR0 non touché
 *     (WP n'a d'effet qu'avec PG = 1) ;
 *   - pagination active : CR0.WP doit être actif (EPERM sinon) ; IRQ
 *     masquées (irq_save) ; WP = 0 ; UN stockage de 32 bits ; WP rétabli
 *     AVANT la restauration exacte de IF. Aucun appel dans la fenêtre.
 * Hypothèses et limites (P4c §1) : cli ne bloque ni NMI, ni #MC, ni les
 * exceptions synchrones ; une NMI/#MC dans la fenêtre s'exécuterait avec
 * WP = 0, puis panic. Monoprocesseur. Le passage ne gère PAS les références
 * PMM (map_page / vmm_unmap les gèrent) ni l'INVLPG (fait par l'appelant).
 * SAMOS_NOINLINE : les écritures de CR0 restent dans CETTE fonction
 * (vérifié par check_elf.sh --production).
 */
static volatile uint32_t g_pt_gate_active;
static uint32_t g_pt_gate_windows;        /* fenêtres WP ouvertes (pagination active) */
static uint32_t g_pt_gate_refusals;

#define PT_GATE_PTE_FLAGS (PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY)

static bool va_read_only_identity(uint32_t page)
{
    return in_page_range(page, __text_start, __data_start) ||
           in_page_range(page, __kro_idt_start, __kro_idt_end) ||
           in_page_range(page, __kro_handlers_start, __kro_handlers_end) ||
           in_page_range(page, __kro_vmm_start, __kro_vmm_end);
}

/* K0.7-P4e-2 — Cadre de pagination (répertoire ou table) : critère par TYPE
 * PMM, non par plage (les tables sont allouées dynamiquement). Un cadre
 * PAGETABLE ne change jamais de type (ni libération, ni réservation : M2.3). */
static bool frame_is_pagetable(uint32_t frame)
{
    const struct pmm *p = pmm_state();
    return (frame >> PAGE_SHIFT) < p->nframes &&
           pmm_core_type(p, frame >> PAGE_SHIFT) == FRAME_PAGETABLE;
}

/*
 * K0.7-P4f-B1 — Frontière de confiance du PMM (défaut D1 de l'audit P4f-A).
 * Les types PMM (meta[], données RW) ne suffisent plus à autoriser une
 * projection : une seule écriture de 16 bits qui retypait une table en
 * ALLOCATED permettait à vmm_map d'en créer un alias inscriptible
 * (docs/review/K0.7-P4f-B1-pmm-trust-boundary.md).
 * Les deux critères ci-dessous n'utilisent PAS le PMM :
 *   - structure de pagination = le répertoire actif (g_pd, page kro_vmm RO)
 *     ou la cible d'un PDE présent (répertoire RO depuis P4e-2) ; ces
 *     sources ne sont modifiables que par la fenêtre du passage ;
 *   - image du noyau = [__kernel_start, __kernel_end) (symboles de l'éditeur
 *     de liens, constants) : .text, .rodata, IDT, gestionnaires, racines du
 *     VMM, GDT/TSS, piles.
 * Aucun de ces cadres n'a d'autre alias légitime que son identité exacte.
 * Coût : parcours des 1024 PDE (lecture seule) par vérification.
 */
static bool frame_is_paging_structure(uint32_t frame)
{
    if (frame == (uint32_t)(uintptr_t)g_pd) {
        return true;
    }
    for (uint32_t i = 0; i < 1024u; i++) {
        uint32_t pde = g_pd[i];
        if ((pde & PTE_PRESENT) && (pde & PTE_FRAME) == frame) {
            return true;
        }
    }
    return false;
}

static bool frame_in_kernel_image(uint32_t frame)
{
    return frame >= ((uint32_t)(uintptr_t)__kernel_start & PTE_FRAME) &&
           frame < (uint32_t)(uintptr_t)__kernel_end;
}

/* K0.7-P4f-B2 — Identité d'un cadre présente, exacte et SANS W (scellée).
 * Critère d'adresse et de drapeaux, sans le PMM. */
static bool table_identity_sealed(uint32_t frame)
{
    if (frame >= g_idmap_top) {
        return false;
    }
    uint32_t pde = g_pd[PD_INDEX(frame)];
    if (!(pde & PTE_PRESENT)) {
        return false;
    }
    uint32_t pte = ((const volatile uint32_t *)(uintptr_t)(pde & PTE_FRAME))[PT_INDEX(frame)];
    return (pte & PTE_PRESENT) && (pte & PTE_FRAME) == frame && !(pte & PTE_WRITE);
}

/* Cadre qui ne doit JAMAIS être projeté ailleurs que dans son identité. */
static bool frame_alias_forbidden(uint32_t frame)
{
    return frame_in_kernel_image(frame) || frame_is_paging_structure(frame);
}

/*
 * Alias hors identité existant d'un cadre : PTE présente, à va >= g_idmap_top,
 * désignant `frame`. Dans l'identité, le passage impose frame == va (critère
 * d'adresse, indépendant du PMM) : seul l'au-delà de g_idmap_top peut porter
 * un alias. Parcours limité aux PDE couvrant [g_idmap_top, 4 GiB).
 * Utilisé avant de sceller et de publier une table neuve : le PMM, s'il est
 * corrompu, peut distribuer comme table un cadre déjà projeté ailleurs (D1,
 * variante V2).
 */
static bool frame_has_foreign_alias(uint32_t frame)
{
    for (uint32_t i = PD_INDEX(g_idmap_top); i < 1024u; i++) {
        uint32_t pde = g_pd[i];
        if (!(pde & PTE_PRESENT)) {
            continue;
        }
        const volatile uint32_t *pt = (const volatile uint32_t *)(uintptr_t)(pde & PTE_FRAME);
        for (uint32_t j = 0; j < 1024u; j++) {
            uint32_t va = (i << 22) | (j << 12);
            uint32_t pte = pt[j];
            if (va >= g_idmap_top && (pte & PTE_PRESENT) && (pte & PTE_FRAME) == frame) {
                return true;
            }
        }
    }
    return false;
}

static samos_err_t pt_gate_check(enum pt_kind kind, uint32_t va, uint32_t value,
                                 volatile uint32_t **entry)
{
    const struct pmm *p = pmm_state();
    uint32_t pd = (uint32_t)(uintptr_t)g_pd;
    if (g_pt_gate_active) {
        return SAMOS_EBUSY;
    }
    if ((va & 0xFFFu) != 0 || pd == 0) {
        return SAMOS_EINVAL;
    }
    if (pmm_core_type(p, pd >> PAGE_SHIFT) != FRAME_PAGETABLE ||
        (paging_enabled() && (cpu_read_cr3() & PTE_FRAME) != pd)) {
        return SAMOS_EPERM;                     /* répertoire inattendu */
    }
    if (value & PTE_USER) {
        return SAMOS_EPERM;                     /* jamais de page utilisateur */
    }
    if (kind == PT_KIND_PDE) {
        uint32_t t = value & PTE_FRAME;
        if ((value & 0xFFFu) != (PTE_PRESENT | PTE_WRITE) || t == pd ||
            (t >> PAGE_SHIFT) >= p->nframes || pmm_core_type(p, t >> PAGE_SHIFT) != FRAME_PAGETABLE) {
            return SAMOS_EPERM;
        }
        /* K0.7-P4f-B2 (D3) — Intégrité du répertoire, contrôles SANS le PMM.
         * Une PDE ne connaît qu'une transition : absente -> présente.
         *   - PDE déjà présente : jamais remplacée, jamais effacée, pas même
         *     par une valeur identique (EEXIST) ; aucun appelant n'en a besoin
         *     (paging_init et table_create n'écrivent que des PDE absentes) ;
         *   - cible déjà structure de pagination (répertoire ou cible d'une
         *     autre PDE) : refus (EPERM), jamais deux PDE vers une table ;
         *   - sous pagination, la cible doit être SCELLÉE (identité sans W)
         *     et sans alias hors identité : une table n'est jamais publiée
         *     inscriptible. Avant PG (paging_init), l'identité n'existe pas
         *     encore et aucun alias n'est possible : le scellement suit en
         *     bloc (seal_paging_structures).
         * Conséquence : l'ensemble des cibles de PDE ne fait que croître, sans
         * doublon ni retrait ; frame_is_paging_structure (P4f-B1) reste vrai
         * pour toute table une fois publiée. */
        if (g_pd[PD_INDEX(va)] & PTE_PRESENT) {
            return SAMOS_EEXIST;
        }
        if (frame_is_paging_structure(t)) {
            return SAMOS_EPERM;
        }
        if (paging_enabled() && (frame_has_foreign_alias(t) || !table_identity_sealed(t))) {
            return SAMOS_EPERM;
        }
        *entry = &g_pd[PD_INDEX(va)];
        return SAMOS_OK;
    }
    uint32_t pde = g_pd[PD_INDEX(va)];
    if (!(pde & PTE_PRESENT)) {
        return SAMOS_ENOENT;
    }
    uint32_t table = pde & PTE_FRAME;
    if (table == pd || pmm_core_type(p, table >> PAGE_SHIFT) != FRAME_PAGETABLE) {
        return SAMOS_EPERM;
    }
    if ((value & 0xFFFu & ~PT_GATE_PTE_FLAGS) != 0) {
        return SAMOS_EINVAL;
    }
    bool identity = va < g_idmap_top;
    if (value & PTE_PRESENT) {
        uint32_t frame = value & PTE_FRAME;
        if (va_must_stay_unmapped(va)) {
            return SAMOS_EPERM;                 /* page nulle, gardes */
        }
        if (identity) {
            if (frame != va || (paging_enabled() && (value & PTE_WRITE) && va_read_only_identity(va))) {
                return SAMOS_EPERM;             /* identité exacte ; jamais W sur une page RO */
            }
            if (paging_enabled() && (value & PTE_WRITE) &&
                (frame_is_pagetable(frame) || frame_is_paging_structure(frame))) {
                return SAMOS_EPERM;             /* P4e-2, P4f-B1 : jamais W sur un cadre de pagination */
            }
        } else if (frame_alias_forbidden(frame)) {
            return SAMOS_EPERM;                 /* P4f-B1 : critère indépendant du PMM */
        } else if ((frame >> PAGE_SHIFT) >= p->nframes ||
                   pmm_core_type(p, frame >> PAGE_SHIFT) != FRAME_ALLOCATED) {
            return SAMOS_EPERM;                 /* M2.5b : seul ALLOCATED est projetable */
        }
    } else if (identity && paging_enabled()) {
        return SAMOS_EPERM;                     /* identité permanente : jamais effacée */
    }
    *entry = &((volatile uint32_t *)(uintptr_t)table)[PT_INDEX(va)];
    return SAMOS_OK;
}

static SAMOS_NOINLINE samos_err_t pt_gate_write(enum pt_kind kind, uint32_t va, uint32_t value)
{
    volatile uint32_t *entry = 0;
    samos_err_t e = pt_gate_check(kind, va, value, &entry);
    if (e != SAMOS_OK) {
        g_pt_gate_refusals++;
        return e;
    }
    if (!paging_enabled()) {
        *entry = value;                         /* démarrage : WP sans effet (PG = 0) */
        return SAMOS_OK;
    }
    uint32_t cr0 = cpu_read_cr0();
    if (!(cr0 & CR0_WP)) {
        g_pt_gate_refusals++;
        return SAMOS_EPERM;                     /* protection déjà absente : état anormal */
    }
    uint32_t flags = irq_save();
    g_pt_gate_active = 1;
    cpu_write_cr0(cr0 & ~CR0_WP);
    *entry = value;                             /* UNE écriture de 32 bits */
    cpu_write_cr0(cr0);                         /* WP rétabli AVANT IF */
    g_pt_gate_active = 0;
    irq_restore(flags);
    g_pt_gate_windows++;
    return SAMOS_OK;
}

void paging_gate_stats(uint32_t *windows, uint32_t *refusals)
{
    *windows = g_pt_gate_windows;
    *refusals = g_pt_gate_refusals;
}

/*
 * K0.7-P4e-2 — Scellement de l'alias d'identité d'un cadre de pagination :
 * W retiré de la PTE d'identité du cadre (seul alias : vmm_map et le passage
 * refusent toute autre projection d'un cadre PAGETABLE), puis INVLPG.
 * L'INVLPG est INDISPENSABLE : la remise à zéro d'une table neuve, ou une
 * lecture de la table par le passage, a pu charger dans le TLB une
 * traduction inscriptible de cette page ; sans invalidation, une écriture
 * directe réussirait encore (SDM vol. 3A §4.10.4.1 : modifier une PTE
 * n'invalide pas le TLB).
 * Les accès du MMU aux structures de pagination (parcours, bits A/D) se
 * font par adresse PHYSIQUE et ne sont pas soumis aux droits de cet alias
 * (SDM vol. 3A §4.8) : sceller l'alias ne gêne pas la traduction.
 * Les écritures logicielles ultérieures des entrées passent par la fenêtre
 * du passage (WP = 0), qui reste possible sur une page sans W.
 * Idempotente (une table déjà scellée est réécrite sans W : sans effet).
 * SAMOS_NOINLINE : la présence de l'INVLPG après l'écriture est vérifiée sur
 * le code machine (check_elf.sh) ; sous QEMU, le vidage du TLB à chaque
 * changement de CR0.WP masquerait son absence à l'exécution.
 */
static SAMOS_NOINLINE samos_err_t seal_table_frame(uint32_t table)
{
    if (!paging_enabled() || (table & 0xFFFu) != 0 || table >= g_idmap_top ||
        !frame_is_pagetable(table)) {
        return SAMOS_EINVAL;
    }
    uint32_t *alias = get_table(table);
    if (alias == 0) {
        return SAMOS_EPERM;                     /* l'identité est complète : incohérence */
    }
    uint32_t pte = alias[PT_INDEX(table)];
    if (!(pte & PTE_PRESENT) || (pte & PTE_FRAME) != table) {
        return SAMOS_EPERM;                     /* alias d'identité absent ou inexact */
    }
    samos_err_t e = pt_gate_write(PT_KIND_PTE, table, pte & ~PTE_WRITE);
    if (e != SAMOS_OK) {
        return e;
    }
    cpu_invlpg((const void *)(uintptr_t)table);
    return SAMOS_OK;
}

/*
 * K0.7-P4e-2 — Scellement en bloc des structures de pagination existantes,
 * appelé par paging_init juste après PG | WP (avant : WP sans effet, et le
 * passage écrit directement). Ordre (justification : design §3) :
 *   1. toutes les tables publiées, SAUF T0 = la table qui contient les PTE
 *      d'identité des cadres de pagination modifiés ici (celle du
 *      répertoire), par index de PDE croissant ;
 *   2. T0 elle-même : chaque étape précédente a écrit dans T0 ; la sceller
 *      après eux laisse le moins d'écritures possible sur une T0 déjà RO
 *      (elles restent possibles par la fenêtre, mais l'ordre réduit les
 *      dépendances à observer en cas d'échec) ;
 *   3. le répertoire, en dernier : aucun PDE n'est écrit pendant ce
 *      scellement ; tant qu'il reste RW, une erreur des étapes 1-2 laisse
 *      la racine de la traduction dans l'état connu de paging_init.
 * Toute erreur est renvoyée : paging_init échoue, kmain panique (refus
 * fermé). Une table n'est jamais exposée : les tables publiées étaient
 * inscriptibles AVANT ce point, comme toute la RAM (aucun code non fiable
 * ne s'exécute avant), et ne le sont plus APRÈS.
 */
static samos_err_t seal_paging_structures(void)
{
    uint32_t pd = (uint32_t)(uintptr_t)g_pd;
    uint32_t t0 = g_pd[PD_INDEX(pd)] & PTE_FRAME;
    for (uint32_t i = 0; i < 1024u; i++) {
        uint32_t pde = g_pd[i];
        if (!(pde & PTE_PRESENT) || (pde & PTE_FRAME) == t0) {
            continue;
        }
        samos_err_t e = seal_table_frame(pde & PTE_FRAME);   /* 1. tables */
        if (e != SAMOS_OK) {
            return e;
        }
    }
    samos_err_t e = seal_table_frame(t0);                     /* 2. T0 */
    if (e != SAMOS_OK) {
        return e;
    }
    e = seal_table_frame(pd);                                 /* 3. répertoire */
    return e;
}

/*
 * K0.7-P4e-2 — Audit structurel des structures de pagination (production et
 * test). Parcourt le répertoire, chaque table publiée et le PMM :
 *   writable   : cadres de pagination dont l'alias d'identité porte W ;
 *   aliases    : PTE présentes désignant un cadre de pagination ailleurs que
 *                dans son identité exacte (alias supplémentaire) ;
 *   bad_pdes   : PDE présents dont les drapeaux ne sont pas P | W (+ A), ou
 *                qui désignent un cadre non PAGETABLE ou le répertoire ;
 *   unpublished: cadres PAGETABLE ni répertoire ni table publiée.
 * Coût : 1024 PDE + 1024 PTE par table + un parcours du PMM (O(nframes)).
 */
bool paging_audit(struct paging_audit *a)
{
    const struct pmm *p = pmm_state();
    uint32_t pd = (uint32_t)(uintptr_t)g_pd;
    *a = (struct paging_audit){ 0 };
    for (uint32_t i = 0; i < 1024u; i++) {
        uint32_t pde = g_pd[i];
        if (!(pde & PTE_PRESENT)) {
            continue;
        }
        uint32_t t = pde & PTE_FRAME;
        a->tables++;
        if ((pde & 0xFFFu & ~(uint32_t)PTE_ACCESSED) != (PTE_PRESENT | PTE_WRITE) || t == pd ||
            !frame_is_pagetable(t)) {
            a->bad_pdes++;
            continue;                           /* ne pas parcourir une « table » douteuse */
        }
        const volatile uint32_t *pt = (const volatile uint32_t *)(uintptr_t)t;
        for (uint32_t j = 0; j < 1024u; j++) {
            uint32_t pte = pt[j];
            uint32_t va = (i << 22) | (j << 12);
            if (!(pte & PTE_PRESENT) || !frame_is_pagetable(pte & PTE_FRAME)) {
                continue;
            }
            if ((pte & PTE_FRAME) != va) {
                a->aliases++;
            } else if (pte & PTE_WRITE) {
                a->writable++;
            }
        }
    }
    uint32_t published = a->tables + 1u;        /* + le répertoire */
    uint32_t typed = 0;
    for (uint32_t f = 0; f < p->nframes; f++) {
        if (pmm_core_type(p, f) == FRAME_PAGETABLE) {
            typed++;
        }
    }
    a->unpublished = typed > published ? typed - published : 0;
    /* K0.7-P4f-B2 — PDE en double (deux PDE vers une même table, ou PDE vers
     * le répertoire), sans le PMM. O(n²) sur les PDE présentes. */
    for (uint32_t i = 0; i < 1024u; i++) {
        uint32_t pde = g_pd[i];
        if (!(pde & PTE_PRESENT)) {
            continue;
        }
        uint32_t t = pde & PTE_FRAME;
        bool dup = t == pd;
        for (uint32_t k = 0; k < i && !dup; k++) {
            dup = (g_pd[k] & PTE_PRESENT) && (g_pd[k] & PTE_FRAME) == t;
        }
        if (dup) {
            a->dup_pdes++;
        }
    }
    /* K0.7-P4f-B1 — Contrôles SANS le PMM : alias hors identité d'un cadre
     * de pagination publié ou de l'image ; identité inscriptible d'un cadre
     * de pagination publié. */
    for (uint32_t i = 0; i < 1024u; i++) {
        uint32_t pde = g_pd[i];
        if (!(pde & PTE_PRESENT)) {
            continue;
        }
        const volatile uint32_t *pt = (const volatile uint32_t *)(uintptr_t)(pde & PTE_FRAME);
        for (uint32_t j = 0; j < 1024u; j++) {
            uint32_t pte = pt[j], va = (i << 22) | (j << 12);
            if (!(pte & PTE_PRESENT)) {
                continue;
            }
            uint32_t frame = pte & PTE_FRAME;
            if (frame != va && frame_alias_forbidden(frame)) {
                a->foreign_aliases++;
            } else if (frame == va && (pte & PTE_WRITE) && frame_is_paging_structure(frame)) {
                a->struct_writable++;
            }
        }
    }
    return a->writable == 0 && a->aliases == 0 && a->bad_pdes == 0 && a->unpublished == 0 &&
           typed == published && a->foreign_aliases == 0 && a->struct_writable == 0 && a->dup_pdes == 0;
}

#if CONFIG_KTEST
/* Image de TEST uniquement : accès direct au passage pour les tests de refus,
 * et simulation d'une fenêtre en cours (imbrication). */
samos_err_t paging_gate_test_write(int kind, uint32_t va, uint32_t value)
{
    return pt_gate_write(kind == 0 ? PT_KIND_PDE : PT_KIND_PTE, va, value);
}

void paging_gate_test_set_active(bool active)
{
    g_pt_gate_active = active ? 1u : 0u;
}
#endif

/*
 * K0.7-M2.4c — Chemin unique de création d'une projection.
 *   counted = true  : projection DYNAMIQUE (vmm_map). Un cadre ALLOCATED
 *                     prend exactement une référence (pmm_ref) ; un cadre FREE
 *                     est refusé (il pourrait être réalloué en restant projeté).
 *                     Depuis M2.5b, tout type autre que ALLOCATED est refusé ;
 *                     depuis M2.5c, seuls les droits 0 et PTE_WRITE sont admis.
 *   counted = false : projection en IDENTITÉ permanente, jamais comptée.
 *                     Réservée à paging_init(), avant l'activation de la
 *                     pagination (vérifié par KASSERT) : ce chemin ne peut pas
 *                     servir à créer une projection dynamique.
 * Transaction : toutes les vérifications faillibles précèdent la prise de
 * référence, et l'écriture de la PTE, qui ne peut plus échouer, la suit.
 */
static samos_err_t map_page(uint32_t va, uint32_t pa, uint32_t flags, bool counted)
{
    if ((va | pa) & 0xFFFu || (flags & ~0xFFFu) != 0) {
        return SAMOS_EINVAL;
    }
    /* K0.7-M2.5c — Droits d'une projection dynamique : 0 (lecture seule) ou
     * PTE_WRITE, rien d'autre (constat C1). Refusés (SAMOS_EINVAL) : PTE_USER
     * (accès ring 3), PTE_PRESENT (posé par le VMM seul, ci-dessous), PWT/PCD
     * et PAT (cache), A/D (gérés par le CPU), G et les bits 9-11 (logiciels,
     * réservés). Premier contrôle : un refus ne crée aucune table, ne prend
     * aucune référence et n'écrit aucune PTE. Le chemin d'identité
     * (counted = false) n'est pas concerné : paging_init() ne passe que
     * 0 ou PTE_WRITE. */
    if (counted && (flags & ~PTE_WRITE) != 0) {
        return SAMOS_EINVAL;
    }
    if (va_must_stay_unmapped(va)) {
        return SAMOS_EPERM;
    }
    if (!counted) {
        KASSERT(!paging_enabled());
    }
    enum frame_type t = pmm_core_type(pmm_state(), pa >> PAGE_SHIFT);
    /* K0.7-M2.5b — Seul un cadre ALLOCATED est projetable dynamiquement.
     * Refusés (SAMOS_EPERM) : FREE (E5 : réallocation sous projection),
     * KERNEL (code, constantes, GDT, IDT, TSS, table du PMM : E3),
     * PAGETABLE (structures de pagination : E3), RESERVED et hors mémoire
     * gérée (pmm_core_type renvoie FRAME_RESERVED au-delà de nframes).
     * Vérifié AVANT get_table()/table_create() : un refus ne crée aucune table. */
    if (counted && t != FRAME_ALLOCATED) {
        return SAMOS_EPERM;
    }
    /* K0.7-P4f-B1 — Second critère, INDÉPENDANT du PMM : jamais d'alias d'un
     * cadre de pagination publié ni de l'image du noyau, même si leur
     * descripteur PMM a été falsifié (D1). Avant toute création de table :
     * un refus reste sans effet de bord. Le passage le revérifie. */
    if (counted && frame_alias_forbidden(pa)) {
        return SAMOS_EPERM;
    }
    uint32_t *pt = get_table(va);
    if (pt == 0) {
        samos_err_t ce = table_create(va, &pt);
        if (ce != SAMOS_OK) {
            return ce;
        }
    }
    if (pt[PT_INDEX(va)] & PTE_PRESENT) {
        return SAMOS_EEXIST;                   /* jamais d'écrasement silencieux */
    }
    if (counted && t == FRAME_ALLOCATED) {
        samos_err_t e = pmm_ref(pa);           /* dernière étape faillible */
        if (e != SAMOS_OK) {
            return e;                          /* aucune PTE écrite, aucune référence prise */
        }
    }
    /* K0.7-P4e-1 : écriture par le passage contrôlé, qui revalide la cible
     * et la valeur. Refus après pmm_ref : la référence est rendue, aucune PTE
     * n'a été écrite (seule la table éventuellement créée subsiste, comme
     * pour ERANGE, documenté depuis M2.4c). */
    samos_err_t ge = pt_gate_write(PT_KIND_PTE, va, pa | flags | PTE_PRESENT);
    if (ge != SAMOS_OK) {
        if (counted && t == FRAME_ALLOCATED) {
            samos_err_t ue = pmm_unref(pa);
            KASSERT(ue == SAMOS_OK);
        }
        return ge;
    }
    (void)pt;
    if (paging_enabled()) {
        cpu_invlpg((const void *)(uintptr_t)va);   /* pas d'entrée périmée « non présente » dans le TLB */
    }
    return SAMOS_OK;
}

samos_err_t vmm_map(uint32_t va, uint32_t pa, uint32_t flags)
{
    return map_page(va, pa, flags, true);
}

/*
 * K0.7-M2.4d — Suppression transactionnelle d'une projection dynamique.
 * Distinction compté / non compté, sans état supplémentaire :
 *   (a) toute adresse < idmap_top appartient à l'identité permanente, complète
 *       dès paging_init (vmm_map ne peut rien y ajouter : chaque page y est
 *       présente, sauf la page nulle et la garde, interdites par E1/E2)
 *       => refusée (EPERM), jamais supprimée ;
 *   (b) au-delà, la PTE vient de vmm_map (compté). Le type d'un cadre ne peut
 *       pas changer tant qu'il est projeté (ALLOCATED référencé non libérable ;
 *       KERNEL, RESERVED, PAGETABLE sans transition) : « ALLOCATED maintenant »
 *       équivaut donc à « une référence a été prise au map ».
 * Ordre : vérifier que la référence existe, PUIS effacer la PTE et invalider
 * le TLB, PUIS relâcher la référence (ne peut plus échouer). En cas de refus,
 * la projection reste intacte ; aucune PTE présente ne désigne jamais un
 * cadre ALLOCATED sans référence.
 */
samos_err_t vmm_unmap(uint32_t va, uint32_t *pa)
{
    if ((va & 0xFFFu) != 0) {
        return SAMOS_EINVAL;                   /* K0.6 arrondissait silencieusement */
    }
    if (va < g_idmap_top) {
        return SAMOS_EPERM;                    /* identité permanente */
    }
    uint32_t *pt = get_table(va);
    if (pt == 0 || !(pt[PT_INDEX(va)] & PTE_PRESENT)) {
        return SAMOS_ENOENT;
    }
    uint32_t frame = pt[PT_INDEX(va)] & PTE_FRAME;
    bool counted = pmm_core_type(pmm_state(), frame >> PAGE_SHIFT) == FRAME_ALLOCATED;
    if (counted && pmm_refs(frame) == 0) {
        return SAMOS_EINVAL;                   /* incohérence : on ne touche à rien */
    }
    /* K0.7-P4e-1 : effacement par le passage contrôlé ; un refus laisse la
     * projection et la référence intactes. */
    samos_err_t ge = pt_gate_write(PT_KIND_PTE, va, 0);
    if (ge != SAMOS_OK) {
        return ge;
    }
    if (pa != 0) {
        *pa = frame;
    }
    /* Indispensable : sans INVLPG, le TLB peut conserver l'ancienne
     * traduction et l'accès continuerait de réussir (vérifié par autotest). */
    cpu_invlpg((const void *)(uintptr_t)va);
    if (counted) {
        samos_err_t e = pmm_unref(frame);
        KASSERT(e == SAMOS_OK);                /* garanti par la vérification préalable */
    }
    return SAMOS_OK;
}

bool vmm_translate(uint32_t va, uint32_t *pa, uint32_t *flags)
{
    uint32_t *pt = get_table(va);
    if (pt == 0) {
        return false;
    }
    uint32_t pte = pt[PT_INDEX(va)];
    if (!(pte & PTE_PRESENT)) {
        return false;
    }
    if (pa != 0) {
        *pa = (pte & PTE_FRAME) | (va & 0xFFFu);
    }
    if (flags != 0) {
        *flags = pte & 0xFFFu;
    }
    return true;
}

uint32_t paging_directory(void) { return (uint32_t)(uintptr_t)g_pd; }
uint32_t paging_table_count(void) { return g_tables; }

void mem_probe_last_fault(uint32_t *cr2, uint32_t *err)
{
    *cr2 = g_probe_cr2;
    *err = g_probe_err;
}

static bool in_range(uint32_t a, const char *lo, const char *hi)
{
    return a >= (uint32_t)(uintptr_t)lo && a < (uint32_t)(uintptr_t)hi;
}

const char *paging_classify(uint32_t addr, uint32_t err)
{
    bool protection = (err & 1u) != 0;
    bool write = (err & 2u) != 0;
    if (addr < PAGE_SIZE) {
        return "NULL pointer dereference (page 0 is never mapped)";
    }
    if (in_range(addr, __stack_guard_start, __stack_guard_end)) {
        return "kernel stack overflow (guard page below the boot stack)";
    }
    if (in_range(addr, __stack_top_guard_start, __stack_top_guard_end)) {
        return "access above the boot stack top (S1 top guard page)";
    }
    if (in_range(addr, __df_guard_start, __df_guard_end)) {
        return "access to the double-fault stack guard page (below S2)";
    }
    if (protection && write && (in_range(addr, __kro_idt_start, __kro_idt_end) ||
                                in_range(addr, __kro_handlers_start, __kro_handlers_end) ||
                                in_range(addr, __kro_vmm_start, __kro_vmm_end))) {
        return "write to a sealed kernel structure (IDT, handler tables or VMM roots)";
    }
    if (protection && write && addr < g_idmap_top && frame_is_pagetable(addr & PTE_FRAME)) {
        return "write to a sealed page table or page directory";
    }
    if (protection && write && in_range(addr, __text_start, __rodata_start)) {
        return "write to read-only kernel code (.text)";
    }
    if (protection && write && in_range(addr, __rodata_start, __data_start)) {
        return "write to read-only kernel data (.rodata)";
    }
    if (!protection && addr >= g_idmap_top) {
        return "access to an unmapped address (outside identity-mapped RAM)";
    }
    return protection ? "protection violation" : "access to an unmapped page";
}

bool paging_is_probe_fault(const struct interrupt_frame *f)
{
    return f->eip == (uint32_t)(uintptr_t)mem_probe_read_insn ||
           f->eip == (uint32_t)(uintptr_t)mem_probe_write_insn;
}

static void page_fault_handler(struct interrupt_frame *f)
{
    uint32_t cr2 = cpu_read_cr2();
    /* Faute attendue dans une sonde : correction et reprise (probe.asm). */
    if (paging_is_probe_fault(f)) {
        g_probe_cr2 = cr2;
        g_probe_err = f->err_code;
        f->eip = (uint32_t)(uintptr_t)mem_probe_fixup;
        return;
    }
    exception_report(f);
    const char *why = paging_classify(cr2, f->err_code);
    kprintf("    diagnosis: %s\n", why);
    panic("page fault at %p: %s", (void *)(uintptr_t)cr2, why);
}

samos_err_t paging_init(uint32_t idmap_top)
{
    uint32_t kend = (uint32_t)(uintptr_t)__kernel_end;
    if (idmap_top < kend) {
        idmap_top = kend;                      /* l'image du noyau doit être projetée */
    }
    idmap_top &= PTE_FRAME;
    g_idmap_top = idmap_top;

    uint32_t pd = pmm_alloc_pagetable();        /* K0.7-M2.3 : répertoire typé FRAME_PAGETABLE */
    if (pd == 0) {
        return SAMOS_ENOMEM;
    }
    g_pd = (uint32_t *)(uintptr_t)pd;

    uint32_t ro_start = (uint32_t)(uintptr_t)__text_start;
    uint32_t ro_end = (uint32_t)(uintptr_t)__data_start;      /* .text + .rodata */
    /* Page 0 : jamais projetée (on commence à 4 KiB). Les pages de garde
     * (va_must_stay_unmapped : garde basse de S1, garde haute de S1 depuis
     * R1f-2, garde basse de S2 depuis R1f-3) restent NON présentes : une seule règle pour l'identité et pour
     * vmm_map. */
    for (uint32_t va = PAGE_SIZE; va < idmap_top; va += PAGE_SIZE) {
        if (va_must_stay_unmapped(va)) {
            continue;                          /* page de garde : NON présente */
        }
        uint32_t flags = (va >= ro_start && va < ro_end) ? 0u : PTE_WRITE;
        samos_err_t e = map_page(va, va, flags, false);   /* identité : non comptée */
        if (e != SAMOS_OK) {
            return e;
        }
    }

    if (!interrupt_register(EXC_PF, page_fault_handler)) {
        return SAMOS_EBUSY;
    }
    /* La tâche de double faute doit voir les mêmes projections. */
    gdt_set_double_fault_cr3(pd);

    cpu_write_cr3(pd);
    /* PG : active la traduction. WP : les pages en lecture seule le sont aussi
     * pour le ring 0 (sinon le noyau pourrait écrire dans .text sans faute). */
    cpu_write_cr0(cpu_read_cr0() | CR0_PG | CR0_WP);
    /* K0.7-P4e-2 : répertoire et tables en lecture seule dans l'identité,
     * avant tout autre code (IRQ encore masquées, aucun pilote). */
    return seal_paging_structures();
}

/*
 * K0.7-P4d — Scellement d'une plage de la projection d'identité : retire
 * PTE_WRITE de chaque page, puis INVLPG. Opération à SENS UNIQUE (elle ne fait
 * que retirer un droit ; aucune fonction ne le rend) ; elle passe par les
 * tables de pages, encore inscriptibles en P4d, et non par une fenêtre
 * CR0.WP : CR0.WP reste actif en permanence.
 * Préconditions, TOUTES vérifiées avant la première modification (pas de
 * scellement partiel en cas de refus) :
 *   - pagination active, bornes alignées, start < end <= g_idmap_top (EINVAL) ;
 *   - chaque page présente, en identité (PTE -> cadre de même adresse), et
 *     de type FRAME_KERNEL : seules les structures de l'image sont visées
 *     (EPERM). Les identités ne sont pas comptées : aucune référence PMM ne
 *     change.
 */
samos_err_t paging_seal_identity(uint32_t start, uint32_t end)
{
    if (!paging_enabled() || ((start | end) & 0xFFFu) != 0 || start >= end || end > g_idmap_top) {
        return SAMOS_EINVAL;
    }
    for (uint32_t va = start; va < end; va += PAGE_SIZE) {
        uint32_t *pt = get_table(va);
        if (pt == 0) {
            return SAMOS_EPERM;
        }
        uint32_t pte = pt[PT_INDEX(va)];
        if (!(pte & PTE_PRESENT) || (pte & PTE_FRAME) != va ||
            pmm_core_type(pmm_state(), va >> PAGE_SHIFT) != FRAME_KERNEL) {
            return SAMOS_EPERM;
        }
    }
    for (uint32_t va = start; va < end; va += PAGE_SIZE) {
        uint32_t *pt = get_table(va);
        /* K0.7-P4e-1 : par le passage contrôlé. Préconditions déjà vérifiées
         * pour toute la plage : un refus est une incohérence interne (par
         * exemple CR0.WP trouvé à 0). Il est RENVOYÉ, non transformé en
         * KASSERT : l'appelant de production (kmain) panique déjà sur tout
         * échec du scellement (refus fermé, scellement éventuellement
         * partiel jamais utilisé), et l'image de test le signale comme un
         * échec au lieu d'une panique. Les pages déjà traitées restent
         * scellées (l'opération ne fait que retirer W). */
        samos_err_t ge = pt_gate_write(PT_KIND_PTE, va, pt[PT_INDEX(va)] & ~PTE_WRITE);
        if (ge != SAMOS_OK) {
            return ge;
        }
        cpu_invlpg((const void *)(uintptr_t)va);
    }
    return SAMOS_OK;
}

/*
 * K0.7-P4d — Scellement des trois pages préparées en P4b (linker.ld) :
 * kro_idt (IDT), kro_handlers (g_handlers, g_irq_handlers), kro_vmm
 * (g_idmap_top, g_pd). À appeler APRÈS la dernière écriture légitime :
 * dans kmain, après pit_init et keyboard_init (dernières irq_register) ;
 * dans l'image de test, ktest_run_all l'appelle après les suites qui
 * modifient l'IDT ou les gestionnaires. Idempotente. Une page par appel :
 * chaque scellement est une ligne distincte (mutations M19 à M21).
 */
samos_err_t paging_seal_critical(void)
{
    samos_err_t e;
    e = paging_seal_identity((uint32_t)(uintptr_t)__kro_idt_start, (uint32_t)(uintptr_t)__kro_idt_end);
    if (e != SAMOS_OK) {
        return e;
    }
    e = paging_seal_identity((uint32_t)(uintptr_t)__kro_handlers_start, (uint32_t)(uintptr_t)__kro_handlers_end);
    if (e != SAMOS_OK) {
        return e;
    }
    e = paging_seal_identity((uint32_t)(uintptr_t)__kro_vmm_start, (uint32_t)(uintptr_t)__kro_vmm_end);
    return e;
}
