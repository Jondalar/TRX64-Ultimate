/*
 * app_registry.cc
 *
 * U64 App Loader - keeps the registry of apps with a manifest in /Flash/config/apps.json and wires
 * the registered apps into the file browser context menu and the task menu
 * (doc/AppLoader_Spec.md section 3).
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "action.h"
#include "app_api.h"
#include "app_format.h"
#include "app_loader.h"
#include "app_registry.h"
#include "browsable_root.h"
#include "filemanager.h"
#include "init_function.h"
#include "json.h"
#include "pattern.h"
#include "tasks_collection.h"
#include "userinterface.h"

#define APP_REGISTRY_DIR "/Flash/config"
#define APP_REGISTRY_FILE "apps.json"
#define APP_FLASH_DIR "/Flash/apps"
#define APP_REGISTRY_MAX_SIZE (APP_REGISTRY_MAX_APPS * (U2A_MANIFEST_MAX_SIZE + 512))
#define APP_TASK_CATEGORY_ORDER 900
#define APP_COMPARE_CHUNK_SIZE 4096

// A scratch AppManifest on the heap. It is 1.3 KB, and the refresh runs on the stack of whatever task asks for it:
// the HTTP listener has 1600 words, and a refresh there that found a changed manifest (frames of the route, of
// lookup_fresh(), refresh(), scan_flash() and app_read_manifest(), then save() and the file system on top) overran
// it and corrupted the heap next to it (T67). The boot refresh runs on a big stack and never showed it.
class ManifestBuffer
{
    AppManifest *p;

  public:
    ManifestBuffer() : p(new AppManifest) {}
    ~ManifestBuffer() { delete p; }
    ManifestBuffer(const ManifestBuffer &) = delete;
    ManifestBuffer &operator=(const ManifestBuffer &) = delete;
    AppManifest *get(void) { return p; }
    AppManifest &operator*(void) { return *p; }
    AppManifest *operator->(void) { return p; }
};

// A task menu action of an app. Task actions are persistent, so they are kept for the whole run
// time and hidden when their app no longer declares them.
struct AppTaskItem
{
    AppActionRef *ref;
    char category[APP_MANIFEST_CATEGORY_LEN + 1];
    char label[APP_MANIFEST_LABEL_LEN + 1];
    Action *action;
};

static AppRegistry *app_registry = NULL;

InitFunction app_registry_init("App Registry", AppRegistry::init, NULL, NULL, 32); // after the Flash disk mount

static bool is_on_flash(const char *path) { return strncasecmp(path, "/Flash/", 7) == 0; }

AppRegistry::AppRegistry() : entries(8, NULL), refs(16, NULL), task_items(8, NULL)
{
    task_menu_created = false;
    mutex = xSemaphoreCreateRecursiveMutex();
}

// Guards 'entries' and the entries themselves between the UI task, the HTTP task and the launch paths. Recursive,
// because the locked functions call each other. Never held while a popup is open or an app runs.
void AppRegistry::lock(void) { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
void AppRegistry::unlock(void) { xSemaphoreGiveRecursive(mutex); }

AppRegistry *AppRegistry::getRegistry(void) { return app_registry; }

void AppRegistry::init(void *obj, void *param)
{
    if (app_registry) {
        return;
    }
    app_registry = new AppRegistry();
    app_registry->load();
    app_registry->refresh(true);
    BrowsableDirEntry::context_items_hook() = AppRegistry::add_context_items;
}

AppRegistryEntry *AppRegistry::find(const char *id)
{
    for (int i = 0; i < entries.get_elements(); i++) {
        if (strcmp(entries[i]->manifest.id, id) == 0) {
            return entries[i];
        }
    }
    return NULL;
}

AppActionRef *AppRegistry::get_ref(const char *id, const char *action)
{
    for (int i = 0; i < refs.get_elements(); i++) {
        if ((strcmp(refs[i]->id, id) == 0) && (strcmp(refs[i]->action, action) == 0)) {
            return refs[i];
        }
    }
    AppActionRef *ref = new AppActionRef;
    strcpy(ref->id, id);
    strcpy(ref->action, action);
    refs.append(ref);
    return ref;
}

/* Persistence                                                         */

