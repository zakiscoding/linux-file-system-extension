#!/usr/bin/env bash
source "$(dirname -- "$0")/env.sh"
require make python3
cd "$ROOT"
make test
bash scripts/build-kernel.sh
bash scripts/build-guest.sh
bash scripts/run-qemu.sh
