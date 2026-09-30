#include "onvif_client.hpp"

#include <curl/curl.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <pugixml.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <ctime>

#include "../common/crypto.hpp"

namespace {

// Ищет первый узел с данным local-name (игнорируя namespace-префикс)
pugi::xml_node find_node(pugi::xml_node root, const char* local_name) {
    for (pugi::xml_node node : root.children()) {
        const char* name = node.name();
        const char* colon = std::strchr(name, ':');
        const char* local = colon ? colon + 1 : name;
        if (std::strcmp(local, local_name) == 0) return node;
        if (auto found = find_node(node, local_name)) return found;
    }
    return {};
}

std::vector<pugi::xml_node> find_all(pugi::xml_node root, const char* local_name) {
    std::vector<pugi::xml_node> out;
    for (pugi::xml_node node : root.children()) {
        const char* name = node.name();
        const char* colon = std::strchr(name, ':');
        const char* local = colon ? colon + 1 : name;
        if (std::strcmp(local, local_name) == 0) out.push_back(node);
        auto nested = find_all(node, local_name);
        out.insert(out.end(), nested.begin(), nested.end());
    }
    return out;
}

std::string node_text(pugi::xml_node root, const char* local_name) {
    auto node = find_node(root, local_name);
    return node ? node.text().get() : "";
}

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

OnvifClient::OnvifClient(std::string ip, int port, std::string username, std::string password)
    : ip_(std::move(ip)),
      port_(port),
      username_(std::move(username)),
      password_(std::move(password)),
      device_url_("http://" + ip_ + ":" + std::to_string(port_) + "/onvif/device_service") {}

bool OnvifClient::get_device_information(OnvifDeviceInfo& out) {
    const std::string resp = request(device_url_, "<tds:GetDeviceInformation xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/>");
    if (resp.empty()) return false;

    pugi::xml_document doc;
    if (!doc.load_string(resp.c_str())) return false;

    out.manufacturer = node_text(doc, "Manufacturer");
    out.model        = node_text(doc, "Model");
    out.firmware     = node_text(doc, "FirmwareVersion");
    out.serial       = node_text(doc, "SerialNumber");
    spdlog::info("onvif device: {} {}", out.manufacturer, out.model);
    return !out.manufacturer.empty() || !out.model.empty();
}

bool OnvifClient::get_profiles(std::vector<OnvifProfile>& out) {
    if (!resolve_media_url()) return false;

    const std::string resp = request(media_url_, "<trt:GetProfiles xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"/>");
    if (resp.empty()) return false;

    pugi::xml_document doc;
    if (!doc.load_string(resp.c_str())) return false;

    for (auto& profile : find_all(doc, "Profiles")) {
        OnvifProfile p;
        p.name  = profile.attribute("Name").value();
        p.token = profile.attribute("token").value();
        if (auto res = find_node(profile, "Resolution")) {
            p.width  = find_node(res, "Width").text().as_int();
            p.height = find_node(res, "Height").text().as_int();
        }
        out.push_back(std::move(p));
    }
    spdlog::info("onvif profiles: {}", out.size());
    return !out.empty();
}

bool OnvifClient::get_stream_uri(const std::string& profile_token, std::string& out_uri) {
    if (!resolve_media_url()) return false;

    const std::string body =
        "<trt:GetStreamUri xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<trt:StreamSetup>"
        "<tt:Stream xmlns:tt=\"http://www.onvif.org/ver10/schema\">RTP-Unicast</tt:Stream>"
        "<tt:Transport xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
        "<tt:Protocol>RTSP</tt:Protocol></tt:Transport>"
        "</trt:StreamSetup>"
        "<trt:ProfileToken>" + profile_token + "</trt:ProfileToken>"
        "</trt:GetStreamUri>";

    const std::string resp = request(media_url_, body);
    if (resp.empty()) return false;

    pugi::xml_document doc;
    if (!doc.load_string(resp.c_str())) return false;

    out_uri = node_text(doc, "Uri");
    return !out_uri.empty();
}

bool OnvifClient::resolve_media_url() {
    if (!media_url_.empty()) return true;

    const std::string resp = request(device_url_, "<tds:GetCapabilities xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"><tds:Category>Media</tds:Category></tds:GetCapabilities>");
    if (resp.empty()) return false;

    pugi::xml_document doc;
    if (!doc.load_string(resp.c_str())) return false;

    media_url_ = node_text(doc, "XAddr");
    if (media_url_.empty()) {
        // запасной вариант для камер, не отдающих capabilities
        media_url_ = "http://" + ip_ + ":" + std::to_string(port_) + "/onvif/media_service";
        spdlog::warn("onvif: capabilities empty, fallback media url");
    }
    return true;
}

std::string OnvifClient::build_envelope(const std::string& body) const {
    // WS-Security UsernameToken с PasswordDigest:
    // Digest = Base64( SHA1( nonce + created + password ) )
    unsigned char nonce[16];
    RAND_bytes(nonce, sizeof(nonce));

    char created[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));

    std::string digest_input;
    digest_input.append(reinterpret_cast<char*>(nonce), sizeof(nonce));
    digest_input += created;
    digest_input += password_;

    unsigned char sha[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(digest_input.data()), digest_input.size(), sha);

    const std::string nonce_b64  = crypto::base64_encode(nonce, sizeof(nonce));
    const std::string digest_b64 = crypto::base64_encode(sha, sizeof(sha));

    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Header>"
        "<wsse:Security xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
        "<wsse:UsernameToken>"
        "<wsse:Username>" + username_ + "</wsse:Username>"
        "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" +
            digest_b64 + "</wsse:Password>"
        "<wsse:Nonce>" + nonce_b64 + "</wsse:Nonce>"
        "<wsu:Created xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">" +
            std::string(created) + "</wsu:Created>"
        "</wsse:UsernameToken>"
        "</wsse:Security>"
        "</s:Header>"
        "<s:Body>" + body + "</s:Body>"
        "</s:Envelope>";
}

std::string OnvifClient::request(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    const std::string envelope = build_envelope(body);
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, envelope.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/soap+xml; charset=utf-8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        spdlog::warn("onvif request failed: {}", curl_easy_strerror(rc));
        response.clear();
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}
