-- HomeNVR — миграция 002: настройки зон motion detection (ТЗ §23)
-- Формат: JSON-массив прямоугольников [{x,y,w,h}] в долях кадра 0..1.
-- Пустой массив зон детекции = весь кадр.

INSERT INTO system_settings (key, value) VALUES
    ('motion_zones_detect', '[]'),
    ('motion_zones_ignore', '[]')
ON CONFLICT (key) DO NOTHING;