void AppRegistry::load(void)
{
    FileManager *fm = FileManager::getFileManager();
    File *f = NULL;
    if ((fm->fopen(APP_REGISTRY_DIR, APP_REGISTRY_FILE, FA_READ, &f) != FR_OK) || !f) {
        return;
    }
    uint32_t size = f->get_size32();
    uint32_t transferred = 0;
    char *buffer = NULL;
    if ((size >= 2) && (size <= APP_REGISTRY_MAX_SIZE)) {
        buffer = new char[size + 1];
        if (f->read(buffer, size, &transferred) != FR_OK) {
            transferred = 0;
        }
        buffer[transferred] = 0;
    }
    fm->fclose(f);
    if (!buffer) {
        printf("AppRegistry: %s/%s has an invalid size\n", APP_REGISTRY_DIR, APP_REGISTRY_FILE);
        return;
    }

    // A JSON text of n bytes holds at most (n + 1) / 2 tokens.
    JSON *root = NULL;
    JSON *apps = NULL;
    if (transferred >= 2) {
        int tokens = convert_text_to_json_objects(buffer, transferred, transferred / 2 + 1, &root);
        if ((tokens > 0) && root && (root->type() == eObject)) {
            apps = ((JSON_Object *)root)->get("apps");
        }
    }
    if (apps && (apps->type() == eList)) {
        JSON_List *list = (JSON_List *)apps;
        for (int i = 0; (i < list->get_num_elements()) && (entries.get_elements() < APP_REGISTRY_MAX_APPS); i++) {
            JSON *item = (*list)[i];
            if (!item || (item->type() != eObject)) {
                continue;
            }
            AppRegistryEntry *entry = new AppRegistryEntry;
            const char *error = app_manifest_from_json((JSON_Object *)item, entry->manifest);
            const char *path = ((JSON_Object *)item)->string_or("path", "");
            if (!error && (!path[0] || find(entry->manifest.id))) {
                error = "no path or duplicate id";
            }
            if (error) {
                printf("AppRegistry: skipping entry %d (%s)\n", i, error);
                delete entry;
                continue;
            }
            entry->path = path;
            entries.append(entry);
        }
    }
    delete root;
    delete[] buffer;
    printf("AppRegistry: %d app(s) registered\n", entries.get_elements());
}

bool AppRegistry::save(void)
{
    FileManager *fm = FileManager::getFileManager();
    fm->create_dir(APP_REGISTRY_DIR); // /Flash always exists, so this is the only missing level
    File *f = NULL;
    FRESULT fres = fm->fopen(APP_REGISTRY_DIR, APP_REGISTRY_FILE, FA_WRITE | FA_CREATE_ALWAYS, &f);
    if ((fres != FR_OK) || !f) {
        printf("AppRegistry: cannot write %s/%s (%d)\n", APP_REGISTRY_DIR, APP_REGISTRY_FILE, fres);
        return false;
    }

    JSON_List *apps = JSON::List();
    JSON_Object *root = JSON::Obj()->add("version", 1)->add("apps", apps);
    for (int i = 0; i < entries.get_elements(); i++) {
        AppManifest &m = entries[i]->manifest;
        JSON_List *filetypes = JSON::List();
        for (int j = 0; j < m.num_filetypes; j++) {
            AppFileType &ft = m.filetypes[j];
            filetypes->add(JSON::Obj()->add("ext", ft.ext)->add("label", ft.label)->add("action", ft.action));
        }
        JSON_List *tasks = JSON::List();
        for (int j = 0; j < m.num_tasks; j++) {
            AppTask &t = m.tasks[j];
            tasks->add(JSON::Obj()->add("category", t.category)->add("label", t.label)->add("action", t.action));
        }
        JSON_List *rest = JSON::List();
        for (int j = 0; j < m.num_rest; j++) {
            rest->add(m.rest[j]);
        }
        apps->add(JSON::Obj()
                      ->add("id", m.id)
                      ->add("name", m.name)
                      ->add("version", m.version)
                      ->add("path", entries[i]->path.c_str())
                      ->add("filetypes", filetypes)
                      ->add("tasks", tasks)
                      ->add("rest", rest));
    }

    const char *text = root->render();
    uint32_t len = strlen(text);
    uint32_t transferred = 0;
    fres = f->write(text, len, &transferred);
    delete root;
    fm->fclose(f);
    if ((fres != FR_OK) || (transferred != len)) {
        printf("AppRegistry: writing %s/%s failed (%d)\n", APP_REGISTRY_DIR, APP_REGISTRY_FILE, fres);
        return false;
    }
    return true;
}

