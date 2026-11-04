#ifndef FILETYPE_U2A_H
#define FILETYPE_U2A_H

#include "browsable_root.h"
#include "filetypes.h"

class SubsysCommand;

class FileTypeU2A : public FileType
{
    BrowsableDirEntry *node;
    static SubsysResultCode_e execute_st(SubsysCommand *cmd);

  public:
    FileTypeU2A(BrowsableDirEntry *n);
    ~FileTypeU2A();

    int fetch_context_items(IndexedList<Action *> &list);
    static FileType *test_type(BrowsableDirEntry *obj);
};

#endif
