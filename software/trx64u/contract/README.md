# Base 3.3 contract (MultiCore)

Copies of MultiCore's public headers for base 3.3 and the exports of their shared libraries, taken from TRX64U-MultiCore
branch `t239.3-boundary`, commit `0aff1a3b` (`coreapp/src/`), unchanged. The pending calls of our three libraries map to
them as `docs/dev/boundary-trx64u-handover.md` of that commit says.

They stay here until an SDK release carries them (base 3.3); then this folder goes and the libraries build against the
SDK's `include/`. Never edit them here: a change is MultiCore's, and comes as a new copy.

The new `LibApi` groups (`sync`, `settings`, `ctx`, `io`, `board`) and the members appended to `SvcSys` and `SvcFs` are
not in the SDK 0.1.0 `libapi.h`; our libraries reach them through `../include/base33.h`.
