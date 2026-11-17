/*
 * SAMOS / AtlasOS — kernel/tests/test_layout.c
 * Autotests du jalon K0.2 : disposition mémoire, zone de garde, mesure de pile.
 */
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/layout.h>
#include <arch/cpu.h>

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
    KTEST_CHECK(n == 5);
    KTEST_CHECK(kernel_layout_valid());
    KTEST_CHECK((r[0].perms & LAYOUT_X) && !(r[0].perms & LAYOUT_W));   /* .text R-X */
    KTEST_CHECK(r[1].perms == LAYOUT_R);                                 /* .rodata R-- */
    KTEST_CHECK((r[2].perms & LAYOUT_W) && !(r[2].perms & LAYOUT_X));    /* données RW- */
    KTEST_CHECK(r[3].perms == LAYOUT_GUARD);
    KTEST_CHECK(r[3].end - r[3].start == 4096);
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
    KTEST_CHECK(boot_stack_top == __kernel_end);               /* la pile termine l'image */
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

    /* Test négatif : simuler un débordement en écrivant dans la zone de
     * garde, vérifier la détection, puis restaurer le motif. Sans pagination
     * cette écriture est possible ; après K0.6 elle provoquerait une #PF. */
    volatile uint32_t *g = (volatile uint32_t *)(uintptr_t)(__stack_guard_end - 4);
    *g = 0;
    KTEST_CHECK(!boot_stack_guard_intact());
    *g = BOOT_STACK_PAINT;
    KTEST_CHECK(boot_stack_guard_intact());
}

void ktest_suite_layout(void)
{
    ktest_suite("K0.2 layout");
    test_regions();
    test_stack_placement();
    test_stack_instrumentation();
}
