# DEVELOPMENT.md — Запуск и разработка

## Требования

- Debian 13 (или любой Linux/Windows с Docker)
- Docker + Docker Compose plugin

## Первый запуск

```bash
# 1. Переменные окружения
cp .env.example .env
# отредактируйте .env — задайте надёжный POSTGRES_PASSWORD

# 2. Секрет для шифрования паролей камеры (AES-GCM, ТЗ §39)
mkdir -p data/config
openssl rand -hex 32 > data/config/secret_key
chmod 600 data/config/secret_key

# 3. Запуск всей системы
docker compose up -d --build

# 4. Проверка
curl http://localhost/health        # через nginx -> backend
curl http://localhost/api/system/status
```

Откройте http://<ip-сервера> — при первом запуске показывается
мастер настройки (setup wizard, ТЗ §73.5).

## Структура

| Путь | Что это |
|---|---|
| `backend/` | C++20 backend (Crow, CMake) |
| `backend/db/migrations/` | SQL-миграции, применяются backend'ом при старте |
| `frontend/` | Next.js + TypeScript + Tailwind |
| `nginx/nginx.conf` | Единственная внешняя точка входа (порт 80) |
| `data/` | Persistent volumes: postgres, recordings, thumbnails, exports, config |

## Полезные команды

```bash
docker compose logs -f backend      # логи backend
docker compose ps                   # статус контейнеров
docker compose down                 # остановить
docker compose up -d --build backend  # пересобрать только backend
docker compose restart nginx        # применить изменения nginx.conf
```

## Troubleshooting (опыт первого развёртывания)

| Проблема | Причина | Решение |
|---|---|---|
| Пустая страница в браузере | CSP `script-src 'self'` блокирует inline-скрипты Next.js | `'unsafe-inline'` в script-src (уже исправлено) |
| `/health` отдаёт 404 от frontend | nginx не имел маршрута /health | добавлен `location = /health` |
| Internal compiler error при сборке | Мало RAM в VM при `-j$(nproc)` | сборка с `-j2` |
| Изменения nginx.conf не действуют | Конфиг смонтирован в контейнер, но nginx его не перечитал | `docker compose restart nginx` |
| В Debian 13 нет пакета `docker-compose-v2` | В репозитории Debian он называется `docker-compose` (это v2) | `apt install docker-compose` |

## После перезагрузки сервера

Все контейнеры имеют `restart: unless-stopped` (ТЗ §55) —
система поднимается автоматически.

## Безопасность

- `data/` и `.env` в `.gitignore` — секреты не попадают в Git
- PostgreSQL не опубликован наружу (нет `ports:` у сервиса)
- Наружу смотрит только nginx (порт 80)
