# myMediaCombain

Домашняя WEB-система видеонаблюдения / NVR для **одной IP-камеры**.

Полное техническое задание: [docs/TZ.txt](docs/TZ.txt) (v2.1).

## Что делает система

Пользователь через браузер (desktop и mobile) может:

- смотреть камеру в реальном времени (HLS; WebRTC — в следующей версии);
- просматривать архив записей по дате/времени (HLS-плейлист с seek);
- находить события движения (motion detection);
- скачивать видеофрагменты в MP4.

## Роли

| Роль | Возможности |
|---|---|
| **USER** | live view, архив, события, скачивание |
| **ADMIN** | всё, что USER + настройки камеры, записи, motion, storage, пользователи |

## Технологический стек

| Слой | Технологии |
|---|---|
| Frontend | Next.js, TypeScript, React, Tailwind CSS |
| Backend | C++20+, REST API, WebSocket, FFmpeg/libav, OpenCV, ONVIF |
| База данных | PostgreSQL (только metadata) |
| Reverse proxy | Nginx (единственная внешняя точка входа) |
| Платформа | Debian 13, Docker, Docker Compose |

## Архитектура

```
Browser → Nginx ─┬─→ Next.js (UI)
                 └─→ C++ Backend ─┬─→ ONVIF/RTSP → IP Camera
                                  ├─→ PostgreSQL (metadata)
                                  └─→ /data/recordings (видео на диске)
```

## Структура репозитория

```
myMediaCombain/
├── docs/
│   ├── TZ.txt            # Техническое задание v2.1
│   ├── DESIGN.md         # Архитектура
│   └── DEVELOPMENT.md    # Запуск и разработка
├── README.md             # Этот файл (в корне — показывается на GitHub)
├── frontend/             # Next.js приложение
├── backend/              # C++ backend
├── nginx/                # Конфигурация Nginx
├── docker-compose.yml    # Оркестрация контейнеров
└── data/                 # Persistent volumes (postgres, recordings, config)
```

## Ключевые принципы

- Одна камера, только WEB-интерфейс
- RTSP не покидает backend; браузеру отдаётся WebRTC/HLS
- Видео хранится на диске сегментами 1–5 минут
- Циклическая перезапись: при заполнении диска удаляются самые старые сегменты
- Простой интерфейс для пользователя без IT-навыков (русский язык)

## Этапы разработки (roadmap)

- [x] **Phase 1** — Infrastructure: Docker Compose, PostgreSQL, Nginx, Next.js, C++ backend skeleton ✅ (код готов, проверка запуска — на Debian)
- [~] **Phase 2** — Camera: ONVIF, RTSP, reconnect, статус + auth (argon2, сессии, rate limit) + setup wizard ✅ (код готов, не тестирован)
- [~] **Phase 3** — Recording: сегментная запись (stream copy, fMP4), circular overwrite, /api/storage ✅ (код готов, не тестирован)
- [~] **Phase 4** — Motion: OpenCV pipeline, зоны detect/ignore, события, thumbnails, /api/events ✅ (код готов, не тестирован)
- [~] **Phase 5** — Web UI: setup wizard, login, live (hls.js), архив, события, скачивание ✅ (код готов, не тестирован)
- [~] **Phase 6** — Admin panel: камера, запись, motion, хранилище, пользователи, система ✅ (код готов, не тестирован)
- [ ] **Phase 7** — Hardening: HTTPS, health checks, recovery

## Запуск

См. [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

```bash
cp .env.example .env
mkdir -p data/config && openssl rand -hex 32 > data/config/secret_key
docker compose up -d --build
```

## Документация

Документация ведётся параллельно с кодом: каждое изменение кода сопровождается обновлением README и документов в `docs/`.
