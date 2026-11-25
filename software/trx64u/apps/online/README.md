The online search of trx64u: a resident sub-app of format 3 (`APP_KIND = lib`, needs `base>=3.5`, version 2.0) that searches Assembly64, CommoServe and
Beyond64 and lists what it found in the folder `/Online` of the Library. Spec: `docs/specs/U21-online-search-rework.md` (form, results tab, Beyond64; D1-D9).

PROVENANCE. It began as a copy of `apps/online` of TRX64U-MultiCore (branch `t239.3`, commit `6e280710`, the commit SDK 0.4.6 is packed from; ours since U20
D22); Alex Damhuis' code in both repositories. MultiCore's `PROVENANCE.md` (rows "Assembly64 / CommoServe client" and "Online search"): ours,
GPL-3.0-or-later; the host name, the URLs, the headers of a request (`Client-Id`, `User-Agent`, `Connection: close`) and the URL encoding follow Gideon's
`software/network/assembly.cc` and `components/pattern.cc` (`url_encode`), the Assembly64 query is built the way his `software/userinterface/assembly_search.cc`
builds it; none of his client (BSD sockets, `HttpRequest`, the server library's body parser, `json.h`) is copied. U21 rewrote `online_search.cc` and `app.toml`
on SDK 0.5.0 and added the Beyond64 client (its public JSON API, https://beyond64.org/#help/json-api); MultiCore's copy is no longer the reference.

The parts. `online_lib.cc` is `lib_register`: it registers the operations `online.*` and the state values of the form and the results (`online_search.cc`) and
exports the table "online" (`online_api.h` of the SDK's `include/`, major 1), which the base's file manager (`netfs.cc`) reads the folder `/Online` through; the
base takes the table only when it is at least as large as its own `OnlineApi`, so the sub-app is built against the pinned SDK's header. `online_http.cc` is the
client (one HTTP/1.1 GET over `api->net`: `connect`, `send`, `recv`, `wait`, `close`; the body by Content-Length, chunked, or the close), `online_json.cc` reads
the answers, `otext.h` holds the text helpers (no libc in an image) and folds the servers' UTF-8 to what the menu's fonts draw. Every request runs on a job
(`api->job->start`), never on the menu task or the executor task (api.md 1.3.9), one at a time.

The menu (`app.toml`, the menu record; D9). F3 gets two tabs after the base's Browse and Search: "3 Online", the form, and "4 Results", shown only while there
are results (`show = "@online.shown"`). The form: Source first (a `pick` list: Assembly64, CommoServe, Beyond64), then the rows of that source (Assembly64:
Name, Group, Handle, Event, Category, Subcategory, Archive, Year, Added within, Rating, File type, Sort by; CommoServe: the four texts and Category, Year,
File type, Sort by; Order once Sort by is chosen; Beyond64: Text, Look in, Category, Platform, Environment, Hardware, AI, Show; Category match and Hardware
match once two are chosen), Clear the form, and Search at the foot (`F3/online/mainfoot`, a `job` with the `busy` caption "Searching <source>", greyed
without a network, the hint names F8 System > Network). The choices of Assembly64 and CommoServe (`/leet/search/aql/presets`) and of Beyond64
(`/api/c64/filters`) load by themselves when the form is shown with a network. Keys (`base/menu.toml`, a library's menu record holds no key table): S the next
source, X clear (form) / N new search, X clear (results). The results tab: its title is "<source> • <text> • <count>", the node Results is a `list` (one row per
result: title, the year or Beyond64's category, the side lines; Beyond64's last row asks for the next page), RETURN on a result opens its folder
`/Online/<result>/` in Browse (`lib.show`, `goto_screen`), and RETURN on a file there is the Library's action sheet (Run downloads, the menu closes).
Beyond64's Show: Loadable only (default, D8) lists the releases with a `.prg`, `.d64` or `.d81` (the files the download bridge serves; one details request per
release, D5); All releases lists the others too, "Only on a computer".

Built by `build.sh online` (`build/online/online.u2a`), carried in record 7 of `trx64u.cor` (`pack_cor.sh --lib`), and an asset of the `fw-<branch>-<sha8>`
pre-release (U20 D22, MultiCore T261 D7). MultiCore's host test of the 0.4.6 copy (`tests/loader/test_online.inc`) is not copied; here the emulator check
(`tools/emu_ops.py --check base/ops_trx64u.txt`) shows that the image loads and registers exactly its operations and states (the `online.*` lines of the golden),
and the searches were run in ue2emu against the real servers (U21 §7).
