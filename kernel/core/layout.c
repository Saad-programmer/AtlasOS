/*
 * SAMOS / AtlasOS — kernel/core/layout.c
 * Disposition mémoire du noyau et instrumentation de la pile (K0.2).
 */
#include <samos/layout.h>
#include <samos/kernel.h>
#include <samos/qemu_dbg.h>

#define PAGE_MASK 0xFFFu

/* Les adresses des symboles du linker ne sont pas des constantes de
 * compilation en C : le tableau est rempli à l'exécution. */
static struct kernel_region g_regions[5];

static size_t layout_fill(void)
{
    g_regions[0] = (struct kernel_region){ ".text",   (uintptr_t)__text_start,   (uintptr_t)__rodata_start,      LAYOUT_R | LAYOUT_X };
    g_regions[1] = (struct kernel_region){ ".rodata", (uintptr_t)__rodata_start, (uintptr_t)__data_start,        LAYOUT_R };
    g_regions[2] = (struct kernel_region){ ".data+.bss", (uintptr_t)__data_start, (uintptr_t)__stack_guard_start, LAYOUT_R | LAYOUT_W };
    g_regions[3] = (struct kernel_region){ "stack-guard", (uintptr_t)__stack_guard_start, (uintptr_t)__stack_guard_end, LAYOUT_GUARD };
    g_regions[4] = (struct kernel_region){ "boot-stack", (uintptr_t)boot_stack_bottom, (uintptr_t)__kernel_end,   LAYOUT_R | LAYOUT_W };
    return sizeof(g_regions) / sizeof(g_regions[0]);
}

size_t kernel_layout(const struct kernel_region **out)
{
    size_t n = layout_fill();
    *out = g_regions;
    return n;
}

bool kernel_layout_valid(void)
{
    const struct kernel_region *r;
    size_t n = kernel_layout(&r);
    uintptr_t expected = (uintptr_t)__kernel_start;

    for (size_t i = 0; i < n; i++) {
        if ((r[i].start & PAGE_MASK) != 0 || (r[i].end & PAGE_MASK) != 0) {
            return false;       /* une région doit couvrir des pages entières */
        }
        if (r[i].start != expected || r[i].end <= r[i].start) {
            return false;       /* régions contiguës, non vides, croissantes */
        }
        expected = r[i].end;
    }
    return expected == (uintptr_t)__kernel_end;
}

static void print_perms(uint32_t p)
{
    if (p & LAYOUT_GUARD) {
        qemu_dbg_puts("---G");
        return;
    }
    qemu_dbg_putc((p & LAYOUT_R) ? 'r' : '-');
    qemu_dbg_putc((p & LAYOUT_W) ? 'w' : '-');
    qemu_dbg_putc((p & LAYOUT_X) ? 'x' : '-');
    qemu_dbg_putc(' ');
}

void kernel_layout_print(void)
{
    const struct kernel_region *r;
    size_t n = kernel_layout(&r);
    qemu_dbg_puts("SAMOS: kernel image ");
    qemu_dbg_put_hex32((uint32_t)(uintptr_t)__kernel_start);
    qemu_dbg_puts("-");
    qemu_dbg_put_hex32((uint32_t)(uintptr_t)__kernel_end);
    qemu_dbg_puts("\n");
    for (size_t i = 0; i < n; i++) {
        qemu_dbg_puts("SAMOS:   ");
        qemu_dbg_put_hex32((uint32_t)r[i].start);
        qemu_dbg_puts("-");
        qemu_dbg_put_hex32((uint32_t)r[i].end);
        qemu_dbg_puts(" ");
        print_perms(r[i].perms);
        qemu_dbg_puts(" ");
        qemu_dbg_puts(r[i].name);
        qemu_dbg_puts("\n");
    }
}

/*
 * La pile croît vers le bas : la case la plus basse jamais écrite marque la
 * profondeur maximale. On cherche, depuis boot_stack_bottom vers le haut, le
 * premier mot qui n'est plus égal au motif.
 * Limite : une valeur légitimement égale au motif fausserait la mesure d'un
 * mot (4 octets) ; c'est acceptable pour un indicateur de diagnostic.
 */
size_t boot_stack_high_water(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)(uintptr_t)boot_stack_bottom;
    const volatile uint32_t *top = (const volatile uint32_t *)(uintptr_t)boot_stack_top;
    while (p < top && *p == BOOT_STACK_PAINT) {
        p++;
    }
    return (size_t)((uintptr_t)top - (uintptr_t)p);
}

bool boot_stack_guard_intact(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)(uintptr_t)__stack_guard_start;
    const volatile uint32_t *end = (const volatile uint32_t *)(uintptr_t)__stack_guard_end;
    for (; p < end; p++) {
        if (*p != BOOT_STACK_PAINT) {
            return false;
        }
    }
    return true;
}
