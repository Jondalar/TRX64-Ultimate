#ifndef FILETYPE_C64RE_H
#define FILETYPE_C64RE_H

#include "filetypes.h"
#include "browsable_root.h"

// A .c64re snapshot or a .c64rering ring save in the file browser (T66,
// docs/specs/T20-delta-ring.md §9.4): "Load Snapshot" puts the machine into
// the state the file holds and lets it run from there.
class FileTypeC64re : public FileType
{
    BrowsableDirEntry *node;
    int type;
public:
    FileTypeC64re(BrowsableDirEntry *par, int type);
    ~FileTypeC64re();

    int   fetch_context_items(IndexedList<Action *> &list);
    static FileType *test_type(BrowsableDirEntry *obj);

    static SubsysResultCode_e execute_st(SubsysCommand *cmd);
};

#endif
