# Fonts

`globals.xml` compiles the ASCII range of `LiberationSans-Regular.ttf` into five sizes with `<bin>` and
`as_file="false"`. The font is not in this repository: until U20 A5 (2026-11-24) this folder held a symlink
to the copy vendored with the firmware submodule (`software/lvgl/src/libs/freetype/LiberationSans-Regular.ttf`
of the fork `TREX/1541ultimate`, which the Gitea branch `gideon-app` still pins; `LiberationSans-LICENSE.txt`
lies beside it). The submodule left this tree, so put a copy or a link of that file here before opening the
project in the LVGL Editor; do not commit it.

LVGL Pro font syntax: https://lvgl.io/docs/pro/syntax/fonts
