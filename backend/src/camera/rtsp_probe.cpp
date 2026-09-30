#include "rtsp_probe.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace rtsp {

// URL содержит credentials — никогда не логируем его (ТЗ §58)
bool probe(const std::string& url, int timeout_sec) {
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) return false;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", std::to_string(timeout_sec * 1'000'000).c_str(), 0);
    av_dict_set(&opts, "max_delay", "500000", 0);

    bool ok = false;
    if (avformat_open_input(&fmt, url.c_str(), nullptr, &opts) == 0) {
        if (avformat_find_stream_info(fmt, nullptr) >= 0) {
            AVPacket* pkt = av_packet_alloc();
            // ждём первый видеопакет (до 50 пакетов)
            for (int i = 0; i < 50 && !ok; ++i) {
                if (av_read_frame(fmt, pkt) < 0) break;
                const auto* par = fmt->streams[pkt->stream_index]->codecpar;
                if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                    spdlog::info("rtsp probe ok: {}x{}, codec_id={}",
                                 par->width, par->height, static_cast<int>(par->codec_id));
                    ok = true;
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        } else {
            spdlog::warn("rtsp probe: no stream info");
        }
    } else {
        spdlog::warn("rtsp probe: connection failed");
    }

    av_dict_free(&opts);
    avformat_close_input(&fmt);
    return ok;
}

}  // namespace rtsp
