#pragma once
// Проверка RTSP-потока через libavformat (ТЗ §28).
#include <string>

namespace rtsp {

// Пытается открыть RTSP-поток и прочитать несколько кадров.
// timeout_sec — таймаут подключения (TCP).
// Возвращает true, если поток открылся и удалось получить видеопакет.
bool probe(const std::string& url, int timeout_sec = 10);

}  // namespace rtsp
