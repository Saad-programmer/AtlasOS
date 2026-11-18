/*
 * SAMOS / AtlasOS — kernel/tests/test_layout.c
 * Autotests du jalon K0.2 : disposition mémoire, zone de garde, mesure de pile.
 */
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/layout.h>
#include <arch/cpu.h>
#include <arch/paging.h>

/* Consomme volontairement ~2 KiB de pile. noinline + volatile empêchent le
 * compilateur de supprimer ou de réduire le tableau. */
static __attribute__((noinline)) uint32_t consume_stack(void)
{
    volatile uint8_t buf[2048];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)i;
    }
    return buf[100] + buf[2000];
}

static void test_regions(void)
{
    const struct kernel_region *r;
    size_t n = kernel_layout(&r);
    KTEST_CHECK(n == 8);                                                  /* K0.7-R1f-1 */
    KTEST_CHECK(kernel_layout_valid());
    KTEST_CHECK((r[0].perms & LAYOUT_X) && !(r[0].perms & LAYOUT_W));   /* .text R-X */
    KTEST_CHECK(r[1].perms == LAYOUT_R);                                 /* .rodata R-- */
    KTEST_CHECK((r[2].perms & LAYOUT_W) && !(r[2].perms & LAYOUT_X));    /* données RW- */
    KTEST_CHECK(r[5].perms == LAYOUT_GUARD);                             /* garde basse S1 */
    KTEST_CHECK(r[5].end - r[5].start == 4096);
    KTEST_CHECK((r[3].perms & LAYOUT_RESERVED) && r[3].end - r[3].start == 4096);   /* garde S2 */
    KTEST_CHECK((r[7].perms & LAYOUT_RESERVED) && r[7].end - r[7].start == 4096);   /* garde haute */
    KTEST_CHECK(r[4].end - r[4].start == 4096 && r[6].end - r[6].start == 16384);
    /* Aucune région ne doit être à la fois W et X. */
    bool wx = false;
    for (size_t i = 0; i < n; i++) {
        if ((r[i].perms & LAYOUT_W) && (r[i].perms & LAYOUT_X)) {
            wx = true;
        }
    }
    KTEST_CHECK(!wx);
}

static void test_stack_placement(void)
{
    KTEST_CHECK(__stack_guard_end == boot_stack_bottom);       /* garde juste sous la pile */
    /* K0.7-R1f-1 : la page réservée à la garde haute termine l'image. */
    KTEST_CHECK(boot_stack_top == __stack_top_guard_start);
    KTEST_CHECK(__stack_top_guard_end == __kernel_end);
    KTEST_CHECK(__bss_end <= __stack_guard_start);             /* la pile n'est plus dans .bss */
    /* Le point d'entrée a été placé juste après l'en-tête Multiboot (12 octets) */
    uint32_t esp = cpu_read_esp();
    KTEST_CHECK(esp > (uint32_t)(uintptr_t)boot_stack_bottom);
}

static void test_stack_instrumentation(void)
{
    KTEST_CHECK(boot_stack_guard_intact());
    size_t before = boot_stack_high_water();
    KTEST_CHECK(before > 0 && before < 16384);
    (void)consume_stack();
    size_t after = boot_stack_high_water();
    KTEST_CHECK(after >= before && after >= 2048);
    KTEST_CHECK(boot_stack_guard_intact());                    /* 2 KiB ne débordent pas */

    /* Test négatif : simuler un débordement en écrivant dans la zone de garde.
     * K0.2 (sans pagination) : l'écriture réussissait et la peinture
     * corrompue était détectée a posteriori.
     * K0.6 (pagination active) : la page n'est pas projetée ; l'écriture,
     * faite par une sonde, doit être REFUSÉE par le MMU (faute de page). */
    volatile uint32_t *g = (volatile uint32_t *)(uintptr_t)(__stack_guard_end - 4);
    if (cpu_read_cr0() & CR0_PG) {
        KTEST_CHECK(mem_probe_write(g, 0) == -1);
        KTEST_CHECK(boot_stack_guard_intact());
    } else {
        *g = 0;
        KTEST_CHECK(!boot_stack_guard_intact());
        *g = BOOT_STACK_PAINT;
        KTEST_CHECK(boot_stack_guard_intact());
    }
}

void ktest_suite_layout(void)
{
    ktest_suite("K0.2 layout");
    test_regions();
    test_stack_placement();
    test_stack_instrumentation();
}
