# ecpprog

This folder is empty in this repository. The export is a flat tree: it carries the files of the
application, not the submodules the application pins, and not their history. What belongs here is one
commit of someone else's repository:

| | |
|---|---|
| Repository | https://github.com/GideonZ/ecpprog |
| Commit | `16bdfa2274dc8d48feb2f4f96c41658d76a63dcf` |

To build the application, put that commit in place. From the root of this repository:

```sh
git clone https://github.com/GideonZ/ecpprog software/ultimate-app/tools/ecpprog
git -C software/ultimate-app/tools/ecpprog checkout 16bdfa2274dc8d48feb2f4f96c41658d76a63dcf
```
