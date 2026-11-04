# lwip

This folder is empty in this repository. The export is a flat tree: it carries the files of the
application, not the submodules the application pins, and not their history. What belongs here is one
commit of someone else's repository:

| | |
|---|---|
| Repository | https://github.com/GideonZ/lwip |
| Commit | `26a22151f4b7ebb2925523192edc83fa7f31cba9` |

To build the application, put that commit in place. From the root of this repository:

```sh
git clone https://github.com/GideonZ/lwip software/ultimate-app/software/lwip
git -C software/ultimate-app/software/lwip checkout 26a22151f4b7ebb2925523192edc83fa7f31cba9
```
