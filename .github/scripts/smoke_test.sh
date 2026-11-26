#!/bin/bash
# End-to-end check of a published LIST image, doing what a user does:
# start it with docker compose, register, log in, upload the Korean caption
# test capture and read back the decoded "DTVCC Service 1" text.
#
# Usage: .github/scripts/smoke_test.sh <image>
set -euo pipefail

IMAGE="$1"
BASE=http://localhost
PCAP=sample_data/pcap/st2110/2110-40/anc_cea708_korean.pcap
COMPOSE=$(mktemp --suffix=.yml)
EXPECTED=$(printf '안녕하세요. 한글 자막 테스트입니다.\nST 2110-40 CEA-708 디코더\n자막 방송 ♪\n')

sed "s|image: ghcr.io/ksatkbs/pi-list:korean-captions.*|image: ${IMAGE}|" docs/docker-compose-korean.yml > "$COMPOSE"
export COMPOSE_FILE="$COMPOSE" COMPOSE_PROJECT_NAME=smoke

fail() {
    echo "::error::$*"
    docker compose ps || true
    docker compose logs listserver --tail 300 || true
    exit 1
}

docker compose up -d

echo "== register"
code=000
for _ in $(seq 1 60); do
    code=$(curl -s -o /tmp/register.json -w '%{http_code}' -X POST "$BASE/user/register" \
        -H 'Content-Type: application/json' -d '{"username":"smoke","password":"smoke-test"}' || true)
    [ "$code" = 201 ] && break
    sleep 5
done
[ "$code" = 201 ] || fail "register returned HTTP $code"

echo "== login"
TOKEN=$(curl -sf -X POST "$BASE/auth/login" -H 'Content-Type: application/json' \
    -d '{"username":"smoke","password":"smoke-test"}' | jq -r '.content.token')
[ -n "$TOKEN" ] && [ "$TOKEN" != null ] || fail "login failed"
AUTH="Authorization: Bearer $TOKEN"

echo "== upload"
PCAP_ID=$(curl -sf -X PUT "$BASE/api/pcap" -H "$AUTH" \
    -F "pcap=@$PCAP" -F "originalFilename=$(basename "$PCAP")" | jq -r '.uuid')
[ -n "$PCAP_ID" ] && [ "$PCAP_ID" != null ] || fail "upload failed"
echo "pcap id: $PCAP_ID"

echo "== wait for analysis"
analyzed=false
for _ in $(seq 1 60); do
    analyzed=$(curl -sf "$BASE/api/pcap/$PCAP_ID" -H "$AUTH" | jq -r '.analyzed // false' || echo false)
    [ "$analyzed" = true ] && break
    sleep 5
done
[ "$analyzed" = true ] || fail "analysis did not finish"

echo "== find the CEA-708 caption payload"
curl -sf "$BASE/api/pcap/$PCAP_ID/streams" -H "$AUTH" > /tmp/streams.json
read -r STREAM_ID FILE < <(jq -r '
    .[] | . as $s | (.media_specific.sub_streams // [])[]
    | select(.did_sdid == 24833) | (.sub_sub_streams // [])[]
    | select(.type == "DTVCC Service 1") | "\($s.id) \(.filename)"' /tmp/streams.json | head -1) || true
[ -n "${FILE:-}" ] || { jq . /tmp/streams.json; fail "no 'DTVCC Service 1' payload"; }

TEXT=$(curl -sf "$BASE/api/pcap/$PCAP_ID/stream/$STREAM_ID/ancillary/$FILE" -H "$AUTH")
printf '%s\n' "$TEXT"
[ "$TEXT" = "$EXPECTED" ] || fail "unexpected caption text" # $(...) drops trailing newlines on both sides

echo "== restart the server container: the site and login must come back"
docker compose restart listserver
code=000
for _ in $(seq 1 36); do
    code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/" || true)
    [ "$code" = 200 ] && break
    sleep 5
done
[ "$code" = 200 ] || fail "site not reachable after restart (HTTP $code)"
code=000
for _ in $(seq 1 36); do
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/auth/login" -H 'Content-Type: application/json' \
        -d '{"username":"smoke","password":"smoke-test"}' || true)
    [ "$code" = 200 ] && break
    sleep 5
done
[ "$code" = 200 ] || fail "login failed after restart (HTTP $code)"

echo "== OK: Korean captions decoded through the published image"
docker compose down -v
