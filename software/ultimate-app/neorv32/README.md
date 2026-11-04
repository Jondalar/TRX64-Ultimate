# neorv32

This folder is empty in this repository. The export is a flat tree: it carries the files of the
application, not the submodules the application pins, and not their history. What belongs here is one
commit of someone else's repository:

| | |
|---|---|
| Repository | https://github.com/GideonZ/neorv32 |
| Commit | `70c5eabf29f20bdb2001490330869c4f0484fa19` |

To build the application, put that commit in place. From the root of this repository:

```sh
git clone https://github.com/GideonZ/neorv32 software/ultimate-app/neorv32
git -C software/ultimate-app/neorv32 checkout 70c5eabf29f20bdb2001490330869c4f0484fa19
```