/* Refresh against the files (doc/AppLoader_Spec.md section 3.5)       */

static bool file_exists(const char *path)
{
    FileInfo info(INFO_SIZE);
    return FileManager::getFileManager()->fstat(path, info) == FR_OK;
}

// True if the drive the path lies on is there: "/USB0/x.u2a" asks for "/USB0". A medium that is not mounted
// (yet) is not a reason to forget the app.
static bool medium_present(const char *path)
{
    if (is_on_flash(path)) {
        return true;
    }
    const char *end = strchr(path + 1, '/');
    if (!end) {
        return true;
    }
    mstring root(path, 0, end - path - 1);
    return file_exists(root.c_str());
}

// Compares an entry with its file. 'm' receives the manifest of the file when it could be read.
AppRegistry::Check AppRegistry::check_entry(AppRegistryEntry *entry, AppManifest &m)
{
    const char *path = entry->path.c_str();
    if (!file_exists(path)) {
        return medium_present(path) ? CHECK_GONE : CHECK_NO_MEDIUM;
    }
    const char *error = app_read_manifest(path, m);
    if (error) {
        printf("AppRegistry: %s is not a valid app (%s)\n", path, error);
        return CHECK_INVALID;
    }
    if (strcmp(m.id, entry->manifest.id) != 0) {
        return CHECK_OTHER_APP;
    }
    return CHECK_OK;
}

// Drops the entry at index 'i'. The registry never frees AppActionRefs, so menu actions stay valid.
void AppRegistry::drop_entry(int i, const char *why)
{
    AppRegistryEntry *entry = entries[i];
    printf("AppRegistry: dropping %s (%s): %s\n", entry->manifest.id, entry->path.c_str(), why);
    entries.remove(entry);
    delete entry;
}

// Registers every valid app file in /Flash/apps whose id is not known yet. Returns true if one was added.
bool AppRegistry::scan_flash(void)
{
    FileManager *fm = FileManager::getFileManager();
    Directory *dir = NULL;
    if ((fm->open_directory(APP_FLASH_DIR, &dir) != FR_OK) || !dir) {
        return false;
    }
    bool added = false;
    ManifestBuffer m; // not on the stack: see ManifestBuffer
    FileInfo info(INFO_SIZE);
    while (dir->get_entry(info) == FR_OK) {
        const char *dot = strrchr(info.lfname, '.');
        if ((info.attrib & (AM_DIR | AM_VOL | AM_HID)) || (info.lfname[0] == '.') || !dot ||
            (strcasecmp(dot, ".u2a") != 0)) {
            continue;
        }
        mstring path(APP_FLASH_DIR "/");
        path += info.lfname;
        bool known = false;
        for (int i = 0; !known && (i < entries.get_elements()); i++) {
            known = (strcasecmp(entries[i]->path.c_str(), path.c_str()) == 0);
        }
        if (known) {
            continue;
        }
        const char *error = app_read_manifest(path.c_str(), *m);
        if (error) {
            printf("AppRegistry: ignoring %s (%s)\n", path.c_str(), error);
            continue;
        }
        if (find(m->id)) {
            printf("AppRegistry: %s has the id %s of %s, ignored\n", path.c_str(), m->id, find(m->id)->path.c_str());
            continue;
        }
        if (entries.get_elements() >= APP_REGISTRY_MAX_APPS) {
            printf("AppRegistry: too many apps, ignoring %s\n", path.c_str());
            continue;
        }
        AppRegistryEntry *entry = new AppRegistryEntry;
        entry->manifest = *m;
        entry->path = path;
        entries.append(entry);
        printf("AppRegistry: found %s %s at %s\n", m->id, m->version, path.c_str());
        added = true;
    }
    delete dir;
    return added;
}

