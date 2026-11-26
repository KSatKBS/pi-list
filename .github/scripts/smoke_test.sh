#!/bin/bash
# End-to-end check of a published LIST image, doing what a user does:
#  - start it with docker compose, register, log in
#  - upload the Korean caption capture plus video and audio samples
#  - read back the decoded "DTVCC Service 1" caption text
#  - render an audio stream to MP3
#  - open every stream / tab of the GUI in Chrome (no JS error, no blank page)
#  - restart the container: the site and login must come back
#
# Usage: .github/scripts/smoke_test.sh <image>
set -euo pipefail

IMAGE="$1"
BASE=http://localhost
USER_JSON='{"username":"smoke","password":"smoke-test"}'
KOREAN_PCAP=sample_data/pcap/st2110/2110-40/anc_cea708_korean.pcap
VIDEO_PCAP=sample_data/pcap/st2110/2110-20/2110-20_720p50.pcap
AUDIO_PCAP=sample_data/pcap/st2110/2110-30/l24_48000_8ch_0125.pcap
COMPOSE=$(mktemp --suffix=.yml)
EXPECTED=$(printf '안녕하세요. 한글 자막 테스트입니다.\nST 2110-40 CEA-708 디코더\n자막 방송 ♪\n')
FAILED=0

sed "s|image: ghcr.io/ksatkbs/pi-list:korean-captions.*|image: ${IMAGE}|" docs/docker-compose-korean.yml > "$COMPOSE"
export COMPOSE_FILE="$COMPOSE" COMPOSE_PROJECT_NAME=smoke

dump_logs() {
    docker compose ps || true
    # analysis engine output (st2110_extractor / stream_pre_processor), then the end of the server log
    docker compose logs --no-color listserver > /tmp/listserver.log 2>&1 || true
    grep -n -A60 -E "st2110_extractor\] .*Command|Segmentation|Aborted|terminate called|core dumped|exception" /tmp/listserver.log | cut -c1-600 | head -1500 || true
    tail -150 /tmp/listserver.log | cut -c1-600 || true
}
fail() {
    echo "::error::$*"
    dump_logs
    exit 1
}
soft_fail() {
    echo "::error::$*"
    FAILED=1
}
wait_http() { # url expected_code [curl args...]
    local url=$1 want=$2 code=000
    shift 2
    for _ in $(seq 1 60); do
        code=$(curl -s -o /dev/null -w '%{http_code}' "$@" "$url" || true)
        [ "$code" = "$want" ] && return 0
        sleep 5
    done
    echo "$url -> HTTP $code" >&2
    return 1
}

docker compose up -d

echo "== register / login"
wait_http "$BASE/user/register" 201 -X POST -H 'Content-Type: application/json' -d "$USER_JSON" || fail "register failed"
TOKEN=$(curl -sf -X POST "$BASE/auth/login" -H 'Content-Type: application/json' -d "$USER_JSON" | jq -r '.content.token')
[ -n "$TOKEN" ] && [ "$TOKEN" != null ] || fail "login failed"
AUTH="Authorization: Bearer $TOKEN"

upload() {
    local id
    id=$(curl -sf -X PUT "$BASE/api/pcap" -H "$AUTH" -F "pcap=@$1" -F "originalFilename=$(basename "$1")" | jq -r '.uuid')
    [ -n "$id" ] && [ "$id" != null ] || fail "upload of $1 failed"
    echo "$id"
}
wait_analyzed() {
    local analyzed=false
    for _ in $(seq 1 72); do
        analyzed=$(curl -sf "$BASE/api/pcap/$1" -H "$AUTH" | jq -r '.analyzed // false' || echo false)
        [ "$analyzed" = true ] && return 0
        sleep 5
    done
    fail "analysis of $1 did not finish"
}

echo "== wait until the analysis pipeline (RabbitMQ consumers) is up"
for _ in $(seq 1 60); do
    n=$(docker compose logs --no-color listserver 2>/dev/null | grep -c "Waiting for messages in" || true)
    [ "$n" -ge 3 ] && break
    sleep 5
done
sleep 15

