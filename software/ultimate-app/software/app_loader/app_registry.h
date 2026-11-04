/*
 * app_registry.h
 *
 * U64 App Loader - the registry of apps with a manifest (doc/AppLoader_Spec.md section 3).
 * It lives in /Flash/config/apps.json and adds the apps' file types to the file browser
 * context menu and their tasks to the task menu.
 */

#ifndef APP_LOADER_APP_REGISTRY_H_
#define APP_LOADER_APP_REGISTRY_H_

#include "FreeRTOS.h"
#include "semphr.h"
#include "app_manifest.h"
#include "indexed_list.h"
#include "menu.h"
#include "mystring.h"
#include "subsys.h"

#define APP_REGISTRY_MAX_APPS 32

class Action;
class BrowsableDirEntry;
class UserInterface;

struct AppRegistryEntry
{
    AppManifest manifest;
    mstring path; // full path of the .u2a file
};

// Names one action of one app. Menu actions point to such a reference instead of registry
// entries; the registry never frees them, so a menu action stays valid when its app is removed.
struct AppActionRef
{
    char id[APP_MANIFEST_ID_LEN + 1];
    char action[APP_MANIFEST_ACTION_LEN + 1];
};

struct AppTaskItem;

class AppRegistry : public ObjectWithMenu
{
  public:
    enum Check
    {
        CHECK_OK,
        CHECK_GONE,      // no file, and its medium is there
        CHECK_NO_MEDIUM, // no file, because the drive is not there (USB/SD not mounted)
        CHECK_INVALID,   // a file that is not a valid app (maybe still being written)
        CHECK_OTHER_APP  // a valid app with another id
    };

  private:
    IndexedList<AppRegistryEntry *> entries;
    IndexedList<AppActionRef *> refs;
    IndexedList<AppTaskItem *> task_items;
    bool task_menu_created;

    AppRegistry();
    void load(void);
    bool save(void);
    AppRegistryEntry *find(const char *id);
    AppActionRef *get_ref(const char *id, const char *action);
    void update_tasks(void);
    Check check_entry(AppRegistryEntry *entry, AppManifest &m);
    void drop_entry(int i, const char *why);
    bool scan_flash(void);
    SemaphoreHandle_t mutex;

    static void add_context_items(BrowsableDirEntry *entry, IndexedList<Action *> &items);
    static SubsysResultCode_e S_run_filetype(SubsysCommand *cmd);
    static SubsysResultCode_e S_run_task(SubsysCommand *cmd);
    static SubsysResultCode_e launch(SubsysCommand *cmd, bool from_browser);

  public:
    // NULL until the registry has been loaded at boot.
    static AppRegistry *getRegistry(void);
    static void init(void *obj, void *param);

    // Registers the app at 'app_path' after a plain "Run App" (section 3.1). Offers to copy the
    // app to /Flash, or to update a different copy there, then saves the registry and updates the
    // task menu.
    void register_app(UserInterface *ui, const char *app_path, const AppManifest &manifest);

    // ObjectWithMenu: called each time the task menu opens, after all built-in task items exist.
    void update_task_items(bool writablePath);

    // The registered apps, for the REST routes. Entries stay valid until the next register_app().
    int get_count(void) { return entries.get_elements(); }
    AppRegistryEntry *get_entry(int i) { return entries[i]; }
    AppRegistryEntry *lookup(const char *id) { return find(id); }

    // Guards the entries (and pointers to them) between tasks. Recursive. The routes hold it while they use an
    // entry; launches and popups never hold it.
    void lock(void);
    void unlock(void);

    // Makes the registry match the files (doc/AppLoader_Spec.md 3.5). 'boot': only /Flash entries are checked.
    // Returns true if something changed. Invalidates entry pointers: call with lock() held and look up again.
    bool refresh(bool boot);

    // lookup() for a launch: checks the entry against its file first and refreshes once if it is stale or
    // unknown. NULL if the app is not there; 'check' tells why (not mounted / invalid file keep the entry).
    // Call with lock() held while the entry is in use.
    AppRegistryEntry *lookup_fresh(const char *id, Check &check);
};

#endif /* APP_LOADER_APP_REGISTRY_H_ */
