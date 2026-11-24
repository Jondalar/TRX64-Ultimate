# ROMs

No ROM is in this repository (U20 D14). The simulations read the stock C64 and 1541-II ROMs here:
`basic.901226-01.bin`, `kernal.901227-03.bin`, `characters.901225-01.bin` (through `tools/rom2mem.py`)
and `1541-ii.bin` (the drive and pause benches, by hand). No bitstream contains them.

`ci/roms.txt` pins each one: its size, its SHA-256 and a download URL at a fixed commit of VICE's data
(https://github.com/VICE-Team/svn-mirror, `vice/data/C64` and `vice/data/DRIVES`). The same bytes are in
Gideon Zweijtzer's 1541ultimate (`roms/`, https://github.com/GideonZ/1541ultimate/tree/master/roms).
`ci/roms.py` puts them here and checks size and hash; a file with another hash is refused:

```sh
python3 ci/roms.py fetch                     # all four
python3 ci/roms.py fetch roms/1541-ii.bin    # one
```

`tools/rom2mem.py` fetches the three C64 ROMs itself when they are missing, and the stage `sims` fetches what
each bench reads before it runs it (`ci/sims.sh`); a ROM whose source cannot be reached there makes the
bench SKIPPED, not red. The store `ROMS_STORE` (else `/cache/roms` on the CI runners) keeps one copy per
SHA-256, so a fetch does not need the network twice. The files here are ignored by git (`.gitignore`).
