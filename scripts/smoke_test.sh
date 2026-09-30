#!/usr/bin/env bash
# Smoke-test HomeNVR (Phase 7): прогон всех API-функций против localhost.
# Запуск на сервере: ./scripts/smoke_test.sh [username] [password]
set -u
BASE="http://localhost"
USER="${1:-admin}"
PASS="${2:-}"
COOKIE=$(mktemp)
PASS_COUNT=0; FAIL_COUNT=0

t() { # t <описание> <ожидаемый код> <curl args...>
  local desc="$1" want="$2"; shift 2
  local code
  code=$(curl -s -m 30 -b "$COOKIE" -c "$COOKIE" -o /tmp/smoke_body -w '%{http_code}' "$@")
  if [ "$code" = "$want" ]; then
    echo "PASS  [$code] $desc"; PASS_COUNT=$((PASS_COUNT+1))
  else
    echo "FAIL  [$code != $want] $desc"; head -c 200 /tmp/smoke_body; echo; FAIL_COUNT=$((FAIL_COUNT+1))
  fi
}

echo "=== базовые ==="
t "GET /health"                 200 "$BASE/health"
t "GET / (frontend)"            200 "$BASE/"
t "GET /login"                  200 "$BASE/login"
t "GET /icon.svg (favicon)"     200 "$BASE/icon.svg"
t "GET /api/system/status без авторизации -> 401" 401 "$BASE/api/system/status"

if [ -n "$PASS" ]; then
  echo "=== auth ==="
  printf '{"username":"%s","password":"%s"}' "$USER" "$PASS" > /tmp/smoke_login.json
  t "POST /api/auth/login"      200 -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d @/tmp/smoke_login.json
  t "GET /api/auth/me"          200 "$BASE/api/auth/me"
  t "POST /api/setup повторно -> 403" 403 -X POST "$BASE/api/setup" -H 'Content-Type: application/json' -d @/tmp/smoke_login.json
  printf '{"username":"%s","password":"definitely-wrong"}' "$USER" > /tmp/smoke_bad.json
  t "POST login с неверным паролем -> 401" 401 -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d @/tmp/smoke_bad.json

  echo "=== система и камера ==="
  t "GET /api/system/status"    200 "$BASE/api/system/status"
  grep -q '"uptime_sec"' /tmp/smoke_body && { echo "PASS  статус содержит uptime_sec"; PASS_COUNT=$((PASS_COUNT+1)); } \
    || { echo "FAIL  статус: нет uptime_sec"; FAIL_COUNT=$((FAIL_COUNT+1)); }
  t "GET /api/camera"           200 "$BASE/api/camera"
  t "GET /api/storage"          200 "$BASE/api/storage"

  echo "=== записи и live ==="
  TODAY=$(date +%F)
  t "GET /api/recordings?date=today" 200 "$BASE/api/recordings?date=$TODAY"
  RECS=$(head -c 4000 /tmp/smoke_body)
  echo "$RECS" | grep -q '"id"' && echo "PASS  записи за сегодня есть" && PASS_COUNT=$((PASS_COUNT+1)) \
    || { echo "FAIL  записей за сегодня нет"; FAIL_COUNT=$((FAIL_COUNT+1)); }
  t "GET live playlist"         200 "$BASE/stream/live/index.m3u8"
  grep -q '#EXTM3U' /tmp/smoke_body && { echo "PASS  плейлист валидный m3u8"; PASS_COUNT=$((PASS_COUNT+1)); } \
    || { echo "FAIL  плейлист невалидный"; FAIL_COUNT=$((FAIL_COUNT+1)); }
  SEG=$(grep -oE '[^ ]+\.ts' /tmp/smoke_body | tail -1)
  [ -n "$SEG" ] && t "GET live ts-сегмент $SEG" 200 "$BASE/stream/live/$SEG"

  echo "=== события и архив ==="
  t "GET /api/events"           200 "$BASE/api/events"
  t "GET /api/events?date=today" 200 "$BASE/api/events?date=$TODAY"
  # архивная HLS-сессия по первому закрытому сегменту
  FIRST=$(echo "$RECS" | sed 's/},{/}\n{/g' | grep ended_at | head -1)
  ST=$(echo "$FIRST" | grep -oE '"started_at":"[^"]+' | cut -d'"' -f4)
  EN=$(echo "$FIRST" | grep -oE '"ended_at":"[^"]+' | cut -d'"' -f4)
  if [ -n "$ST" ] && [ -n "$EN" ]; then
    printf '{"start":"%s","end":"%s"}' "$ST" "$EN" > /tmp/smoke_arc.json
    t "POST /api/archive/session" 200 -X POST "$BASE/api/archive/session" -H 'Content-Type: application/json' -d @/tmp/smoke_arc.json
    ARC_URL=$(grep -oE '"url":"[^"]+' /tmp/smoke_body | cut -d'"' -f4)
    [ -n "$ARC_URL" ] && t "GET архивный плейлист" 200 "$BASE$ARC_URL"
    printf '{"start":"%s","end":"%s"}' "$ST" "$EN" > /tmp/smoke_exp.json
    t "POST /api/export"        200 -X POST "$BASE/api/export" -H 'Content-Type: application/json' -d @/tmp/smoke_exp.json
    EXP_ID=$(grep -oE '"id":[0-9]+' /tmp/smoke_body | cut -d: -f2)
    if [ -n "$EXP_ID" ]; then
      echo "  … жду готовности экспорта #$EXP_ID"
      for i in $(seq 1 30); do
        sleep 4
        ST_CODE=$(curl -s -m 10 -b "$COOKIE" -o /tmp/smoke_exp_st -w '%{http_code}' "$BASE/api/export/$EXP_ID")
        ST_ST=$(grep -oE '"status":"[A-Z]+' /tmp/smoke_exp_st | cut -d'"' -f4)
        [ "$ST_ST" = "READY" ] && break
        [ "$ST_ST" = "FAILED" ] && break
      done
      if [ "$ST_ST" = "READY" ]; then
        echo "PASS  экспорт готов"; PASS_COUNT=$((PASS_COUNT+1))
        t "GET download export" 200 "$BASE/api/export/$EXP_ID/download"
        head -c 12 /tmp/smoke_body | grep -q 'ftyp' && { echo "PASS  файл — валидный MP4"; PASS_COUNT=$((PASS_COUNT+1)); } \
          || { echo "FAIL  файл не MP4"; FAIL_COUNT=$((FAIL_COUNT+1)); }
      else
        echo "FAIL  экспорт не готов (status=$ST_ST)"; FAIL_COUNT=$((FAIL_COUNT+1))
      fi
    fi
  else
    echo "SKIP  нет закрытых сегментов для архива/экспорта"
  fi

  echo "=== admin ==="
  t "GET /api/admin/users"      200 "$BASE/api/admin/users"
  t "GET /api/admin/logs"       200 "$BASE/api/admin/logs?lines=20"
  grep -q '"lines"' /tmp/smoke_body && { echo "PASS  логи возвращают JSON со строками"; PASS_COUNT=$((PASS_COUNT+1)); } \
    || { echo "FAIL  логи: нет поля lines"; FAIL_COUNT=$((FAIL_COUNT+1)); }
fi

rm -f "$COOKIE"
echo
echo "=== ИТОГ: $PASS_COUNT passed, $FAIL_COUNT failed ==="
[ "$FAIL_COUNT" = 0 ]
