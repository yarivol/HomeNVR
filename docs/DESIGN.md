# DESIGN.md — Архитектура myMediaCombain NVR

Основано на ТЗ v2.0 ([../TZ.txt](../TZ.txt)).

## 1. Компоненты

| Контейнер | Образ/стек | Назначение |
|---|---|---|
| `nginx` | nginx:alpine | Единственная внешняя точка входа, reverse proxy, отдача файлов |
| `frontend` | Next.js + TS + Tailwind | UI: setup wizard, login, live, архив, события, admin panel |
| `backend` | C++20, CMake, Crow | Работа с камерой (ONVIF/RTSP), запись, motion detection, REST API, WebSocket, HLS, экспорт |
| `postgres` | postgres:16 | Metadata: пользователи, камера, записи, события, экспорты, настройки |

## 2. Схема потоков

```
                    INTERNET (опционально, HTTPS)
                        |
                      NGINX  :80/:443
                        |
        +---------------+----------------+
        | /              | /api, /ws      | /download
        v                v                v
     Next.js         C++ Backend      Video Storage
                         |
        +---------+------+------+--------+
        |         |             |        |
      ONVIF     RTSP       Motion Det.  Export
        |         |        (OpenCV)   (FFmpeg)
        +----+----+             |
             |                  |
          IP CAMERA      PostgreSQL
                         /data/recordings
                         /data/thumbnails
                         /data/exports
```

## 3. Backend (C++) — модули

```
backend/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── api/            # HTTP REST API (endpoints из ТЗ §40)
│   ├── ws/             # WebSocket события (ТЗ §41)
│   ├── camera/         # ONVIF client, RTSP connection manager, reconnect state machine
│   ├── recorder/       # FFmpeg segment recorder (1–5 мин), запись metadata в БД
│   ├── motion/         # OpenCV pipeline: resize→gray→blur→diff→threshold→contours
│   ├── storage/        # circular overwrite, контроль свободного места
│   ├── exporter/       # объединение сегментов → MP4 (stream copy, без перекодирования)
│   ├── db/             # PostgreSQL client (libpqxx), миграции
│   ├── auth/           # сессии, password hashing (bcrypt/argon2), проверка ролей
│   └── common/         # логирование (structured), конфиг, crypto для секретов
└── tests/
```

### Reconnect state machine (ТЗ §57)

```
CONNECTED → DISCONNECTED → RECONNECTING → CONNECTED
```

Каждый переход публикует WebSocket-событие `camera.*`.

### Live view и архив (ТЗ §73.1, §73.2)

- **Live:** backend запускает FFmpeg (RTSP → HLS, stream copy) и отдаёт плейлист/сегменты через `/api/live`. Задержка 2–5 сек. WebRTC — post-MVP.
- **Архив:** backend генерирует HLS-плейлист из записанных сегментов на лету (remux MP4→HLS со stream copy — дёшево). Seek по плейлисту, склейка файлов не нужна.
- **Скачивание:** только здесь сегменты склеиваются в единый MP4 (export).

### Суб-поток (ТЗ §73.3)

Если камера отдаёт второй RTSP-поток: motion detection и live preview работают на суб-потоке (низкое разрешение), запись — на основном. Два RTSP URL в настройках камеры. Fallback — уменьшенная копия основного потока (320×180).

### Motion pipeline (ТЗ §21, §60)

Кадры берутся из суб-потока (или уменьшенной копии основного) — минимальная нагрузка на CPU. Поддержка зон detection/ignore, pre-buffer (кольцевой буфер кадров ~10 сек).

### Запись

- FFmpeg, stream copy (без перекодирования для H.264)
- Аудио записывается, если камера его отдаёт (ТЗ §73.7)
- Сегменты 5 минут (дефолт): `/data/recordings/camera/YYYY/MM/DD/HH/mmss.mp4`
- Circular overwrite: при достижении `MAX_STORAGE_USAGE` (90%) удаляются самые старые сегменты (приоритет — непрерывность записи)

## 4. Библиотеки backend (ТЗ §73.4)

| Задача | Библиотека |
|---|---|
| HTTP/WebSocket | Crow |
| JSON | nlohmann/json |
| PostgreSQL | libpqxx |
| Логирование | spdlog |
| Password hashing | libargon2 |
| Видео | FFmpeg (libav*) |
| Motion | OpenCV |
| Зависимости | apt-пакеты Debian 13 внутри Docker-образа |

## 4. База данных (PostgreSQL)

Таблицы (ТЗ §31–38): `users`, `roles`, `camera`, `camera_profiles`, `recordings`, `motion_events`, `exports`, `system_settings`.

- Пароли пользователей — только hash (argon2/bcrypt)
- Пароли камеры и RTSP URL — зашифрованы (AES-GCM, ключ из Docker secret)
- Миграции — SQL-файлы, применяются backend'ом при старте

## 5. API (REST)

См. ТЗ §40. Аутентификация — сессия (HttpOnly cookie). Каждый защищённый endpoint проверяет роль. `/api/admin/*` — только ADMIN.

WebSocket (`/ws`): realtime события `camera.*`, `motion.*`, `recording.*`, `storage.*`, `export.*` (ТЗ §41).

## 6. Frontend (Next.js)

Страницы:

```
/setup            — мастер первого запуска: создание админа, настройка камеры (ТЗ §73.5)
/login            — авторизация (username + password)
/                 — главная: live preview, статус, последние события
/archive          — выбор даты, таймлайн, HLS-плеер, скачивание
/events           — список событий движения с thumbnails
/admin            — 7 разделов (камера, видео, запись, motion, storage, users, система)
```

- Локализация: все строки в `locales/ru.json` (задел под en)
- Аутентификация: HttpOnly session cookie, срок 7 дней
- Responsive, mobile-first, touch-friendly
- Стиль: минимализм, крупные кнопки, без технических деталей для USER

## 7. Docker Compose

```yaml
services:
  nginx:     # порт 80, единственный опубликованный (HTTP в локалке, ТЗ §73.6)
  frontend:  # Next.js
  backend:   # C++ (Crow)
  postgres:  # не публикуется наружу
volumes:     # ./data/postgres, ./data/recordings, ./data/config
```

- `restart: unless-stopped` везде (автозапуск после reboot)
- Health checks для backend (`GET /health`) и postgres
- Секреты через `.env` / Docker secrets (не в Git)
- HTTPS (443) добавляется при привязке домена — post-MVP

## 8. Безопасность (чеклист)

- [ ] password hashing (argon2)
- [ ] HttpOnly session cookies
- [ ] RTSP credentials никогда не уходят во frontend
- [ ] PostgreSQL не опубликован наружу
- [ ] секреты не в Git
- [ ] проверка роли на каждом защищённом endpoint
- [ ] авторизация download endpoints
- [ ] секреты не попадают в логи

## 9. Расширения за пределами MVP

Архитектура допускает (без изменения ядра): AI-детектор (отдельный контейнер), GPU acceleration (NVDEC/QSV), уведомления (Telegram/email), расписание записи, английский язык.
