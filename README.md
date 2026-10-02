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
- Режимы записи: постоянная (continuous) или по движению (motion) с pre-buffer 10 с
- Циклическая перезапись: при заполнении диска удаляются самые старые сегменты
- Простой интерфейс для пользователя без IT-навыков (русский язык)

## Этапы разработки (roadmap)

- [x] **Phase 1** — Infrastructure ✅ проверено на Debian 13
- [x] **Phase 2** — Camera (ONVIF/RTSP/reconnect) + auth (argon2id, сессии, rate limit) + setup wizard ✅ API проверено
- [x] **Phase 3** — Recording: сегментная запись (stream copy, fMP4), circular overwrite, /api/storage, запись по движению (pre-buffer + cooldown) ✅ проверено
- [x] **Phase 4** — Motion: OpenCV pipeline, зоны detect/ignore, события, thumbnails, /api/events ✅ код собран
- [x] **Phase 5** — Web UI: setup wizard, login, live (hls.js), архив, события, скачивание ✅ UI рендерится
- [x] **Phase 6** — Admin panel: камера, видео (ONVIF), запись, motion, хранилище, пользователи, система, логи ✅ код собран
- [x] **Phase 7** — Hardening: health checks + autoheal (recovery), ротация логов (docker + spdlog), backup/restore скрипты ✅ (HTTPS — при привязке домена, ТЗ §73.6)

## Статус развёртывания

Система **развёрнута и полностью протестирована** на Debian 13 (VirtualBox):
smoke-тест 31/31 PASS — auth (argon2id, сессии, rate limit 429), RBAC (USER/ADMIN),
камера (RTSP reconnect, вкл/выкл), запись сегментов с ротацией (continuous и
по движению с pre-buffer), live HLS, motion-события с thumbnails и зонами,
архивный HLS (включая активный сегмент и дыры от circular overwrite),
экспорт MP4, circular overwrite (max_storage_usage + min_free_space),
WS-realtime в UI, autoheal (SIGSTOP-тест: автоперезапуск за ~90 с), backup/restore.
Soak: 0 ошибок backend за 30 мин под нагрузкой.

**Аудит (2026-10):** полный ручной аудит backend/frontend/infra, закрыты:
типобезопасный разбор JSON во всех роутах (исключение не покидает handler),
RTSP-таймауты под FFmpeg 7 + interrupt_callback (мёртвая сеть не вешает запись),
серверная валидация значений настроек, recovery «осиротевших» записей при старте
+ SIGTERM-graceful stop, потолок ожидания ffmpeg в экспорте, чистка файлов-сирот,
честные длительность/время motion-событий (фильтр коротких срабатываний),
лимит тела запроса в nginx, no-new-privileges для контейнеров, UI для лимитов
хранилища и pre-buffer.

Тестирование без реальной камеры: mediamtx + ffmpeg-издатели
(`testsrc2`) в той же docker-сети, RTSP `rtsp://rtsp-test:8554/cam` (+`/cam_sub`).

⏳ Осталось проверить с реальной камерой: ONVIF-автообнаружение профилей
и запись видео-параметров (вкладка «Видео» реализована: чтение конфигурации/опций,
SetVideoEncoderConfiguration с проверкой применения; на тестовом стенде нет
ONVIF-устройства — проверен только путь graceful-degradation «камера не отвечает по ONVIF»).

## Запуск

См. [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

```bash
cp .env.example .env
mkdir -p data/config && openssl rand -hex 32 > data/config/secret_key
docker compose up -d --build
```

## Документация

Документация ведётся параллельно с кодом: каждое изменение кода сопровождается обновлением README и документов в `docs/`.
