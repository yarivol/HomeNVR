#pragma once
// Прерывание блокирующих операций libav (H2 аудита).
//
// FFmpeg сам не контролирует таймауты «молча» умерших TCP-соединений
// (камера выдернута без RST, NAT-timeout, half-open). Опция RTSP "stimeout"
// удалена в FFmpeg 7 (Debian 13 = ffmpeg 7.1) — молча игнорируется.
//
// interrupt_callback вызывается libav периодически внутри блокирующих вызовов
// (avformat_open_input / av_read_frame / ...): вернув 1, мы прерываем операцию,
// и вызов возвращает ошибку вместо вечного ожидания.
//
// ВАЖНО: контекст обязан жить дольше, чем используется AVFormatContext
// (FFmpeg хранит только указатель) — размещать на стеке функции сессии.
#include <atomic>
#include <chrono>
#include <limits>

namespace avx {

class Interrupt {
public:
    // монотонный дедлайн в мс; LLONG_MAX = дедлайн не активен
    std::atomic<long long> deadline_ms_{std::numeric_limits<long long>::max()};

    static long long now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // взвести таймер: операция обязана уложиться в sec секунд
    void arm_seconds(int sec) { deadline_ms_ = now_ms() + sec * 1000LL; }
    void disarm() { deadline_ms_ = std::numeric_limits<long long>::max(); }

    // сигнатура совместима с AVIOInterruptCB (int(void*))
    static int check(void* opaque) {
        auto* self = static_cast<Interrupt*>(opaque);
        return now_ms() > self->deadline_ms_.load() ? 1 : 0;
    }
};

}  // namespace avx
