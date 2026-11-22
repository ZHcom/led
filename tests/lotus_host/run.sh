#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
# Host-only mocks: do not build these files into ESP32 firmware.
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -I main -o "$T/protocol" main/lotus_protocol.c tests/lotus_host/test_protocol.c
"$T/protocol"
cc -std=gnu11 -Wall -Wextra -Werror -I tests/lotus_host/stubs -I main main/lotus_protocol.c main/lotus_ha.c tests/lotus_host/test_ha_host.c -l:libcjson.so.1 -o "$T/ha"
"$T/ha"
cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-parameter -I tests/lotus_host/stubs -I main main/lotus_protocol.c main/lotus_ha.c main/lotus_bridge.c tests/lotus_host/test_bridge_host.c -l:libcjson.so.1 -o "$T/bridge"
"$T/bridge"
