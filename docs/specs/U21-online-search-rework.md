# U21 — The online search of trx64u: a form, a results tab, and Beyond64 as a third source

**Status:** BUILDING since 2026-11-25 (branch `u21-online`): SDK 0.5.0 is pinned (§7). Where the online search lives is decided (D9: the sub-app's own
form and results tab at F3 "3 Online", no P5 source); the form is built next. The UX review of the 0.4.6 screens (FAIL) is in §3.
**Repo:** TRX64-Ultimate, the sub-app `software/trx64u/apps/online/` (`app.toml`, `online_search.cc`, `online_http.cc`, `online_json.cc`).
**Origin:** Alex, 2026-11-25 (translated): "The 'online' u2a app in the LVGL UI looks terrible. 1. Search should not be a tab of its own: in a
form I say what I am looking for and from which source, and I start the search in the form; the result may then become a tab of its own.
2. Add, beside Assembly64 and CommoServe, https://beyond64.org/#help/json-api."

## 1. Decisions (Alex, 2026-11-25, translated where German)

- **D1, the structure.** The search is a form of the sub-app, not a tab: in it the person picks the source and fills the fields of that
  source, and starts the search from the form (Search at the form's foot). The results are a tab of the sub-app's own. The local search of
  F3 "2 Search" stays as it is. A sub-app brings its own tabs and fills their rows from its states; it does not draw LVGL itself (api.md 4.17,
  6.11), so whatever the base's menu engine cannot express is a request to MultiCore (D3).
- **D2, Beyond64.** Beyond64 (https://beyond64.org, public read-only JSON API, §4) is a third source beside Assembly64 and CommoServe.
- **D3, the SDK moves forward generically.** "Ask for it, but so that it moves the SDK forward generically." Sent to MultiCore as their
  T262; Alex decided there: all nine parts in SDK 0.5.0 (T262 D1), names `list` (node kind), `pick` (node key), `show` (tab key) (T262 D2):
  1. `list`: rows of a node that come from the library at run time (count, title, value, tone, side-panel lines, RETURN with the row
     index, paging by a callback), instead of predeclared rows ("Choice 1..12", "Result 1..50").
  2. `pick`: a choice list for libraries (today's `ops_ask_list` is not offered to libraries and holds at most 40 rows).
  3. `show`: a tab that appears and disappears at run time (Results only after a search).
  4. A busy caption for `job` nodes (today "Building the image 0%", menuctl.cc 2117, 3284), a percentage only when the operation reports one.
  5. Search providers in the base's search tab (one Search form with a Source row, "This device" included).
  6. Key rows in the foot legend.
  7-9. Base defects: a run after a fetch leaves the menu open (menuctl.cc 2559); the focus is lost when the focused row hides; every
     RETURN on an operation row blanks the content for about 250 ms (to be confirmed on the board).
- **D4, Beyond64 files the C64 Ultimate cannot load.** "Grey or hide ... best something to filter in the form": a filter in the form,
  "Show: loadable only / all". With "all" a release with no loadable file (only links, or no `.prg`, `.d64`, `.d81`; §4) is listed greyed,
  "Only on a computer".
- **D5, only Beyond64's API as it is.** "We have to make it work with what is there. If we have proposals to enhance the API for a later
  version, fine." The list (`/api/c64/releases`) carries no files, so the filter of D4 asks `/api/c64/release/{id}` for every result before
  the list is shown. Proposals for the API are collected in §5, for later.
- **D6, the order.** "We wait for 0.5.0." The rework is built on SDK 0.5.0 when MultiCore publishes it; until then the sub-app stays as it is
  (0.4.6). Read as: nothing of U21 is built before 0.5.0, also not the parts that do not need it (Beyond64 client, fields per source, the
  app's own defects of §3).
- **D7, Assembly64 and CommoServe stay at 20 results** (§6 point 2). No request to their operators. Alex: "Assembly und CommoServe ist die selbe
  Server infrastructur und das ist halt 10-15 Jahre alter Java mist, das wird nie mehr anders bis die einen Rewrite machen".
- **D8, the filter of D4 defaults to "loadable only"** (§6 point 1). Alex: "nur ladbare ja". Before it is built, the time the per-result details
  of D5 take for a full page of Beyond64 results is measured in ue2emu (real network through slirp); noticeably slow (over about 3 s a page) is
  reported to Alex with the numbers, not worked around (§6).
- **D9, the place: the sub-app's own form and results tab, no P5 source** (§6 point 3). Alex: "Eigenes Formular und eigener Ergebnis-Tab nach
  D1, an der heutigen Stelle F3 „3 Online“. Nur so gehen Auswahllisten und der Filter, und die lokale Suche bleibt unberührt." (Translated: own
  form and own results tab per D1, where it is today, F3 "3 Online"; only so do the choice lists and the filter work, and the local search stays
  untouched.) So the base's search providers (D3.5, P5, api.md 6.20) are not used: the sub-app registers no search source, and F3 "2 Search"
  stays the base's local search.

## 2. The target (from the UX review's proposal, to be fixed in detail on 0.5.0)

- F3 strip (D9): 1 Browse · 2 Search (the base's local search, untouched) · 3 Online (the sub-app's form) · 4 Results (the sub-app's results;
  with `show` only while there are results, D3.3).
- The form (a page of the sub-app, opened from the results tab's "New search" and from the empty tab): first row Source (Assembly64,
  CommoServe, Beyond64; "This device" when the base offers providers, D3.5); the rows below change with the source; choices open a `pick`
  list; Search at the foot; the side panel says the source and the last search.
- Fields per source (measured on the servers 2026-11-25 by the UX review):
  - Assembly64: text Name, Group, Handle, Event; choices Category (12), Subcategory (27), Archive (repo, 9), Rating, File type (10), Year
    (47), Added within (12); Sort by (6) with ascending/descending.
  - CommoServe: text Name, Group, Handle, Event (to be checked: whether CommoServe takes all four); choices Category (5), Year (47), File
    type (7); Sort by (Name, Year) with order.
  - Beyond64: text (up to 120 characters) and "Look in" (title, creator, credits, description, technical); Category (6) with any/all,
    Platform (C64, C128), Environment (Native, GEOS, CP/M), Hardware (10) with all/any, AI (yes, no, unspecified); Show (D4). Order: newest
    first (`s=added` is the only sort).
  - Changing the source drops the choices the new source does not have.
- Running: "Searching Beyond64 ... <- cancels", no percentage (D3.4). Success opens the results tab. Empty: stay in the form, "Nothing found in
  <source> for '<text>'", Search keeps the focus. No network: Search greyed, the hint names F8 System > Network.
- Results tab (title "<source> · <text> · <count>"): a `list` (D3.1), one row per result: title, right the year (Assembly64, CommoServe) or the
  category (Beyond64); the side panel shows the focused result (group/creator, handle, date, category, rating, platform; Beyond64's
  description). RETURN opens its files (name, size); RETURN on a file opens the Library's action sheet; Run downloads and closes the menu
  (D3.7). Paging where the source has it (Beyond64 `more`).

## 3. Today's screens (UX review, 2026-11-25, verdict FAIL)

Pictures from the real app in ue2emu (trx64u, SDK 0.4.6, real searches through slirp), tool `tests/ui/emu_session.py`. The worst, ranked:
1. The form mixes everything in one list: Search first, the source switch named "Library" (as the F3 area), choices loaded by hand, 16 rows
   with Search scrolled away, results in another tab (Browse > /ONLINE).
2. Every request shows "Building the image 0%" (base, D3.4).
3. After a successful search the old error stands in red over the new list (`err` never cleared: online_search.cc).
4. Changing the source keeps Assembly64's choices (`op_library` only flips a flag).
5. Run on an online file leaves the menu open (base, D3.7); the text asks to retry although the base retries.
6. After the choices load no row has the focus (base, D3.8).
7. Rating, Type and Date show a blank value (the app shows `name`, these choices have only `aqlKey`).
8. RETURN blanks the content for about 250 ms (base, D3.9).
9. No network: a misleading error, Search stays enabled. 10. "Nothing found" in red as an error. 11. Result list: focus on "..", no source,
   query or count, no paging (20 is the servers' cap). 12. Names cut by a 56-byte buffer without ellipsis; duplicates get "#5". 13. Server
   keys as labels ("Repo", "Subcat", "Latest") and in the status line. 14. Empty side panel. 15-17. base foot words, the `ask` prompt hides
   the form, `ask_state` prefills 24 of 47 characters.

## 4. Beyond64's API (read 2026-11-25 on https://beyond64.org/#help/json-api)

Public, read-only, no account, key, session or cookie; compact UTF-8 JSON; built for simple HTTP/1.0 clients, "including Commodore 64
Ultimate software"; plain HTTP to beyond64.org answers 200 (measured by the UX review).
- `GET /api/c64/releases`: `q` (text, up to 120 characters, case-insensitive substring; `f` scopes title, creator, credits, description,
  technical, `f=*` all five), `c` (Demo, Application, Game, Tool, Firmware, Source/Docs; `cm` or/and), `pl` (C64, C128), `e` (Native, GEOS,
  CP/M), `ai` (yes, no, unknown), `h` (REU, GeoRAM, EasyFlash emulation, Cartridge emulation / CRT, Ultimate Audio, Dual / Multi-SID,
  Networking, CPU Turbo, UltimateDOS / Command Interface, Ultimate II+; `hm` and/or), `limit` 1-50 (default 10), `page`, `s=added`.
  Answer `{"p":1,"more":true,"r":[{"id":117,"t":"Title","a":"Creator","c":"Demo"}]}`; no total count.
- `GET /api/c64/release/{id}`: `cs`, `pl`, `d` (date), `x` (description, at most 1,200 characters), `f` [{`n` label, `u` URL}], `sb`, `mb`.
- `GET /api/c64/download/{id}/{fileIndex}`: the file through Beyond64 over plain HTTP, only `.prg`, `.d64`, `.d81` (by `f[].n`), exact
  Content-Length, no chunking; JSON errors 400, 404, 415 (type), 502, 503, 504, 500.
- `GET /api/c64/filters`: the valid values (clients should ask, not assume).
- Errors: `{"error":"..."}`; 422 for an invalid value or a too long search; unknown keys ignored; `Cache-Control: public, max-age=300`.

## 5. Proposals for Beyond64's API (for later, D5)

- A list filter "has a file the download bridge serves" (e.g. `dl=1`), or the count of such files per list item, so the filter of D4 needs
  one request instead of one per result.
- The year (`d`) in the list items, for the results' right column.

## 6. Open (one at a time)

- ~~The default of the filter of D4~~: loadable only (D8). ~~Assembly64 and CommoServe stop at 20 results~~: they stay (D7).
- The time the per-result details (D5) take for a page of Beyond64 results, measured in ue2emu before D4/D8 is built (D8).
- ~~Where the online search lives~~: the sub-app's own form and results tab at F3 "3 Online", no P5 source (D9).

## 7. Status and history

- **2026-11-25, SDK 0.5.0 pinned.** `sdk.pin` names the GitHub release `sdk-0.5.0-199705ec` (MultiCore t239.3 `199705ec`, base 3.5; no Gitea
  copy, `fetch_sdk.sh --download` refuses with a sentence). `build.sh` builds all five (trx64u 77,096, ring 136,232, monitor 209,004 bytes of
  image, trex_crt, online), `pack_cor.sh --emu` packs `trx64u.cor` and runs it in ue2emu 0.7.4: READY., the monitor's ping, and the app registers
  exactly `base/ops_trx64u.txt` (384 lines both ways); `rtl_check.py lint-src` clean. Two changes the SDK made necessary: the base has no group
  "look" any more (api.md 4.13.19.6, MultiCore T239.3 D56), so its four operations `base.overlay`, `base.scheme`, `base.squeeze` and `base.nav`
  left the golden and `base/menu.toml` dropped its `hide` of the four settings. Known: `SVC_HAS` of `base/`, `ring/` and `monitor/` (three
  arguments) redefines the SDK's new two-argument `SVC_HAS` of `libapi.h` (a compiler warning, no change of the code).
- **2026-11-25, placement and P5, looked at in ue2emu for Alex's answer (§6).** The sub-app is the F3 tab "3 Online" (a library's `[[tab]]` is
  appended to an area the core's route has, `contrib.cc` `merge_tabs`; its rows come after the core's own, `sort_nodes`); the rows at the foot of
  F7 under HOTKEYS are TREX CRT's (`F7/settings` without a group). A throwaway P5 source (not in git) showed the base's form: Source (This device,
  then the sources), "Name contains", up to four text fields, Search; a job "Searching <name>"; the results as the list page of 6.19 with the
  SELECTED panel. Registering a source changes the local "2 Search": RETURN opens that form instead of the name prompt.
