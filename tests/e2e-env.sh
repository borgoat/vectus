#!/bin/sh
# Start tusd, and Toxiproxy in front of it, for the live and resume tests, then print the variables they read:
#
#   export $(tests/e2e-env.sh)
#   ctest --test-dir build --output-on-failure
#
# Stop them with: docker rm -f vectus-tusd vectus-toxiproxy && docker network rm vectus-e2e
set -eu

docker network inspect vectus-e2e >/dev/null 2>&1 || docker network create vectus-e2e >/dev/null
docker run -d --rm --name vectus-tusd --network vectus-e2e -p 127.0.0.1:8080:8080 \
    tusproject/tusd:v2.10.3 -max-size=67108864 >/dev/null
docker run -d --rm --name vectus-toxiproxy --network vectus-e2e -p 127.0.0.1:8474:8474 -p 127.0.0.1:8666:8666 \
    ghcr.io/shopify/toxiproxy:2.13.0 >/dev/null

waited=0
until curl -fsS -o /dev/null http://127.0.0.1:8474/version 2>/dev/null \
    && curl -fsS -X OPTIONS -o /dev/null http://127.0.0.1:8080/files/ 2>/dev/null; do
    waited=$((waited + 1))
    if [ "$waited" -ge 60 ]; then
        echo "tusd or Toxiproxy did not start within 60s" >&2
        exit 1
    fi
    sleep 1
done

# The resume tests add and remove toxics on this proxy
curl -fsS -X POST http://127.0.0.1:8474/proxies \
    -d '{"name":"tusd","listen":"0.0.0.0:8666","upstream":"vectus-tusd:8080"}' >/dev/null

echo "VECTUS_TEST_URL=http://127.0.0.1:8080/files/"
echo "VECTUS_TEST_TOXIPROXY=http://127.0.0.1:8474"
echo "VECTUS_TEST_PROXIED_URL=http://127.0.0.1:8666/files/"
