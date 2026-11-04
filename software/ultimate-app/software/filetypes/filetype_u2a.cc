/*
 * filetype_u2a.cc
 *
 * Registers ".u2a" files with the File Browser and exposes a "Run App"
 * action that dispatches to the AppLoader subsystem.
 */

#include <string.h>

#include "app_loader.h"
#include "directory.h"
#include "filemanager.h"
#include "filetype_u2a.h"
#include "subsys.h"

/* Self-registration: tells the file-type factory to test every directory
 * entry against FileTypeU2A::test_type. */
FactoryRegistrator<BrowsableDirEntry *, FileType *> tester_u2a(FileType::getFileTypeFactory(), FileTypeU2A::test_type);

#define U2AFILE_RUN 0x2A01

FileTypeU2A::FileTypeU2A(BrowsableDirEntry *n) : node(n) {}
FileTypeU2A::~FileTypeU2A() {}

FileType *FileTypeU2A::test_type(BrowsableDirEntry *obj)
{
    FileInfo *inf = obj->getInfo();
    if (strcmp(inf->extension, "U2A") == 0) {
        return new FileTypeU2A(obj);
    }
    return NULL;
}

int FileTypeU2A::fetch_context_items(IndexedList<Action *> &list)
{
    list.append(new Action("Run App", FileTypeU2A::execute_st, U2AFILE_RUN, 0));
    return 1;
}

SubsysResultCode_e FileTypeU2A::execute_st(SubsysCommand *cmd)
{
    switch (cmd->functionID) {
    case U2AFILE_RUN: {
        SubsysCommand *app_cmd = new SubsysCommand(cmd->user_interface, SUBSYSID_APP_LOADER, APP_CMD_RUN, 0,
                                                   cmd->path.c_str(), cmd->filename.c_str());
        SubsysResultCode_t r = app_cmd->execute();
        return r.status;
    }
    default:
        return SSRET_UNDEFINED_COMMAND;
    }
}
