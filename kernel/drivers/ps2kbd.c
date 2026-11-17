/*
 * SAMOS / AtlasOS — kernel/drivers/ps2kbd.c
 * Contrôleur 8042 et clavier PS/2. Voir keyboard.h et Details.md §30.
 *
 * Ports : 0x60 données (lecture : octet du périphérique ; écriture : octet
 * VERS le périphérique), 0x64 état (lecture) / commande (écriture).
 * Registre d'état : bit 0 OBF (sortie pleine : un octet attend le CPU),
 *                   bit 1 IBF (entrée pleine : le contrôleur n'a pas encore
 *                          lu le dernier octet écrit ; ne pas écrire).
 */
#include <arch/io.h>
#include <arch/irq.h>
#include <samos/keyboard.h>
#include <samos/ringbuf.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64
#define ST_OBF 0x01u
#define ST_IBF 0x02u

#define CMD_READ_CONFIG   0x20u
#define CMD_WRITE_CONFIG  0x60u
#define CMD_DISABLE_PORT2 0xA7u
#define CMD_TEST_CTRL     0xAAu   /* réponse 0x55 = contrôleur OK */
#define CMD_TEST_PORT1    0xABu   /* réponse 0x00 = interface OK */
#define CMD_DISABLE_PORT1 0xADu
#define CMD_ENABLE_PORT1  0xAEu

#define CFG_IRQ1          0x01u   /* IRQ 1 quand un octet du port 1 arrive */
#define CFG_IRQ12         0x02u
#define CFG_PORT1_CLK_OFF 0x10u   /* positionné par la commande 0xAD, effacé par 0xAE */
#define CFG_PORT2_CLK_OFF 0x20u
#define CFG_TRANSLATE     0x40u   /* traduction jeu 2 -> jeu 1 */

#define DEV_RESET         0xFFu   /* réponse : 0xFA (ACK) puis 0xAA (auto-test OK) */
#define DEV_ENABLE_SCAN   0xF4u
#define DEV_ACK           0xFAu
#define DEV_SELFTEST_OK   0xAAu

/* Bornes d'attente : sur matériel réel, io_wait() dure ~1 µs ; le BAT
 * (auto-test) du clavier peut prendre plusieurs centaines de ms. */
#define WAIT_LIMIT        1000000u

#define KBD_BUF_SIZE 256u
static uint8_t g_storage[KBD_BUF_SIZE];
static struct ringbuf g_rb;
static struct kbd_decoder g_dec;
static volatile uint32_t g_received;
static const char *g_error = "not initialized";

static bool wait_input_empty(void)
{
    for (uint32_t i = 0; i < WAIT_LIMIT; i++) {
        if ((inb(PS2_STATUS) & ST_IBF) == 0) {
            return true;
        }
        io_wait();
    }
    return false;
}

static bool wait_output_full(void)
{
    for (uint32_t i = 0; i < WAIT_LIMIT; i++) {
        if (inb(PS2_STATUS) & ST_OBF) {
            return true;
        }
        io_wait();
    }
    return false;
}

static bool ctrl_cmd(uint8_t cmd)
{
    if (!wait_input_empty()) {
        return false;
    }
    outb(PS2_CMD, cmd);
    return true;
}

static bool write_data(uint8_t v)
{
    if (!wait_input_empty()) {
        return false;
    }
    outb(PS2_DATA, v);
    return true;
}

static bool read_data(uint8_t *v)
{
    if (!wait_output_full()) {
        return false;
    }
    *v = inb(PS2_DATA);
    return true;
}

static bool fail(const char *why)
{
    g_error = why;
    return false;
}

static void keyboard_irq(void)
{
    /* Contexte d'interruption : lire l'octet (ce qui libère le 8042 pour le
     * suivant) et le déposer. Aucun décodage, aucune journalisation. */
    uint8_t sc = inb(PS2_DATA);
    g_received++;
    (void)ringbuf_push(&g_rb, sc);   /* file pleine : octet perdu et compté */
}

