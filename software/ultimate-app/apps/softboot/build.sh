#!/bin/bash
# Build softboot.u2a in the U64 build container. The app uses the SDK in
# software/appsdk, so the container needs the whole repository: build/build_app.sh
# mounts it and runs this app's Makefile there.
exec "$(cd "$(dirname "$0")" && pwd)/../../build/build_app.sh" softboot
