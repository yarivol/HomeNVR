# myMediaCombain

Домашняя WEB-система видеонаблюдения / NVR для **одной IP-камеры**.

Полное техническое задание: [TZ.txt](TZ.txt) (v2.0).

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
├── TZ.txt              # Техническое задание v2.0
├── README.md           # Этот файл
├── docs/               # Документация (обновляется вместе с кодом)
├── frontend/           # Next.js приложение
├── backend/            # C++ backend
├── nginx/              # Конфигурация Nginx
├── docker-compose.yml  # Оркестрация контейнеров
└── data/               # Persistent volumes (postgres, recordings, config)
```

## Ключевые принципы

- Одна камера, только WEB-интерфейс
- RTSP не покидает backend; браузеру отдаётся WebRTC/HLS
- Видео хранится на диске сегментами 1–5 минут
- Циклическая перезапись: при заполнении диска удаляются самые старые сегменты
- Простой интерфейс для пользователя без IT-навыков (русский язык)

## Этапы разработки (roadmap)

- [ ] **Phase 1** — Infrastructure: Docker Compose, PostgreSQL, Nginx, Next.js, C++ backend skeleton
- [ ] **Phase 2** — Camera: ONVIF, RTSP, reconnect, статус
- [ ] **Phase 3** — Recording: сегментная запись, circular overwrite
- [ ] **Phase 4** — Motion: OpenCV detection, зоны, события, thumbnails
- [ ] **Phase 5** — Web UI: login, live, архив, события, скачивание
- [ ] **Phase 6** — Admin panel: все разделы настроек
- [ ] **Phase 7** — Hardening: безопасность, HTTPS, логи, health checks

## Документация

Документация ведётся параллельно с кодом: каждое изменение кода сопровождается обновлением README и документов в `docs/`.
