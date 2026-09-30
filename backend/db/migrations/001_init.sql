-- HomeNVR — миграция 001: начальная схема (ТЗ §31-38)
-- PostgreSQL хранит только metadata. Видео — на файловой системе.

-- Роли (ТЗ §33)
CREATE TABLE roles (
    id   SERIAL PRIMARY KEY,
    name TEXT NOT NULL UNIQUE CHECK (name IN ('USER', 'ADMIN'))
);

INSERT INTO roles (name) VALUES ('USER'), ('ADMIN');

-- Пользователи (ТЗ §32)
-- Пароль — только argon2 hash. Email не используется (ТЗ §73.8).
CREATE TABLE users (
    id            SERIAL PRIMARY KEY,
    username      TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    role_id       INTEGER NOT NULL REFERENCES roles(id),
    enabled       BOOLEAN NOT NULL DEFAULT TRUE,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Сессии (HttpOnly cookie, срок 7 дней — ТЗ §73.8)
CREATE TABLE sessions (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires_at TIMESTAMPTZ NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Камера (ТЗ §34). Система рассчитана на одну камеру (ТЗ §6).
-- Пароль камеры зашифрован (AES-GCM), RTSP credentials не покидают backend.
CREATE TABLE camera (
    id                SERIAL PRIMARY KEY,
    name              TEXT NOT NULL DEFAULT 'Камера',
    ip_address        TEXT NOT NULL DEFAULT '',
    onvif_port        INTEGER NOT NULL DEFAULT 80,
    username          TEXT NOT NULL DEFAULT '',
    password_encrypted TEXT NOT NULL DEFAULT '',
    manufacturer      TEXT NOT NULL DEFAULT '',
    model             TEXT NOT NULL DEFAULT '',
    enabled           BOOLEAN NOT NULL DEFAULT TRUE,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Профили камеры (ТЗ §35). Суб-поток — отдельный профиль (ТЗ §73.3).
CREATE TABLE camera_profiles (
    id                 SERIAL PRIMARY KEY,
    camera_id          INTEGER NOT NULL REFERENCES camera(id) ON DELETE CASCADE,
    profile_name       TEXT NOT NULL,
    is_substream       BOOLEAN NOT NULL DEFAULT FALSE,
    width              INTEGER,
    height             INTEGER,
    fps                INTEGER,
    bitrate            INTEGER,
    codec              TEXT,
    rtsp_url_encrypted TEXT NOT NULL DEFAULT '',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Записи / сегменты (ТЗ §36)
CREATE TABLE recordings (
    id          BIGSERIAL PRIMARY KEY,
    camera_id   INTEGER NOT NULL REFERENCES camera(id) ON DELETE CASCADE,
    started_at  TIMESTAMPTZ NOT NULL,
    ended_at    TIMESTAMPTZ,
    file_path   TEXT NOT NULL,
    file_size   BIGINT,
    duration    INTERVAL,
    codec       TEXT,
    width       INTEGER,
    height      INTEGER,
    fps         INTEGER,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_recordings_camera_time ON recordings (camera_id, started_at);

-- События движения (ТЗ §37)
CREATE TABLE motion_events (
    id             BIGSERIAL PRIMARY KEY,
    camera_id      INTEGER NOT NULL REFERENCES camera(id) ON DELETE CASCADE,
    recording_id   BIGINT REFERENCES recordings(id) ON DELETE SET NULL,
    started_at     TIMESTAMPTZ NOT NULL,
    ended_at       TIMESTAMPTZ,
    duration       INTERVAL,
    motion_score   REAL,
    thumbnail_path TEXT,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_motion_events_camera_time ON motion_events (camera_id, started_at);

-- Экспорты (ТЗ §38)
CREATE TABLE exports (
    id         BIGSERIAL PRIMARY KEY,
    camera_id  INTEGER NOT NULL REFERENCES camera(id) ON DELETE CASCADE,
    start_time TIMESTAMPTZ NOT NULL,
    end_time   TIMESTAMPTZ NOT NULL,
    file_path  TEXT,
    status     TEXT NOT NULL DEFAULT 'QUEUED'
               CHECK (status IN ('QUEUED', 'PROCESSING', 'READY', 'FAILED', 'EXPIRED')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at TIMESTAMPTZ
);

-- Системные настройки (ТЗ §31) — key-value
CREATE TABLE system_settings (
    key        TEXT PRIMARY KEY,
    value      JSONB NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Дефолты (ТЗ §73.8)
INSERT INTO system_settings (key, value) VALUES
    ('recording_mode',       '"continuous"'),
    ('segment_duration_sec', '300'),
    ('pre_buffer_sec',       '10'),
    ('max_storage_usage',    '0.9'),
    ('min_free_space',       '0.1'),
    ('overwrite_enabled',    'true'),
    ('motion_enabled',       'true'),
    ('motion_sensitivity',   '0.5'),
    ('motion_min_event_sec', '3'),
    ('motion_cooldown_sec',  '10'),
    ('setup_completed',      'false');
