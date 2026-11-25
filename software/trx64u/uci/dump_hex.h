// trx64u's UCI library (uci/, U18): a header of the firmware that the command interface server includes; no SDK ships it (MultiCore T239.3 D48).
// PROVENANCE: TREX/1541ultimate:software/system/dump_hex.h (1541ultimate, Gideon Zweijtzer, GPL-3.0; our fork at 0103c585), byte for byte the
// header TRX64U-MultiCore's host-app carries at t239.3 6e280710 (no SDK shipped it). Changes here: this header only.
/*
 *   1541 Ultimate - The Storage Solution for your Commodore computer.
 *   Copyright (C) 2009  Gideon Zweijtzer - Gideon's Logic Architectures
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   Abstract:
 *	 Simple routine to dump some hex values on the console.
 *
 */
#ifndef DUMPHEX_H
#define DUMPHEX_H

#include "integer.h"

#ifdef __cplusplus
extern "C" {
#endif

void dump_hex(const void *pp, int len);
void dump_hex_relative(const void *pp, int len);
void dump_hex_actual(const void *pp, int len, int relative);
void dump_hex_dirty(const void *p, int len, uint8_t ptrn);
void dump_hex_verify(const void *pp1, const void *pp2, int len);

#ifdef __cplusplus
}
#endif

#endif

