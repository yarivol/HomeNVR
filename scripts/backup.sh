#!/usr/bin/env bash
# HomeNVR backup (ТЗ §68): PostgreSQL (пользователи, настройки камеры, события,
# метаданные записей) + конфигурация + ключ шифрования.
# Запуск на сервере:  ./scripts/backup.sh
# Записи (data/recordings) НЕ входят в бэкап — их объём отдельная задача (NAS в будущем).
set -euo pipefail
cd "$(dirname "$0")/.."

# подхватываем креды postgres из .env
[ -f .env ] && { set -a; . ./.env; set +a; }
PGUSER="${POSTGRES_USER:-homenvr}"
PGDB="${POSTGRES_DB:-homenvr}"

STAMP=$(date +%Y%m%d-%H%M%S)
OUT_DIR="data/backups"
OUT="$OUT_DIR/homenvr-backup-$STAMP.tar.gz"
mkdir -p "$OUT_DIR"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "==> PostgreSQL dump"
docker exec homenvr-postgres pg_dump -U "$PGUSER" -d "$PGDB" -F c -f /tmp/homenvr.dump
docker cp homenvr-postgres:/tmp/homenvr.dump "$TMP/homenvr.dump"
docker exec homenvr-postgres rm -f /tmp/homenvr.dump

echo "==> Конфигурация и ключи"
cp -r data/config "$TMP/config"
[ -f .env ] && cp .env "$TMP/env"
cp nginx/nginx.conf "$TMP/nginx.conf"

tar -czf "$OUT" -C "$TMP" .
echo "OK: $OUT ($(du -h "$OUT" | cut -f1))"

# храним последние 7 бэкапов
ls -t "$OUT_DIR"/homenvr-backup-*.tar.gz 2>/dev/null | tail -n +8 | xargs -r rm --
