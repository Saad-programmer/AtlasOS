/*
 * SAMOS / AtlasOS — kernel/tests/test_boot.c
 * ---------------------------------------------------------------------------
 * Autotests du jalon K0.1/K0.2 : état CPU après démarrage, pile, ABI,
 * disposition des sections, informations Multiboot, bibliothèque string.
 */
#include <stdint.h>
#include <samos/bootinfo.h>
#include <samos/kernel.h>
#include <samos/ktest.h>
#include <samos/string.h>
#include <arch/cpu.h>

/* Variable de .data : doit avoir sa valeur initiale (segment data chargé). */
static volatile uint32_t g_data_canary = 0xC0FFEE42u;
/* Tableau de .bss : doit valoir zéro (entry.asm efface .bss). */
static volatile uint32_t g_bss_probe[64];

#define PAGE_ALIGNED(p) ((((uintptr_t)(p)) & 0xFFFu) == 0)

static void test_cpu_state(void)
{
    uint32_t eflags = cpu_read_eflags();
    uint32_t cr0 = cpu_read_cr0();
    KTEST_CHECK((eflags & EFLAGS_DF) == 0);   /* ABI : DF = 0 */
    KTEST_CHECK((eflags & EFLAGS_IF) == 0);   /* pas d'IDT : interruptions masquées */
    KTEST_CHECK((cr0 & CR0_PE) != 0);         /* mode protégé */
}

static void test_stack(void)
{
    uintptr_t esp = cpu_read_esp();
    uintptr_t lo = (uintptr_t)boot_stack_bottom;
    uintptr_t hi = (uintptr_t)boot_stack_top;
    KTEST_CHECK(esp > lo && esp < hi);                    /* on s'exécute sur NOTRE pile */
    KTEST_CHECK((hi & 0xFu) == 0);                        /* sommet aligné sur 16 */
    KTEST_CHECK(hi - lo == 16384);                        /* taille attendue */
    /* À l'entrée de kmain : EBP = ESP_entrée - 4 (après push ebp).
     * L'ABI exige (ESP_entrée + 4) % 16 == 0, soit (EBP + 8) % 16 == 0. */
    KTEST_CHECK(kmain_frame_address != 0);
    KTEST_CHECK(((kmain_frame_address + 8u) & 0xFu) == 0);
}

static void test_layout(void)
{
    KTEST_CHECK((uintptr_t)__kernel_start == 0x00100000u);
    KTEST_CHECK(PAGE_ALIGNED(__text_start));
    KTEST_CHECK(PAGE_ALIGNED(__rodata_start));
    KTEST_CHECK(PAGE_ALIGNED(__data_start));
    KTEST_CHECK(PAGE_ALIGNED(__bss_start));
    KTEST_CHECK(PAGE_ALIGNED(__kernel_end));
    KTEST_CHECK(__text_end <= __rodata_start);
    KTEST_CHECK(__rodata_end <= __data_start);
    KTEST_CHECK(__data_end <= __bss_start);
    KTEST_CHECK(__bss_end <= __kernel_end);
    KTEST_CHECK(boot_stack_bottom >= __bss_end && boot_stack_top <= __kernel_end);
    /* Le code de kmain doit se trouver dans .text */
    KTEST_CHECK((char *)(uintptr_t)&kmain >= __text_start && (char *)(uintptr_t)&kmain < __text_end);
}

static void test_sections_content(void)
{
    KTEST_CHECK(g_data_canary == 0xC0FFEE42u);
    bool all_zero = true;
    for (unsigned i = 0; i < SAMOS_ARRAY_SIZE(g_bss_probe); i++) {
        if (g_bss_probe[i] != 0) {
            all_zero = false;
        }
    }
    KTEST_CHECK(all_zero);
}

static void test_bootinfo(void)
{
    const struct bootinfo *bi = bootinfo_get();
    KTEST_CHECK(bi->magic == MULTIBOOT_BOOTLOADER_MAGIC);
    KTEST_CHECK((bi->flags & MBI_FLAG_MEM) != 0);
    KTEST_CHECK((bi->flags & MBI_FLAG_MMAP) != 0);
    KTEST_CHECK((bi->flags & MBI_FLAG_CMDLINE) != 0);
    KTEST_CHECK(cmdline_has("test"));
    KTEST_CHECK(!cmdline_has("tes"));             /* préfixe seul : pas de correspondance */
    KTEST_CHECK(bootinfo_region_ok(bi->mbi_phys, (uint32_t)sizeof(struct multiboot_info)));
    /* Régions invalides rejetées */
    KTEST_CHECK(!bootinfo_region_ok(0, 16));
    KTEST_CHECK(!bootinfo_region_ok(0xFFFFFFF0u, 0x20));          /* débordement 32 bits */
    KTEST_CHECK(!bootinfo_region_ok((uint32_t)(uintptr_t)__text_start, 4)); /* dans le noyau */
    KTEST_CHECK(bootinfo_region_ok(0xFFFFFFF0u, 0x10));           /* fin exacte : ok */
}

static void test_string(void)
{
    char a[16];
    char b[16];
    memset(a, 'x', sizeof(a));
    KTEST_CHECK(a[0] == 'x' && a[15] == 'x');
    memcpy(b, "samos", 6);
    KTEST_CHECK(strcmp(b, "samos") == 0);
    KTEST_CHECK(strlen("atlas") == 5);
    KTEST_CHECK(strnlen("atlas", 3) == 3);
    KTEST_CHECK(memcmp("abc", "abd", 3) < 0);
    KTEST_CHECK(strncmp("abcX", "abcY", 3) == 0);
    memcpy(a, "0123456789", 11);
    memmove(a + 2, a, 5);                         /* chevauchement vers l'avant */
    KTEST_CHECK(memcmp(a, "0101234789", 10) == 0);
    KTEST_CHECK(strlcpy(b, "abcdefghijklmnopqrstuvwxyz", 8) == 26);
    KTEST_CHECK(strcmp(b, "abcdefg") == 0);       /* tronqué et terminé */
}

void ktest_suite_boot(void)
{
    ktest_suite("K0.1/K0.2 boot");
    test_cpu_state();
    test_stack();
    test_layout();
    test_sections_content();
    test_bootinfo();
    test_string();
}
