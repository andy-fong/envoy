#!/bin/bash

# No core dumps.
ulimit -c 0

# Memory limits to avoid crashing the whole system if clangd goes wild.
exec prlimit --as=14000000000:16000000000 --rss=14000000000:16000000000 clangd "$@"
