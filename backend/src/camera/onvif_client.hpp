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
    int width = 0;
    int height = 0;
};

class OnvifClient {
public:
    OnvifClient(std::string ip, int port, std::string username, std::string password);

    bool get_device_information(OnvifDeviceInfo& out);
    bool get_profiles(std::vector<OnvifProfile>& out);
    bool get_stream_uri(const std::string& profile_token, std::string& out_uri);

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