// Makes the registry match the files. Returns true if it changed (then it is saved and the menus updated).
// 'boot': only /Flash entries are checked, because USB and SD mount later than this runs, and a file that is
// there but invalid is dropped. Otherwise (a launch found something stale) every entry is checked, but an
// invalid file is kept (it may be half written) and so is an entry whose medium is not there.
bool AppRegistry::refresh(bool boot)
{
    lock();
    bool changed = false;
    ManifestBuffer m;
    for (int i = entries.get_elements() - 1; i >= 0; i--) {
        AppRegistryEntry *entry = entries[i];
        if (boot && !is_on_flash(entry->path.c_str())) {
            continue;
        }
        Check c = check_entry(entry, *m);
        if ((c == CHECK_GONE) || (c == CHECK_OTHER_APP) || (boot && (c == CHECK_INVALID))) {
            drop_entry(i, (c == CHECK_GONE) ? "file is gone" : (c == CHECK_OTHER_APP) ? "the file holds another app now" : "invalid");
            changed = true;
        } else if ((c == CHECK_OK) && (memcmp(m.get(), &entry->manifest, sizeof(AppManifest)) != 0)) {
            printf("AppRegistry: manifest of %s changed\n", m->id);
            entry->manifest = *m;
            changed = true;
        }
    }
    if (scan_flash()) {
        changed = true;
    }
    if (changed) {
        save();
        update_tasks();
    }
    printf("AppRegistry: %d app(s) after refresh%s\n", entries.get_elements(), changed ? ", changed" : "");
    unlock();
    return changed;
}

// find() for a launch: the entry is checked against its file first, and the registry refreshes once if it is stale
// or unknown. Returns NULL with 'check' telling why: CHECK_NO_MEDIUM (drive not there, entry kept), CHECK_INVALID
// (file there but not a valid app, e.g. still being written; entry kept), CHECK_GONE (no such app). The caller holds
// lock() while it uses the entry.
AppRegistryEntry *AppRegistry::lookup_fresh(const char *id, Check &check)
{
    lock();
    check = CHECK_GONE;
    AppRegistryEntry *entry = find(id);
    if (entry) {
        ManifestBuffer m;
        check = check_entry(entry, *m);
        if ((check == CHECK_NO_MEDIUM) || (check == CHECK_INVALID)) {
            unlock();
            return NULL;
        }
        if ((check == CHECK_OK) && !memcmp(m.get(), &entry->manifest, sizeof(AppManifest))) {
            unlock();
            return entry;
        }
    }
    refresh(false);
    entry = find(id);
    check = entry ? CHECK_OK : CHECK_GONE;
    unlock();
    return entry;
}

/* Registration                                                        */

