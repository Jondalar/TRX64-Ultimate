/*
 * cart_view_text.h
 *
 *  The 40 x 22 character screen of the cartridge tools (T95 D5): the only consumer of cart_screen.c and the
 *  only place that knows a key code or an SDK dialog. Each entry opens the screen, installs itself as the view
 *  (cart_view.h), runs the tool's core and closes both.
 */

#ifndef CART_VIEW_TEXT_H
#define CART_VIEW_TEXT_H

void cart_view_text_writer(const char *path, const char *filename);
void cart_view_text_dumper(void);
void cart_view_text_inspector(void);

#endif
