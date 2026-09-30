# DESIGN.md — Архитектура myMediaCombain NVR

Основано на ТЗ v2.1 ([TZ.txt](TZ.txt)).

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
├── Dockerfile
├── db/migrations/    # SQL-миграции, применяются при старте
└── src/
    ├── main.cpp          # точка входа, роутинг, сборка компонентов
    ├── api/              # REST API: auth, setup wizard, camera, storage (ТЗ §40)
    ├── auth/             # argon2id хеширование, сессии (HttpOnly cookie), require_user/require_admin
    ├── camera/           # ONVIF-клиент (WS-Security), RTSP probe (libavformat),
    │                     # CameraManager: state machine + exponential backoff reconnect (ТЗ §57)
    ├── recorder/         # SegmentRecorder: RTSP → MP4 сегменты 5 мин, stream copy,
    │                     # ротация на кейфрейме, fMP4 (файл читаем даже при обрыве), metadata в БД.
    │                     # Режимы записи (ТЗ §18): continuous (всегда) | motion (pre-buffer
    │                     # кольцевой буфер 10с + запись пока есть движение + cooldown 10с,
    │                     # события от MotionDetector)
    ├── stream/           # LiveStream: ffmpeg RTSP→HLS под супервизором (ТЗ §73.1)
    ├── exporter/         # Exporter: очередь экспортов, concat сегментов → MP4 (stream copy),
    │                     # TTL 1 час, статусы QUEUED/PROCESSING/READY/FAILED/EXPIRED (ТЗ §38)
    ├── motion/           # MotionDetector: OpenCV pipeline (ТЗ §21) на суб-потоке,
    │                     # зоны detect/ignore (ТЗ §23), cooldown + min duration (ТЗ §22),
    │                     # thumbnails JPEG (ТЗ §26), события в motion_events (ТЗ §24)
    ├── storage/          # StorageManager: circular overwrite (ТЗ §20), statvfs,
    │                     # удаление старейших сегментов до 85%, storage.warning/critical в WS
    ├── ws/               # WebSocket-хаб, broadcast событий (ТЗ §41)
    ├── db/               # libpqxx: подключение с retry, миграции, транзакции
    └── common/           # конфиг из env, AES-256-GCM для секретов камеры (ТЗ §39)
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

Страницы (реализовано):

```
/setup            — мастер первого запуска: создание админа, настройка камеры (ТЗ §73.5)
/login            — авторизация (username + password)
/                 — главная: live HLS-плеер, статус камеры, последние события, WS-realtime
/archive          — выбор даты, список сегментов, HLS-просмотр, скачивание MP4
/events           — события движения за выбранный день с thumbnails
/admin            — 6 вкладок: камера, запись, motion, хранилище, пользователи, система
```

Ключевые файлы: `lib/api.ts` (fetch + редирект на 401), `lib/useAuth.ts` (guard + setup-check),
`components/HlsPlayer.tsx` (hls.js, нативный HLS в Safari).

- Локализация: `locales/ru.json` (задел под en; строки постепенно переносятся)
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

Безопасность — приоритет с первых фаз, а не «допилим потом».

- [x] password hashing (argon2id: 64 МБ, 3 итерации)
- [x] HttpOnly + SameSite=Lax session cookies (флаг Secure — при включении HTTPS)
- [x] rate limit на логин: 5 неудач → блокировка 60 сек
- [x] RTSP credentials никогда не уходят во frontend (в БД — AES-256-GCM)
- [x] PostgreSQL не опубликован наружу (нет `ports:` в compose)
- [x] секреты не в Git (.env, data/ в .gitignore; ключ — Docker secret)
- [x] проверка роли на каждом защищённом endpoint (require_user / require_admin)
- [x] setup wizard закрывается после создания первого админа (нет дефолтных паролей)
- [x] nginx: server_tokens off, X-Content-Type-Options, X-Frame-Options, Referrer-Policy, CSP
  - ⚠️ `script-src` включает `'unsafe-inline'` — иначе не работает гидратация Next.js
    App Router (inline-скрипты). Для LAN-приложения приемлемо; при переходе на HTTPS
    стоит перейти на nonce-based CSP.
- [x] параметризованные SQL-запросы (exec_params) — защита от SQL injection
- [x] секреты не попадают в логи (RTSP URL никогда не логируется)
- [x] авторизация download endpoints (X-Accel-Redirect: backend проверяет, nginx отдаёт)
- [ ] HTTPS при публикации в Интернет (post-MVP)

## 9. Расширения за пределами MVP

Архитектура допускает (без изменения ядра): AI-детектор (отдельный контейнер), GPU acceleration (NVDEC/QSV), уведомления (Telegram/email), расписание записи, английский язык.
