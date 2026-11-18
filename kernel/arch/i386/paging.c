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

static uint32_t *g_pd;            /* répertoire de pages (physique = virtuel) */
static uint32_t g_tables;
static uint32_t g_idmap_top;
static volatile uint32_t g_probe_cr2;
static volatile uint32_t g_probe_err;

static bool paging_enabled(void)
{
    return (cpu_read_cr0() & CR0_PG) != 0;
}

static uint32_t *get_table(uint32_t va, bool create)
{
    uint32_t pde = g_pd[PD_INDEX(va)];
    if (pde & PTE_PRESENT) {
        return (uint32_t *)(uintptr_t)(pde & PTE_FRAME);
    }
    if (!create) {
        return 0;
    }
    /* K0.7-M2.3 : cadre typé FRAME_PAGETABLE (non libérable par pmm_free_frame),
     * remis à zéro par le PMM : 1024 PTE non présentes. */
    uint32_t pt = pmm_alloc_pagetable();
    if (pt == 0) {
        return 0;
    }
    /* PDE permissive (P | W) : les droits effectifs sont l'INTERSECTION des
     * droits du PDE et du PTE (SDM vol. 3A §4.6) ; c'est le PTE qui décide. */
    g_pd[PD_INDEX(va)] = pt | PTE_PRESENT | PTE_WRITE;
    g_tables++;
    return (uint32_t *)(uintptr_t)pt;
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
 * indépendamment de l'état actuel de la PTE. La future garde de S2
 * (__df_guard_*) n'est pas encore concernée (R1f-3). */
static bool va_must_stay_unmapped(uint32_t va)
{
    uint32_t page = va & PTE_FRAME;
    if (page == 0) {
        return true;
    }
    if (in_page_range(page, __stack_guard_start, __stack_guard_end)) {
        return true;                           /* garde basse de S1 (E2) */
    }
    return in_page_range(page, __stack_top_guard_start, __stack_top_guard_end);
}

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
     * Vérifié AVANT get_table() : un refus ne crée aucune table. */
    if (counted && t != FRAME_ALLOCATED) {
        return SAMOS_EPERM;
    }
    uint32_t *pt = get_table(va, true);
    if (pt == 0) {
        return SAMOS_ENOMEM;
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
    pt[PT_INDEX(va)] = pa | flags | PTE_PRESENT;
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
    uint32_t *pt = get_table(va, false);
    if (pt == 0 || !(pt[PT_INDEX(va)] & PTE_PRESENT)) {
        return SAMOS_ENOENT;
    }
    uint32_t frame = pt[PT_INDEX(va)] & PTE_FRAME;
    bool counted = pmm_core_type(pmm_state(), frame >> PAGE_SHIFT) == FRAME_ALLOCATED;
    if (counted && pmm_refs(frame) == 0) {
        return SAMOS_EINVAL;                   /* incohérence : on ne touche à rien */
    }
    if (pa != 0) {
        *pa = frame;
    }
    pt[PT_INDEX(va)] = 0;
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
    uint32_t *pt = get_table(va, false);
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
     * (va_must_stay_unmapped : garde basse et, depuis R1f-2, garde haute de
     * S1) restent NON présentes : une seule règle pour l'identité et pour
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
    return SAMOS_OK;
}
