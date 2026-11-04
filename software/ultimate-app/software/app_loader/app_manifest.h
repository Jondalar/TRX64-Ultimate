/*
 * app_manifest.h
 *
 * U64 App Loader - the JSON manifest embedded in .u2a files (doc/AppLoader_Spec.md section 2).
 * The same structure describes an app in the registry.
 */

#ifndef APP_LOADER_APP_MANIFEST_H_
#define APP_LOADER_APP_MANIFEST_H_

class JSON_Object;

#define APP_MANIFEST_ID_LEN 31
#define APP_MANIFEST_NAME_LEN 27
#define APP_MANIFEST_VERSION_LEN 15
#define APP_MANIFEST_EXT_LEN 7
#define APP_MANIFEST_CATEGORY_LEN 31
#define APP_MANIFEST_LABEL_LEN 31
#define APP_MANIFEST_ACTION_LEN 15
#define APP_MANIFEST_MAX_FILETYPES 8
#define APP_MANIFEST_MAX_TASKS 8
#define APP_MANIFEST_MAX_REST 8

struct AppFileType
{
    char ext[APP_MANIFEST_EXT_LEN + 1]; // without dot, compared ignoring case
    char label[APP_MANIFEST_LABEL_LEN + 1];
    char action[APP_MANIFEST_ACTION_LEN + 1];
};

struct AppTask
{
    char category[APP_MANIFEST_CATEGORY_LEN + 1];
    char label[APP_MANIFEST_LABEL_LEN + 1];
    char action[APP_MANIFEST_ACTION_LEN + 1];
};

struct AppManifest
{
    char id[APP_MANIFEST_ID_LEN + 1];
    char name[APP_MANIFEST_NAME_LEN + 1];
    char version[APP_MANIFEST_VERSION_LEN + 1];
    int num_filetypes;
    AppFileType filetypes[APP_MANIFEST_MAX_FILETYPES];
    int num_tasks;
    AppTask tasks[APP_MANIFEST_MAX_TASKS];
    int num_rest; // actions PUT /v1/apps/{id}:run may start; an app without any cannot be started that way
    char rest[APP_MANIFEST_MAX_REST][APP_MANIFEST_ACTION_LEN + 1];
};

// Fills 'out' from a JSON object with the manifest keys (id, name, version, filetypes, tasks, rest).
// Unknown keys are ignored. Returns NULL if the object follows the manifest rules, otherwise a
// short description of the first violation.
const char *app_manifest_from_json(JSON_Object *obj, AppManifest &out);

// Parses and validates a manifest of 'size' bytes, including the terminating NUL.
// Returns NULL on success, otherwise a short description of the problem.
const char *app_manifest_parse(const char *text, int size, AppManifest &out);

#endif /* APP_LOADER_APP_MANIFEST_H_ */
