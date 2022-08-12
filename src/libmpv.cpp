#include <cstdio>
#include <cstring>

#include "libmpv.hpp"

namespace ampnx {

int LibmpvController::initialize() {
    this->mpv = mpv_create();
    if (!this->mpv)
        return MPV_ERROR_NOMEM;

    MPV_CALL(mpv_request_log_messages(this->mpv, "debug"));

    MPV_CALL(mpv_set_option_string(this->mpv, "config", "yes"));
    MPV_CALL(mpv_set_option_string(this->mpv, "config-dir", MpvDirectory));

    MPV_CALL(mpv_initialize(this->mpv));

    MPV_CALL(mpv_set_option_string(this->mpv, "hwdec", "auto"));
    MPV_CALL(mpv_set_option_string(this->mpv, "hwdec-codecs", "mpeg1video,mpeg2video,mpeg4,"
        "vc1,wmv3,h264,hevc,vp8,vp9"));
    MPV_CALL(mpv_set_option_string(this->mpv, "framedrop", "decoder+vo"));
    MPV_CALL(mpv_set_option_string(this->mpv, "vd-lavc-dr", "yes"));
    MPV_CALL(mpv_set_option_string(this->mpv, "vd-lavc-threads", "4"));
    MPV_CALL(mpv_set_option_string(this->mpv, "vd-lavc-skiploopfilter", "all"));
    MPV_CALL(mpv_set_option_string(this->mpv, "vd-lavc-skipframe", "default"));
    MPV_CALL(mpv_set_option_string(this->mpv, "vd-lavc-framedrop", "nonref"));
    MPV_CALL(mpv_set_option_string(this->mpv, "vd-lavc-fast", "yes"));
    // MPV_CALL(mpv_set_option_string(this->mpv, "cache", "yes"));
    // MPV_CALL(mpv_set_option_string(this->mpv, "fbo-format", "rgba8"));
    // MPV_CALL(mpv_set_option_string(this->mpv, "hdr-compute-peak", "no"));

    return 0;
}

LibmpvController::~LibmpvController() {
    mpv_destroy(this->mpv);
}

void LibmpvController::process_events() {
    mpv_event *event;
    do {
        event = mpv_wait_event(this->mpv, 0);
        switch (event->event_id) {
            case MPV_EVENT_LOG_MESSAGE:
                {
                    auto *msg = static_cast<mpv_event_log_message *>(event->data);
                    if (this->log_callback)
                        this->log_callback(this->log_callback_user, msg);
                }
                break;
            case MPV_EVENT_PROPERTY_CHANGE:
            case MPV_EVENT_GET_PROPERTY_REPLY:
                {
                    auto *prop = static_cast<mpv_event_property *>(event->data);

                    auto &map = (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY) ?
                        this->async_properties : this->observed_properties;
                    auto it = map.find(prop->name);

                    if (it != map.end()) {
                        auto &res = it->second;

                        if (res.format == prop->format && prop->data) {
                            if (res.callback)
                                res.callback(res.callback_user, prop);
                            if (res.data)
                                std::memcpy(res.data, prop->data, (res.format == MPV_FORMAT_FLAG) ? 4 : 8);
                        }

                        if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY)
                            this->async_properties.erase(it);
                    }
                }
            default:
                break;
        }
    } while (event->event_id != MPV_EVENT_NONE);
}

int LibmpvController::observe_property(std::string_view name, mpv_format fmt, void *data,
        PropertyCallback callback, void *user) {
    auto res = mpv_observe_property(this->mpv, 0, name.data(), fmt);
    if (!res)
        this->observed_properties.try_emplace(name, fmt, data, callback, user);
    return res;
}

int LibmpvController::get_property_async(std::string_view name, mpv_format fmt, void *data,
        PropertyCallback callback, void *user) {
    auto res = mpv_get_property_async(this->mpv, 0, name.data(), fmt);
    if (!res)
        this->async_properties.try_emplace(name, fmt, data, callback, user);
    return res;
}

int LibmpvController::unobserve_property(std::string_view name) {
    auto res = mpv_unobserve_property(this->mpv, 0);

    // Unconditionally remove observer from our list
    auto it = this->observed_properties.find(name);
    if (it != this->observed_properties.end())
        this->observed_properties.erase(name);
    return res;
}

int LibmpvController::load_file(const char *path) {
    const char *cmd[] = {"loadfile", path, NULL};
    return mpv_command_async(this->mpv, 0, cmd);
}

int LibmpvController::tell(std::int64_t &seconds) {
    return mpv_get_property(this->mpv, "time-pos", MPV_FORMAT_INT64, &seconds);
}

int LibmpvController::seek(std::int64_t seconds, bool relative) {
    char time[10] = {};
    std::snprintf(time, sizeof(time), "%+ld", seconds);
    const char *cmd[] = {"seek", time, relative ? "relative" : "", NULL};
    return mpv_command_async(this->mpv, 0, cmd);
}

} // namespace ampnx
