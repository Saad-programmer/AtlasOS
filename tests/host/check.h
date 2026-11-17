/*
 * SAMOS / AtlasOS — tests/host/check.h
 * Assertions minimales pour les tests unitaires hébergés.
 */
#ifndef SAMOS_HOST_CHECK_H
#define SAMOS_HOST_CHECK_H
#include <stdio.h>
static int g_host_checks, g_host_failures;
#define CHECK(c) do { g_host_checks++; if (!(c)) { g_host_failures++; \
    fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_REPORT(name) (printf("[%s] %s: %d checks, %d failures\n", \
    g_host_failures ? "FAIL" : "PASS", name, g_host_checks, g_host_failures), g_host_failures != 0)
#endif
