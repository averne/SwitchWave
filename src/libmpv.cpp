// Copyright (c) 2024 averne <averne381@gmail.com>
//
// This file is part of SwitchWave.
//
// SwitchWave is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// SwitchWave is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SwitchWave.  If not, see <http://www.gnu.org/licenses/>.

#include <cstdio>
#include <cstring>

#include "libmpv.hpp"

namespace sw {

int LibmpvController::initialize() {
    this->mpv = mpv_create();
    if (!this->mpv)
        return MPV_ERROR_NOMEM;

    MPV_CALL(mpv_request_log_messages(this->mpv, "terminal-default"));

    MPV_CALL(mpv_set_option_string(this->mpv, "config", "yes"));
    MPV_CALL(mpv_set_option_string(this->mpv, "config-dir", LibmpvController::MpvDirectory.data()));
    MPV_CALL(mpv_set_option_string(this->mpv, "user-agent", "SwitchWave/1.0"));

    MPV_CALL(mpv_initialize(this->mpv));

    return 0;
}

LibmpvController::~LibmpvController() {
    mpv_terminate_destroy(this->mpv);
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
            case MPV_EVENT_FILE_LOADED:
                {
                    if (this->file_loaded_callback)
                        this->file_loaded_callback(this->file_loaded_callback_user);
                }
                break;
            case MPV_EVENT_END_FILE:
                {
                    auto *end = static_cast<mpv_event_end_file *>(event->data);
                    if (this->end_file_callback)
                        this->end_file_callback(this->end_file_callback_user, end);
                }
                break;
            case MPV_EVENT_IDLE:
                {
                    if (this->idle_callback)
                        this->idle_callback(this->idle_callback_user);
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
                break;
            case MPV_EVENT_SET_PROPERTY_REPLY:
                {
                    if (event->error)
                        std::printf("Got error reply for set async property: %d\n", event->error);
                    // auto *prop = static_cast<mpv_event_property *>(event->data);
                }
                break;
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

} // namespace sw
