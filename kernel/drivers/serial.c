/*
 * SAMOS / AtlasOS — kernel/drivers/serial.c
 * UART 16550 sur COM1. Voir serial.h et Details.md §18.
 *
 * Registres (décalage par rapport à la base 0x3F8) :
 *   +0 THR (écriture) / RBR (lecture)          | DLL si LCR.DLAB = 1
 *   +1 IER (activation des interruptions)      | DLM si LCR.DLAB = 1
 *   +2 FCR (écriture) / IIR (lecture)
 *   +3 LCR (format de ligne, bit 7 = DLAB)
 *   +4 MCR (contrôle modem, bit 4 = loopback)
 *   +5 LSR (état de ligne, bit 5 = THRE : registre d'émission vide)
 *   +7 SCR (registre de travail libre)
 */
#include <samos/serial.h>
#include <arch/io.h>

#define REG_DATA 0
#define REG_IER  1
#define REG_DLL  0
#define REG_DLM  1
#define REG_FCR  2
#define REG_LCR  3
#define REG_MCR  4
#define REG_LSR  5
#define REG_SCR  7

#define LCR_DLAB     0x80u
#define LCR_8N1      0x03u   /* 8 bits de données, pas de parité, 1 bit de stop */
#define FCR_ENABLE   0xC7u   /* FIFO activée, effacement RX/TX, seuil 14 octets */
#define MCR_NORMAL   0x0Fu   /* DTR | RTS | OUT1 | OUT2 */
#define MCR_LOOPBACK 0x1Eu   /* loopback | RTS | OUT1 | OUT2 */
#define LSR_DATA_READY 0x01u
#define LSR_THRE     0x20u

/* Diviseur : horloge UART 1,8432 MHz / 16 = 115200 ; diviseur 1 => 115200 bauds. */
#define BAUD_DIVISOR 1u

/* Nombre maximal de lectures de LSR avant d'abandonner un caractère : évite
 * de bloquer le noyau indéfiniment sur un UART absent ou défaillant. */
#define TX_SPIN_LIMIT 100000u

static const uint16_t g_base = SERIAL_COM1;
static bool g_ok;
static uint32_t g_dropped;

static inline void reg_w(unsigned reg, uint8_t v) { outb((uint16_t)(g_base + reg), v); }
static inline uint8_t reg_r(unsigned reg) { return inb((uint16_t)(g_base + reg)); }

bool serial_init(void)
{
    g_ok = false;

    /* 0. Présence : le registre de travail doit restituer ce qu'on y écrit.
     *    Sur un bus ISA sans périphérique, la lecture renvoie souvent 0xFF. */
    reg_w(REG_SCR, 0x5A);
    if (reg_r(REG_SCR) != 0x5A) {
        return false;
    }

    reg_w(REG_IER, 0x00);                    /* 1. aucune interruption */
    reg_w(REG_LCR, LCR_DLAB);                /* 2. accès au diviseur */
    reg_w(REG_DLL, (uint8_t)(BAUD_DIVISOR & 0xFFu));
    reg_w(REG_DLM, (uint8_t)(BAUD_DIVISOR >> 8));
    reg_w(REG_LCR, LCR_8N1);                 /* 3. format 8N1, DLAB = 0 */
    reg_w(REG_FCR, FCR_ENABLE);              /* 4. FIFO */

    /* 5. Test en boucle locale : en mode loopback, l'émetteur est relié au
     *    récepteur à l'intérieur de l'UART ; l'octet émis doit être reçu. */
    reg_w(REG_MCR, MCR_LOOPBACK);
    reg_w(REG_DATA, 0xAE);
    bool received = false;
    for (uint32_t i = 0; i < TX_SPIN_LIMIT; i++) {
        if (reg_r(REG_LSR) & LSR_DATA_READY) {
            received = true;
            break;
        }
    }
    if (!received || reg_r(REG_DATA) != 0xAE) {
        return false;
    }

    reg_w(REG_MCR, MCR_NORMAL);              /* 6. fonctionnement normal */
    g_ok = true;
    return true;
}

bool serial_available(void)
{
    return g_ok;
}

static void tx(char c)
{
    for (uint32_t i = 0; i < TX_SPIN_LIMIT; i++) {
        if (reg_r(REG_LSR) & LSR_THRE) {
            reg_w(REG_DATA, (uint8_t)c);
            return;
        }
    }
    g_dropped++;   /* UART bloqué : on perd le caractère plutôt que le noyau */
}

void serial_putc(char c)
{
    if (!g_ok) {
        return;
    }
    if (c == '\n') {
        tx('\r');     /* convention des terminaux série : CR LF */
    }
    tx(c);
}

void serial_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        serial_putc(s[i]);
    }
}

uint32_t serial_dropped(void)
{
    return g_dropped;
}
