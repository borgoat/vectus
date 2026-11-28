#!/bin/sh
# Run vectus's ESP-IDF tests on ESP-IDF's linux target against tusd and Toxiproxy, then build them for the ESP32:
#
#   tests/esp-idf/run.sh
#
# Starts tusd and Toxiproxy with tests/e2e-env.sh unless they are running, and leaves them running.
# On a board instead: set Wi-Fi and the three URLs with `idf.py menuconfig`, then `idf.py flash monitor`. The URLs
# must be reachable from the board, and tests/e2e-env.sh binds tusd and Toxiproxy to 127.0.0.1 only.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
image=${VECTUS_IDF_IMAGE:-espressif/idf:v5.5.6}

docker inspect vectus-tusd >/dev/null 2>&1 || "$root/tests/e2e-env.sh" >/dev/null

# Mounted as /vectus, the name ESP-IDF gives the component
docker run --rm --network vectus-e2e -v "$root":/vectus -w /vectus/tests/esp-idf "$image" bash -c '
    set -e
    [ -f build-linux/sdkconfig ] || idf.py --preview -B build-linux -D SDKCONFIG=build-linux/sdkconfig set-target linux >/dev/null
    idf.py -B build-linux -D SDKCONFIG=build-linux/sdkconfig build > build-linux/idf.log 2>&1 || { tail -60 build-linux/idf.log; exit 1; }
    build-linux/vectus-esp-test.elf
    [ -f build-esp32/sdkconfig ] || idf.py -B build-esp32 -D SDKCONFIG=build-esp32/sdkconfig set-target esp32 >/dev/null
    idf.py -B build-esp32 -D SDKCONFIG=build-esp32/sdkconfig build > build-esp32/idf.log 2>&1 || { tail -60 build-esp32/idf.log; exit 1; }
    grep "binary size" build-esp32/idf.log
'
