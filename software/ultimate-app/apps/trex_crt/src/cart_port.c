/*
 * cart_port.c
 *
 *  The installed platform (T95 D5). One pointer, set once before any tool runs.
 */

#include "cart_port.h"

const cart_port_t *g_port;

void cart_port_set(const cart_port_t *p) { g_port = p; }
