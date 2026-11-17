/*
 * SAMOS / AtlasOS — kernel/core/bootinfo.c
 * Validation des informations Multiboot. Voir bootinfo.h.
 */
#include <samos/bootinfo.h>
#include <samos/kernel.h>
#include <samos/string.h>

static struct bootinfo g_bootinfo;   /* dans .bss : initialisée à zéro */

bool bootinfo_region_ok(uint32_t addr, uint32_t len)
{
    uint32_t kstart = (uint32_t)(uintptr_t)__kernel_start;
    uint32_t kend   = (uint32_t)(uintptr_t)__kernel_end;

    if (addr == 0 || len == 0) {
        return false;
    }
    /* Débordement : addr + len doit rester <= 2^32. Écrit sans calculer
     * addr + len (qui pourrait déborder) : len <= 2^32 - 1 - addr + 1. */
    if (len - 1u > UINT32_MAX - addr) {
        return false;
    }
    uint32_t last = addr + (len - 1u);   /* dernier octet inclus, sans débordement */
    /* Chevauchement avec [kstart, kend) ? */
    if (addr < kend && last >= kstart) {
        return false;
    }
    return true;
}

/*
 * Copie bornée d'une chaîne C située à une adresse physique non fiable.
 * Lit au plus cap-1 octets. Retourne true si la chaîne a été tronquée.
 */
static bool copy_untrusted_string(char *dst, size_t cap, uint32_t phys)
{
    const char *src = (const char *)(uintptr_t)phys;
    size_t len = strnlen(src, cap - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
    /* Tronquée si l'octet suivant n'est pas le terminateur. */
    return src[len] != '\0';
}

samos_err_t bootinfo_init(uint32_t magic, uint32_t mbi_phys)
{
    memset(&g_bootinfo, 0, sizeof(g_bootinfo));
    g_bootinfo.magic = magic;
    g_bootinfo.mbi_phys = mbi_phys;

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        return SAMOS_EINVAL;
    }
    if (!bootinfo_region_ok(mbi_phys, (uint32_t)sizeof(struct multiboot_info))) {
        return SAMOS_EFAULT;
    }

    const struct multiboot_info *mbi = (const struct multiboot_info *)(uintptr_t)mbi_phys;
    g_bootinfo.flags = mbi->flags;

    if (mbi->flags & MBI_FLAG_MEM) {
        g_bootinfo.mem_lower_kib = mbi->mem_lower;
        g_bootinfo.mem_upper_kib = mbi->mem_upper;
    }
    if (mbi->flags & MBI_FLAG_MMAP) {
        if (bootinfo_region_ok(mbi->mmap_addr, mbi->mmap_length)) {
            g_bootinfo.mmap_addr = mbi->mmap_addr;
            g_bootinfo.mmap_length = mbi->mmap_length;
        } else {
            /* Carte invalide : on la considère absente. */
            g_bootinfo.flags &= ~MBI_FLAG_MMAP;
        }
    }
    if ((mbi->flags & MBI_FLAG_CMDLINE) && bootinfo_region_ok(mbi->cmdline, 1)) {
        g_bootinfo.cmdline_truncated =
            copy_untrusted_string(g_bootinfo.cmdline, sizeof(g_bootinfo.cmdline), mbi->cmdline);
    }
    if ((mbi->flags & MBI_FLAG_LOADER) && bootinfo_region_ok(mbi->boot_loader_name, 1)) {
        (void)copy_untrusted_string(g_bootinfo.loader_name, sizeof(g_bootinfo.loader_name),
                                    mbi->boot_loader_name);
    }
    return SAMOS_OK;
}

const struct bootinfo *bootinfo_get(void)
{
    return &g_bootinfo;
}

/*
 * Recherche du jeton `key` dans la ligne de commande copiée.
 * Un jeton correspond s'il vaut exactement `key` ou commence par `key=`.
 * Retourne un pointeur juste après la clé (sur '\0', ' ' ou '='), ou NULL.
 */
static const char *find_token(const char *key)
{
    const char *p = g_bootinfo.cmdline;
    size_t klen = strlen(key);

    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        const char *tok = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        size_t tlen = (size_t)(p - tok);
        if (tlen >= klen && strncmp(tok, key, klen) == 0 &&
            (tlen == klen || tok[klen] == '=')) {
            return tok + klen;
        }
    }
    return NULL;
}

bool cmdline_has(const char *key)
{
    return find_token(key) != NULL;
}

bool cmdline_value(const char *key, char *out, size_t cap)
{
    const char *v = find_token(key);
    if (v == NULL || *v != '=' || cap == 0) {
        return false;
    }
    v++;
    size_t n = 0;
    while (v[n] != '\0' && v[n] != ' ' && n < cap - 1) {
        out[n] = v[n];
        n++;
    }
    out[n] = '\0';
    return true;
}
