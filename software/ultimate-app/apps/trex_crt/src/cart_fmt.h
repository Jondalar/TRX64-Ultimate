/*
 * cart_fmt.h
 *
 *  String formatting for the cartridge tools (T95 D5): no UI, no platform, no libc.
 */

#ifndef CART_FMT_H
#define CART_FMT_H

#include <stdarg.h>
#include <stdbool.h>

// Supports %d %u %x %X %s %c %%, a '0' flag, a width and a precision for %s
int cart_snprintf(char *buf, int size, const char *fmt, ...);
int cart_vsnprintf(char *buf, int size, const char *fmt, va_list ap);

#endif
