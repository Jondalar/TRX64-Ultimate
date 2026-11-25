Online search on ABI 3 (MultiCore T239.3 D12, P3-1, L-3). PROVENANCE: a copy of `apps/online` of TRX64U-MultiCore (branch `t239.3`, commit `6e280710`, the commit
SDK 0.4.6 is packed from and the `online.u2a` that `sdk.pin` pinned until U20 D22 was built from); Alex Damhuis' code in both repositories. MultiCore's `PROVENANCE.md`
(rows "Assembly64 / CommoServe client" and "Online search"): ours, GPL-3.0-or-later; the host name, the four URLs, the headers of a request (`Client-Id`, `User-Agent`,
`Connection: close`) and the URL encoding follow Gideon's `software/network/assembly.cc` and `components/pattern.cc` (`url_encode`), the query is built the way his
`software/userinterface/assembly_search.cc` builds it; none of his client (BSD sockets, `HttpRequest`, the server library's body parser, `json.h`) is copied. Against MultiCore's
copy only the Makefile (its first comment line and its include, the SDK's `mk/app.mk`) and this README are changed here; the sources and `app.toml` are theirs byte for byte.

A resident sub-app of format 3 (`APP_KIND = lib`, needs `base>=3.3`): the Assembly64 / CommoServe search. `online_lib.cc` is `lib_register`: it registers the operations
`online.*` and the state values of the form (`online_search.cc`) and exports the table "online" (`online_api.h` of the SDK's `include/`, major 1), which the base's file manager
(`netfs.cc`) reads the folder `/Online` through; the base takes the table only when it is at least as large as its own `OnlineApi` (`netfs.cc` `online_api`), so the sub-app is
built against the pinned SDK's header. `app.toml` is its menu record: the tab "Online" of F3 with its twenty rows. The client is `online_http.cc` (one GET over
`api->net->connect`, `send`, `recv`, `wait`) and `online_json.cc` (a reader of the answers); the state of the search is in `online_search.cc`, in the image. Every request to the
server runs on a job (`api->job->start`), never on the menu task or the executor task (api.md 1.3.9): `online.presets` and `online.search` are nodes with the attribute `job` that
answer `more` while the job waits; the folder `/Online` answers from what is kept and starts a prefetch for the rest (api.md 9.5.11).

Built by `build.sh online` (`build/online/online.u2a`), carried in record 7 of `trx64u.cor` (`pack_cor.sh --lib`), and an asset of the `fw-<branch>-<sha8>` pre-release (U20 D22,
MultiCore T261 D7). MultiCore's host test of it (`tests/loader/test_online.inc`) is not copied; here the emulator check (`tools/emu_ops.py --check base/ops_trx64u.txt`) shows that
the image loads and registers its operations and states (the `online.*` lines of the golden).
