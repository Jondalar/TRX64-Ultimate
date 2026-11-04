# httpd

This folder is empty in this repository. The export is a flat tree: it carries the files of the
application, not the submodules the application pins, and not their history. What belongs here is one
commit of someone else's repository:

| | |
|---|---|
| Repository | https://github.com/GideonZ/MicroHttpServer |
| Commit | `ad73d3217bb0e1b3f37e0dbba595309b942aea17` |

To build the application, put that commit in place. From the root of this repository:

```sh
git clone https://github.com/GideonZ/MicroHttpServer software/ultimate-app/software/httpd
git -C software/ultimate-app/software/httpd checkout ad73d3217bb0e1b3f37e0dbba595309b942aea17
```
