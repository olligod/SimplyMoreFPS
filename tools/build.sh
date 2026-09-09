#!/bin/sh
# Runs tools/build.py with the same arguments.
exec python3 "$(dirname "$0")/build.py" "$@"
