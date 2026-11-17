/*
 * SAMOS / AtlasOS — kernel/include/samos/log.h
 * ---------------------------------------------------------------------------
 * Journalisation du noyau (K0.3) : une sortie texte unique diffusée vers
 * tous les « puits » disponibles :
 *   - debugcon QEMU (port 0xE9) : toujours tenté, utile aux tests ;
 *   - port série COM1           : si le test de présence a réussi ;
 *   - écran VGA texte           : si initialisé.
 *
 * kprintf() formate dans un tampon de pile de LOG_LINE_MAX octets (pas
 * d'allocation dynamique) ; une ligne plus longue est tronquée et marquée.
 * Contexte d'appel : K0.3 n'a pas d'interruptions ; à partir de K0.5, un
 * gestionnaire d'IRQ ne doit PAS journaliser (voir Details.md §18.5).
 */
#ifndef SAMOS_LOG_H
#define SAMOS_LOG_H

#include <stdarg.h>
#include <stddef.h>
#include <samos/compiler.h>

#define LOG_LINE_MAX 256

#define LOG_SINK_DEBUGCON (1u << 0)
#define LOG_SINK_SERIAL   (1u << 1)
#define LOG_SINK_VGA      (1u << 2)

enum log_level {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};

/* Initialise VGA et série ; retourne le masque des puits actifs. */
unsigned log_init(void);
unsigned log_sinks(void);

/* Écrit n octets bruts sur tous les puits actifs. */
void console_write(const char *s, size_t n);

int  kprintf(const char *fmt, ...) SAMOS_PRINTF(1, 2);
int  kvprintf(const char *fmt, va_list ap);
void klog(enum log_level level, const char *fmt, ...) SAMOS_PRINTF(2, 3);

/* Nombre de lignes tronquées depuis le démarrage (diagnostic). */
unsigned log_truncations(void);

#endif /* SAMOS_LOG_H */