echo "== upload"
KOREAN_ID=$(upload "$KOREAN_PCAP")
wait_analyzed "$KOREAN_ID"
VIDEO_ID=$(upload "$VIDEO_PCAP")
wait_analyzed "$VIDEO_ID"
AUDIO_ID=$(upload "$AUDIO_PCAP")
wait_analyzed "$AUDIO_ID"
for id in "$KOREAN_ID" "$VIDEO_ID" "$AUDIO_ID"; do
    curl -sf "$BASE/api/pcap/$id/streams" -H "$AUTH" | jq -c '.[] | {media_type, state, full_media_type}'
done

# Upload the Korean capture once more, now that the system is warm, to tell
# start-up timing issues apart from decoding problems.
KOREAN_ID2=$(upload "$KOREAN_PCAP")
wait_analyzed "$KOREAN_ID2"
sleep 5
echo "first Korean upload: $(curl -sf "$BASE/api/pcap/$KOREAN_ID/streams" -H "$AUTH" | jq -r '[.[].state] | join(",")')"
echo "second Korean upload: $(curl -sf "$BASE/api/pcap/$KOREAN_ID2/streams" -H "$AUTH" | jq -r '[.[].state] | join(",")')"

echo "== Korean captions"
curl -sf "$BASE/api/pcap/$KOREAN_ID2/streams" -H "$AUTH" > /tmp/streams.json
read -r STREAM_ID FILE < <(jq -r '
    .[] | . as $s | (.media_specific.sub_streams // [])[]
    | select(.did_sdid == 24833) | (.sub_sub_streams // [])[]
    | select(.type == "DTVCC Service 1") | "\($s.id) \(.filename)"' /tmp/streams.json | head -1) || true
if [ -z "${FILE:-}" ]; then
    jq . /tmp/streams.json
    soft_fail "no 'DTVCC Service 1' payload"
else
    TEXT=$(curl -sf "$BASE/api/pcap/$KOREAN_ID2/stream/$STREAM_ID/ancillary/$FILE" -H "$AUTH")
    printf '%s\n' "$TEXT"
    # $(...) drops trailing newlines on both sides
    [ "$TEXT" = "$EXPECTED" ] && echo "captions OK" || soft_fail "unexpected caption text"
fi

echo "== audio: render MP3"
AUDIO_STREAM=$(curl -sf "$BASE/api/pcap/$AUDIO_ID/streams" -H "$AUTH" | jq -r '[.[] | select(.media_type == "audio")][0].id')
if [ -z "$AUDIO_STREAM" ] || [ "$AUDIO_STREAM" = null ]; then
    soft_fail "no audio stream detected"
else
    curl -s -o /dev/null "$BASE/api/pcap/$AUDIO_ID/stream/$AUDIO_STREAM/rendermp3?channels=0,1" -H "$AUTH" || true
    MP3=/tmp/audio.mp3
    ok=0
    for _ in $(seq 1 24); do
        sleep 5
        if curl -sf -o "$MP3" "$BASE/api/pcap/$AUDIO_ID/stream/$AUDIO_STREAM/downloadmp3?channels=0,1" -H "$AUTH" &&
            [ "$(head -c 3 "$MP3")" = ID3 -o "$(head -c 2 "$MP3" | od -An -tx1 | tr -d ' ')" = fffb ]; then
            ok=1
            break
        fi
    done
    [ "$ok" = 1 ] && echo "mp3 OK ($(stat -c %s "$MP3") bytes)" || soft_fail "MP3 rendering failed"
fi

echo "== GUI (Chrome)"
GUI_DIR=$(mktemp -d)
cp .github/scripts/gui_check.js "$GUI_DIR/"
(cd "$GUI_DIR" && npm install --silent --no-audit --no-fund playwright-core@1.49.1 >/dev/null)
node "$GUI_DIR/gui_check.js" "$BASE" smoke smoke-test "$KOREAN_ID2" "$VIDEO_ID" "$AUDIO_ID" || soft_fail "GUI check failed"

echo "== restart the server container: the site and login must come back"
docker compose restart listserver
wait_http "$BASE/" 200 || soft_fail "site not reachable after restart"
wait_http "$BASE/auth/login" 200 -X POST -H 'Content-Type: application/json' -d "$USER_JSON" ||
    soft_fail "login failed after restart"

if [ "$FAILED" != 0 ]; then
    dump_logs
    exit 1
fi
echo "== OK"
docker compose down -v
