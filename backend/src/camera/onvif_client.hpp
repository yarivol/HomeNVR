#pragma once
// Минимальный ONVIF-клиент (ТЗ §27): device info, профили, RTSP URI.
// Реализован ровно то, что нужно системе — полный ONVIF не цель.
#include <string>
#include <vector>

struct OnvifDeviceInfo {
    std::string manufacturer;
    std::string model;
    std::string firmware;
    std::string serial;
};

struct OnvifProfile {
    std::string name;
    std::string token;
    std::string vec_token;   // token VideoEncoderConfiguration этого профиля
    int width = 0;
    int height = 0;
};

// Текущая конфигурация видеокодировщика (ТЗ §29, §45)
struct VideoEncoderConfig {
    std::string token;        // config token (обязателен для Set)
    std::string encoding;     // H264 / H265 / JPEG
    int width = 0;
    int height = 0;
    int quality = 0;
    int fps = 0;              // FrameRateLimit
    int bitrate_kbps = 0;     // BitrateLimit
    std::string raw_xml;      // сериализованный узел Configuration (для полного resend при Set)
};

// Доступные варианты настроек (GetVideoEncoderConfigurationOptions)
struct VideoEncoderOptions {
    std::vector<std::pair<int, int>> resolutions;  // width x height
    int fps_min = 0, fps_max = 0;
    int bitrate_min = 0, bitrate_max = 0;          // kbps
    int quality_min = 0, quality_max = 0;
};

class OnvifClient {
public:
    OnvifClient(std::string ip, int port, std::string username, std::string password);

    bool get_device_information(OnvifDeviceInfo& out);
    bool get_profiles(std::vector<OnvifProfile>& out);
    bool get_stream_uri(const std::string& profile_token, std::string& out_uri);

    // Видео-настройки (ТЗ §29): чтение текущей конфигурации, опций и запись.
    // set — шлёт ПОЛНУЮ конфигурацию (ONVIF требует), изменённые поля подставлены.
    bool get_video_encoder_config(const std::string& profile_token, VideoEncoderConfig& out);
    bool get_video_encoder_options(const std::string& profile_token, const std::string& config_token,
                                   VideoEncoderOptions& out);
    bool set_video_encoder_config(const VideoEncoderConfig& cfg);

private:
    // SOAP-запрос с WS-Security UsernameToken (PasswordDigest)
    std::string request(const std::string& url, const std::string& body);
    // URL media-сервиса из GetCapabilities (кэшируется)
    bool resolve_media_url();
    std::string build_envelope(const std::string& body) const;

    std::string ip_;
    int port_;
    std::string username_;
    std::string password_;
    std::string device_url_;
    std::string media_url_;
};