// Copies the app file to 'flash_path' and overwrites a file that is already there. On success 'path'
// becomes 'flash_path'.
static void copy_to_flash(UserInterface *ui, const char *app_path, const char *flash_path, mstring &path)
{
    const char *slash = strrchr(app_path, '/');
    const char *flash_slash = strrchr(flash_path, '/');
    if (!slash || !slash[1] || !flash_slash || !flash_slash[1]) {
        return;
    }
    mstring dir(app_path, 0, slash - app_path - 1);
    mstring flash_dir(flash_path, 0, flash_slash - flash_path - 1);

    FileManager *fm = FileManager::getFileManager();
    fm->create_dir(flash_dir.c_str());
    FRESULT fres = fm->fcopy(dir.c_str(), slash + 1, flash_dir.c_str(), flash_slash + 1, true);
    if (fres != FR_OK) {
        printf("AppRegistry: copying %s to %s failed (%d)\n", app_path, flash_path, fres);
        if (ui) {
            ui->popup("Copy to Flash failed.", BUTTON_OK);
        }
        return;
    }
    path = flash_path;
}

// Returns true when both files can be opened and have the same size and content.
static bool same_file_content(const char *path_a, const char *path_b)
{
    FileManager *fm = FileManager::getFileManager();
    File *a = NULL;
    File *b = NULL;
    fm->fopen(path_a, FA_READ, &a);
    fm->fopen(path_b, FA_READ, &b);
    bool same = a && b && (a->get_size() == b->get_size());
    if (same) {
        uint8_t *buf_a = new uint8_t[APP_COMPARE_CHUNK_SIZE];
        uint8_t *buf_b = new uint8_t[APP_COMPARE_CHUNK_SIZE];
        uint64_t remaining = a->get_size();
        while (same && (remaining > 0)) {
            uint32_t len = (remaining < APP_COMPARE_CHUNK_SIZE) ? (uint32_t)remaining : APP_COMPARE_CHUNK_SIZE;
            uint32_t read_a = 0;
            uint32_t read_b = 0;
            same = (a->read(buf_a, len, &read_a) == FR_OK) && (read_a == len) &&
                   (b->read(buf_b, len, &read_b) == FR_OK) && (read_b == len) && (memcmp(buf_a, buf_b, len) == 0);
            remaining -= len;
        }
        delete[] buf_a;
        delete[] buf_b;
    }
    if (a) {
        fm->fclose(a);
    }
    if (b) {
        fm->fclose(b);
    }
    return same;
}

void AppRegistry::register_app(UserInterface *ui, const char *app_path, const AppManifest &manifest)
{
    // Popups wait for the user, so the lock is not held over them: the entry is copied first and looked up again after.
    lock();
    AppRegistryEntry *known = find(manifest.id);
    bool had_entry = known != NULL;
    mstring entry_path(known ? known->path.c_str() : "");
    unlock();
    mstring path(app_path);
    if (!is_on_flash(app_path)) {
        // A copy on Flash that was deleted counts as no copy. Manifest versions are not compared,
        // because apps do not change them reliably.
        FileInfo info(INFO_SIZE);
        bool has_copy = had_entry && is_on_flash(entry_path.c_str()) &&
                        (FileManager::getFileManager()->fstat(entry_path.c_str(), info) == FR_OK);
        char msg[80];
        if (!has_copy) {
            if (ui) {
                snprintf(msg, sizeof(msg), "Copy %s to Flash\nso it stays available?", manifest.name);
                if (ui->popup(msg, BUTTON_YES | BUTTON_NO) == BUTTON_YES) {
                    mstring flash_path(APP_FLASH_DIR "/");
                    flash_path += get_filename(app_path);
                    copy_to_flash(ui, app_path, flash_path.c_str(), path);
                }
            }
        } else if (!ui || same_file_content(app_path, entry_path.c_str())) {
            path = entry_path; // keep the copy on Flash
        } else {
            snprintf(msg, sizeof(msg), "Update %s on Flash\nwith this version?", manifest.name);
            if (ui->popup(msg, BUTTON_YES | BUTTON_NO) == BUTTON_YES) {
                copy_to_flash(ui, app_path, entry_path.c_str(), path); // keeps the run path if the copy fails
            } else {
                path = entry_path;
            }
        }
    }

    lock();
    AppRegistryEntry *entry = find(manifest.id);
    if (!entry) {
        if (entries.get_elements() >= APP_REGISTRY_MAX_APPS) {
            unlock();
            if (ui) {
                ui->popup("Too many apps registered.", BUTTON_OK);
            }
            return;
        }
        entry = new AppRegistryEntry;
        entries.append(entry);
    }
    entry->manifest = manifest;
    entry->path = path;
    printf("AppRegistry: registered %s %s at %s\n", manifest.id, manifest.version, path.c_str());
    save();
    update_tasks();
    unlock();
}

