/*
 * path_picker.h
 *
 * Modal path picker: browse directories visually or type a path.
 * Cursor keys navigate the tree; any printable key switches to text input.
 *
 * With a pattern it picks a file as well as a folder (U9 D3d): the files
 * that match are listed beside the directories, and taking one returns its
 * full path. Without a pattern only directories are shown, which is what
 * Copy/Move wants.
 */
#ifndef PATH_PICKER_H
#define PATH_PICKER_H

#include "ui_elements.h"
#include "browsable.h"
#include "filemanager.h"

class UIPathPicker : public UIObject
{
    // Result buffer (caller-owned)
    char *result_buf;
    int   result_max;

    // UI elements
    Window   *window;
    Screen   *screen;
    Keyboard *keyboard;

    // What the box is for
    const char *title;
    const char *pattern;    // NULL: directories only

    // Tree browsing state
    FileManager *fm;
    Path *nav_path;
    IndexedList<FileInfo *> *dir_list;
    int  dir_count;
    int  selected;
    int  scroll_offset;
    int  tree_rows;      // visible rows for the tree
    int  tree_width;

    // Mode
    bool text_mode;      // false = tree, true = text edit
    UIStringEdit edit;   // reuse existing string edit widget

    // Internal helpers
    void load_directory(void);
    void draw_tree(void);
    void draw_path_line(void);
    void enter_text_mode(int first_key);
    void update_result(void);
    FileInfo *entry_at(int idx);   // NULL for the "this folder" row

public:
    UIPathPicker(UserInterface *ui, char *buf, int maxlen,
                 const char *title = NULL, const char *pattern = NULL,
                 const char *start = NULL);
    ~UIPathPicker();

    void init();
    void deinit(void);
    int  poll(int);
};

#endif
