#!/usr/bin/env bash
# HomeNVR restore (ТЗ §68): ./scripts/restore.sh data/backups/homenvr-backup-XXX.tar.gz
set -euo pipefail
cd "$(dirname "$0")/.."

[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: $0 <backup.tar.gz>"; exit 1; }
BACKUP="$1"

[ -f .env ] && { set -a; . ./.env; set +a; }
PGUSER="${POSTGRES_USER:-homenvr}"
PGDB="${POSTGRES_DB:-homenvr}"

echo "ВНИМАНИЕ: текущие данные БД и конфигурация будут ПЕРЕЗАПИСАНЫ."
read -r -p "Продолжить? [yes/N] " ans
[ "$ans" = "yes" ] || { echo "отменено"; exit 0; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
tar -xzf "$BACKUP" -C "$TMP"

echo "==> Останавливаю backend/frontend"
docker compose stop backend frontend nginx

echo "==> Восстанавливаю конфигурацию"
cp -r "$TMP/config/." data/config/
[ -f "$TMP/env" ] && cp "$TMP/env" .env
cp "$TMP/nginx.conf" nginx/nginx.conf

echo "==> PostgreSQL restore"
docker compose up -d postgres
# ждём готовности
for i in $(seq 1 30); do
  docker exec homenvr-postgres pg_isready -U "$PGUSER" -d "$PGDB" -q && break
  sleep 2
done
docker cp "$TMP/homenvr.dump" homenvr-postgres:/tmp/homenvr.dump
docker exec homenvr-postgres pg_restore -U "$PGUSER" -d "$PGDB" --clean --if-exists /tmp/homenvr.dump
docker exec homenvr-postgres rm -f /tmp/homenvr.dump

echo "==> Запускаю систему"
docker compose up -d
echo "OK: восстановление завершено"