/* Menus                                                               */

void AppRegistry::add_context_items(BrowsableDirEntry *br, IndexedList<Action *> &items)
{
    AppRegistry *reg = app_registry;
    FileInfo *info = br->getInfo();
    if (!reg || !info || (info->attrib & (AM_DIR | AM_VOL))) {
        return;
    }
    // FileInfo::extension holds three characters at most, so longer extensions come from the name.
    const char *dot = info->lfname ? strrchr(info->lfname, '.') : NULL;
    reg->lock();
    for (int i = 0; i < reg->entries.get_elements(); i++) {
        AppManifest &m = reg->entries[i]->manifest;
        for (int j = 0; j < m.num_filetypes; j++) {
            AppFileType &ft = m.filetypes[j];
            if ((strcasecmp(ft.ext, info->extension) == 0) || (dot && (strcasecmp(ft.ext, dot + 1) == 0))) {
                items.append(new Action(ft.label, AppRegistry::S_run_filetype, 0, 0, reg->get_ref(m.id, ft.action)));
            }
        }
    }
    reg->unlock();
}

// Shows a task action for every task of every registered app and hides the others (section 3.3).
void AppRegistry::update_tasks(void)
{
    // Built-in categories are created when the task menu opens for the first time. Adding app
    // tasks before that would create a built-in category with the app category order.
    if (!task_menu_created) {
        return;
    }
    lock();
    for (int i = 0; i < task_items.get_elements(); i++) {
        task_items[i]->action->hide();
    }
    IndexedList<TaskCategory *> *categories = TasksCollection::getCategories();
    int num_categories = categories->get_elements();

    for (int i = 0; i < entries.get_elements(); i++) {
        AppManifest &m = entries[i]->manifest;
        for (int j = 0; j < m.num_tasks; j++) {
            AppTask &t = m.tasks[j];
            AppTaskItem *item = NULL;
            for (int k = 0; k < task_items.get_elements(); k++) {
                AppTaskItem *it = task_items[k];
                if ((strcmp(it->ref->id, m.id) == 0) && (strcmp(it->ref->action, t.action) == 0) &&
                    (strcmp(it->category, t.category) == 0) && (strcmp(it->label, t.label) == 0)) {
                    item = it;
                    break;
                }
            }
            if (!item) {
                item = new AppTaskItem;
                item->ref = get_ref(m.id, t.action);
                strcpy(item->category, t.category);
                strcpy(item->label, t.label);
                item->action = new Action(t.label, AppRegistry::S_run_task, 0, 0, item->ref);
                item->action->setPersistent();
                TasksCollection::getCategory(t.category, APP_TASK_CATEGORY_ORDER)->append(item->action);
                task_items.append(item);
            }
            item->action->show();
        }
    }
    if (categories->get_elements() != num_categories) {
        TasksCollection::sort();
    }
    unlock();
}

void AppRegistry::update_task_items(bool writablePath)
{
    if (!task_menu_created) {
        task_menu_created = true;
        update_tasks();
    }
}

/* Launch from a registry entry                                        */

SubsysResultCode_e AppRegistry::S_run_filetype(SubsysCommand *cmd) { return launch(cmd, true); }

SubsysResultCode_e AppRegistry::S_run_task(SubsysCommand *cmd) { return launch(cmd, false); }