bool keyboard_init(void)
{
    uint8_t v;
    kbd_decoder_init(&g_dec);
    if (!ringbuf_init(&g_rb, g_storage, KBD_BUF_SIZE)) {
        return fail("ring buffer");
    }

    /* 1. Désactiver les deux ports pendant la configuration. */
    if (!ctrl_cmd(CMD_DISABLE_PORT1) || !ctrl_cmd(CMD_DISABLE_PORT2)) {
        return fail("controller not responding");
    }
    /* 2. Vider le tampon de sortie (octets résiduels du BIOS), borné. */
    for (unsigned i = 0; i < 64 && (inb(PS2_STATUS) & ST_OBF); i++) {
        (void)inb(PS2_DATA);
    }
    /* 3. Configuration : IRQ désactivées pendant l'initialisation. */
    uint8_t cfg;
    if (!ctrl_cmd(CMD_READ_CONFIG) || !read_data(&cfg)) {
        return fail("read config");
    }
    cfg = (uint8_t)((cfg & ~(CFG_IRQ1 | CFG_IRQ12)) | CFG_TRANSLATE | CFG_PORT2_CLK_OFF);
    if (!ctrl_cmd(CMD_WRITE_CONFIG) || !write_data(cfg)) {
        return fail("write config");
    }
    /* 4. Auto-test du contrôleur. Sur certains contrôleurs il réinitialise la
     *    configuration : on la réécrit ensuite. */
    if (!ctrl_cmd(CMD_TEST_CTRL) || !read_data(&v) || v != 0x55) {
        return fail("controller self-test");
    }
    if (!ctrl_cmd(CMD_WRITE_CONFIG) || !write_data(cfg)) {
        return fail("rewrite config");
    }
    /* 5. Test de l'interface du port 1 (lignes horloge/données). */
    if (!ctrl_cmd(CMD_TEST_PORT1) || !read_data(&v) || v != 0x00) {
        return fail("port 1 interface test");
    }
    /* 6. Activer le port 1, puis réinitialiser le clavier lui-même. */
    if (!ctrl_cmd(CMD_ENABLE_PORT1)) {
        return fail("enable port 1");
    }
    if (!write_data(DEV_RESET) || !read_data(&v) || v != DEV_ACK) {
        return fail("keyboard reset: no ACK");
    }
    if (!read_data(&v) || v != DEV_SELFTEST_OK) {
        return fail("keyboard self-test (BAT)");
    }
    if (!write_data(DEV_ENABLE_SCAN) || !read_data(&v) || v != DEV_ACK) {
        return fail("enable scanning");
    }
    /* 7. Activer l'IRQ 1 dans le contrôleur, puis dans le PIC.
     *    ATTENTION : `cfg` a été lu APRÈS la commande 0xAD (étape 1), qui
     *    positionne le bit 4 « horloge du port 1 coupée ». Réécrire cette
     *    copie telle quelle annulerait la commande 0xAE de l'étape 6 et
     *    couperait de nouveau le clavier (défaut réellement rencontré en
     *    K0.5, voir Walkthrough). On efface donc explicitement le bit 4. */
    cfg = (uint8_t)((cfg | CFG_IRQ1) & ~CFG_PORT1_CLK_OFF);
    if (!ctrl_cmd(CMD_WRITE_CONFIG) || !write_data(cfg)) {
        return fail("enable IRQ1");
    }
    if (!irq_register(1, keyboard_irq)) {
        return fail("IRQ1 registration");
    }
    g_error = "ok";
    return true;
}

const char *keyboard_error(void)
{
    return g_error;
}

bool keyboard_get_event(struct key_event *ev)
{
    uint8_t sc;
    while (ringbuf_pop(&g_rb, &sc)) {
        if (kbd_decode(&g_dec, sc, ev)) {
            return true;
        }
    }
    return false;
}

uint32_t keyboard_bytes_received(void) { return g_received; }
uint32_t keyboard_dropped(void) { return g_rb.dropped; }
