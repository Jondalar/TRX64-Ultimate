# Online search on ABI 3 (MultiCore T239.3 L-3): a resident sub-app, format 3, built by software/trx64u/build.sh against the pinned SDK; see the SDK's mk/app.mk
# (make SDK=<the SDK's folder>).
# `base>=3.6`: api->net connects (SvcNet::connect, resolve, 3.3), the loader serves the named export (3.2), the menu record has a node of kind list, `pick = "@state"`, a tab's `show` and a job's `busy` (3.5, T262), its `[[key]]` rows and `ask_max`/`ask_any`, and ctx.browse_sheet (3.6, T262 D4; browse_sheet from a list row works from SDK 0.5.2). APP_EXPORTS lists the table the file manager finds.
APP_NAME = online
APP_KIND = lib
APP_VERSION = 2.0
APP_NEEDS = base>=3.6
APP_EXPORTS = online=1
APP_MENU = app.toml
# S13 budget. heap_keep 0: the form, the results and the entries are static arrays of the image (bss); at rest the sub-app holds through api->mem only the last download, which the
# next one or the unload gives back. heap_peak 16 MiB: the most at once is one download, which online_http.cc reads into one block grown by half again up to ONLINE_DOWNLOAD_MAX
# (6 MiB) of body, so a growing realloc has the old block (up to 6 MiB) and the new (up to 9 MiB) at once = 15 MiB; the JSON answers (ONLINE_JSON_MAX = 96 KiB text and its nodes) are far below. tab_fetch gives the old file back before it starts the next download (the job never touches dl_b), so the peak is one download. The Work (one request at a time: the arrays of presets, results and entries a job fills) is bss of the image, not heap_keep.
APP_BUDGET = heap_keep=0 heap_peak=16777216
APP_CCSRCS = online_lib.cc online_search.cc online_http.cc online_json.cc
include $(SDK)/mk/app.mk