// U5: the TRX64 monitor is no longer an app. It runs in the firmware as the debug service
// (software/io/c64/trx_service.cc), which owns the stops whether a screen is open or not, so
// C= + O in the file browser and in Timewarp call that and nothing is launched from the registry.
// The monitor was the only caller of a task launch by id, so that entry point is gone; a task
// still starts from the Developer menu through S_run_task.

SubsysResultCode_e AppRegistry::launch(SubsysCommand *cmd, bool from_browser)
{
    AppRegistry *reg = app_registry;
    const AppActionRef *ref = (const AppActionRef *)cmd->direct_obj;
    UserInterface *ui = cmd->user_interface;

    // The app may have been removed or re-registered since the menu was built. The entry is only used under the
    // lock; what the launch needs is copied.
    bool found = false;
    mstring entry_path;
    if (reg && ref) {
        reg->lock();
        AppRegistryEntry *entry = reg->find(ref->id);
        if (entry) {
            AppManifest &m = entry->manifest;
            for (int i = 0; from_browser && !found && (i < m.num_filetypes); i++) {
                found = (strcmp(m.filetypes[i].action, ref->action) == 0);
            }
            for (int i = 0; !from_browser && !found && (i < m.num_tasks); i++) {
                found = (strcmp(m.tasks[i].action, ref->action) == 0);
            }
            entry_path = entry->path;
        }
        reg->unlock();
    }
    if (!found) {
        if (ui) {
            ui->popup("App not found.", BUTTON_OK);
        }
        return SSRET_INVALID_PARAMETER;
    }

    if (!file_exists(entry_path.c_str())) {
        if (!medium_present(entry_path.c_str())) {
            printf("AppRegistry: medium of %s not present\n", entry_path.c_str());
            if (ui) {
                ui->popup("Medium not present.", BUTTON_OK);
            }
            return SSRET_CANNOT_OPEN_FILE;
        }
        printf("AppRegistry: %s not found, refreshing\n", entry_path.c_str());
        if (ui) {
            ui->popup("App not found, entry removed.", BUTTON_OK);
        }
        reg->refresh(false); // drops the entry, and finds the app again if a copy lies in /Flash/apps
        return SSRET_CANNOT_OPEN_FILE;
    }

    // Split the app path into directory and file name for the App Loader command.
    const char *app_path = entry_path.c_str();
    const char *slash = strrchr(app_path, '/');
    if (!slash || (slash == app_path) || !slash[1]) {
        return SSRET_INVALID_PARAMETER;
    }
    int dir_len = slash - app_path;
    char *dir = new char[dir_len + 1];
    memcpy(dir, app_path, dir_len);
    dir[dir_len] = 0;

    mstring selected_file;
    if (from_browser) {
        selected_file = cmd->path;
        if ((selected_file.length() > 0) && (selected_file[selected_file.length() - 1] != '/')) {
            selected_file += "/";
        }
        selected_file += cmd->filename;
    }

    AppLaunchParams params = {};
    params.selected_file = from_browser ? selected_file.c_str() : NULL;
    params.launch_source = from_browser ? U2P_LAUNCH_BROWSER : U2P_LAUNCH_MENU;
    params.action = ref->action; // references are never freed
    // U4 D5: a task launch does not build a selected file, so the command's file name carries a
    // value to the app as ctx->args. The menu items send none; the field is for a caller that
    // builds the command itself.
    const char *task_args = cmd->filename.c_str();
    params.args = (from_browser || !task_args || !task_args[0]) ? NULL : task_args;

    SubsysCommand *run = new SubsysCommand(ui, SUBSYSID_APP_LOADER, APP_CMD_RUN_ACTION, 0, dir, slash + 1);
    run->buffer = &params;
    run->bufferSize = sizeof(params);
    SubsysResultCode_t result = run->execute();
    delete[] dir;
    return result.status;
}
