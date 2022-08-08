#include <cstdio>

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
    // MPV_CALL(mpv_set_option_string(this->mpv, "hdr-compute-peak", "no"));

    return 0;
}

LibmpvController::~LibmpvController() {
    mpv_destroy(this->mpv);
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
