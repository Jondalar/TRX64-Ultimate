/*
 * app_manifest.cc
 *
 * U64 App Loader - parses and validates app manifests with the firmware JSON classes.
 */

#include <string.h>

#include "app_format.h"
#include "app_manifest.h"
#include "json.h"

// [a-z0-9_]
static bool is_identifier(const char *s)
{
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '_'))) {
            return false;
        }
    }
    return true;
}

// Copies the string under 'key' to 'dest'. Fails if the key is missing, not a string, empty or
// longer than 'max_len' bytes.
static bool copy_string(JSON_Object *obj, const char *key, char *dest, int max_len)
{
    JSON *value = obj->get(key);
    if (!value || (value->type() != eString)) {
        return false;
    }
    const char *s = ((JSON_String *)value)->get_string();
    int len = strlen(s);
    if ((len < 1) || (len > max_len)) {
        return false;
    }
    strcpy(dest, s);
    return true;
}

static bool copy_identifier(JSON_Object *obj, const char *key, char *dest, int max_len)
{
    return copy_string(obj, key, dest, max_len) && is_identifier(dest);
}

// Returns the list under 'key' in 'list' (NULL if the key is absent). Fails if the key holds
// something else than a list, or a list with more than 'max_entries' entries.
static bool get_list(JSON_Object *obj, const char *key, int max_entries, JSON_List *&list)
{
    list = NULL;
    JSON *value = obj->get(key);
    if (!value) {
        return true;
    }
    if (value->type() != eList) {
        return false;
    }
    list = (JSON_List *)value;
    return list->get_num_elements() <= max_entries;
}

const char *app_manifest_from_json(JSON_Object *obj, AppManifest &out)
{
    memset(&out, 0, sizeof(out));
    if (!obj) {
        return "not a JSON object";
    }
    if (!copy_identifier(obj, "id", out.id, APP_MANIFEST_ID_LEN)) {
        return "invalid id";
    }
    if (!copy_string(obj, "name", out.name, APP_MANIFEST_NAME_LEN)) {
        return "invalid name";
    }
    if (!copy_string(obj, "version", out.version, APP_MANIFEST_VERSION_LEN)) {
        return "invalid version";
    }

    JSON_List *list;
    if (!get_list(obj, "filetypes", APP_MANIFEST_MAX_FILETYPES, list)) {
        return "invalid filetypes";
    }
    for (int i = 0; list && (i < list->get_num_elements()); i++) {
        JSON *entry = (*list)[i];
        if (!entry || (entry->type() != eObject)) {
            return "invalid filetypes entry";
        }
        JSON_Object *o = (JSON_Object *)entry;
        AppFileType *ft = &out.filetypes[i];
        if (!copy_string(o, "ext", ft->ext, APP_MANIFEST_EXT_LEN) || strchr(ft->ext, '.')) {
            return "invalid filetypes ext";
        }
        if (!copy_string(o, "label", ft->label, APP_MANIFEST_LABEL_LEN)) {
            return "invalid filetypes label";
        }
        if (!copy_identifier(o, "action", ft->action, APP_MANIFEST_ACTION_LEN)) {
            return "invalid filetypes action";
        }
        out.num_filetypes++;
    }

    if (!get_list(obj, "tasks", APP_MANIFEST_MAX_TASKS, list)) {
        return "invalid tasks";
    }
    for (int i = 0; list && (i < list->get_num_elements()); i++) {
        JSON *entry = (*list)[i];
        if (!entry || (entry->type() != eObject)) {
            return "invalid tasks entry";
        }
        JSON_Object *o = (JSON_Object *)entry;
        AppTask *task = &out.tasks[i];
        if (!copy_string(o, "category", task->category, APP_MANIFEST_CATEGORY_LEN)) {
            return "invalid tasks category";
        }
        if (!copy_string(o, "label", task->label, APP_MANIFEST_LABEL_LEN)) {
            return "invalid tasks label";
        }
        if (!copy_identifier(o, "action", task->action, APP_MANIFEST_ACTION_LEN)) {
            return "invalid tasks action";
        }
        out.num_tasks++;
    }

    if (!get_list(obj, "rest", APP_MANIFEST_MAX_REST, list)) {
        return "invalid rest";
    }
    for (int i = 0; list && (i < list->get_num_elements()); i++) {
        JSON *entry = (*list)[i];
        const char *s = (entry && (entry->type() == eString)) ? ((JSON_String *)entry)->get_string() : "";
        int len = strlen(s);
        if ((len < 1) || (len > APP_MANIFEST_ACTION_LEN) || !is_identifier(s)) {
            return "invalid rest action";
        }
        strcpy(out.rest[i], s);
        out.num_rest++;
    }
    return NULL;
}

const char *app_manifest_parse(const char *text, int size, AppManifest &out)
{
    memset(&out, 0, sizeof(out));
    if ((size < 2) || (size > U2A_MANIFEST_MAX_SIZE) || (text[size - 1] != 0)) {
        return "bad size or no NUL";
    }

    // The JSON parser writes into its input, so parse a copy.
    char *buffer = new char[size];
    memcpy(buffer, text, size);

    // A JSON text of n bytes holds at most (n + 1) / 2 tokens.
    JSON *root = NULL;
    const char *error = "not a JSON object";
    int tokens = convert_text_to_json_objects(buffer, size - 1, size / 2 + 1, &root);
    if ((tokens > 0) && root && (root->type() == eObject)) {
        error = app_manifest_from_json((JSON_Object *)root, out);
    }
    delete root;
    delete[] buffer;
    return error;
}
