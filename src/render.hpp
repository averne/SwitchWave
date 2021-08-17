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

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <switch.h>
#include <deko3d.hpp>
#include <mpv/render_dk3d.h>

#include "libmpv.hpp"
#include "utils.hpp"

namespace sw {

class Renderer {
    public:
        constexpr static auto NumSwapchainImages = 3;
        constexpr static auto NumLibMpvImages    = 3;
        constexpr static auto CmdBufSize         = 0x10000;
        constexpr static auto MaxNumDescriptors  = 64;

    public:
        struct Texture {
            dk::Image image;
            dk::UniqueMemBlock memblock;
            DkResHandle handle = -1;
        };

    public:
        ~Renderer();

        int initialize();

        dk::Device get_device() const {
            return this->dk;
        }

        dk::Queue get_queue() const {
            return this->queue;
        }

        int create_mpv_render_context(LibmpvController &lmpv);
        void destroy_mpv_render_context();

        Texture create_texture(int width, int height,
            DkImageFormat format, std::uint32_t flags = 0);
        Texture load_texture(std::string_view path, int width, int height,
            DkImageFormat format, std::uint32_t flags = 0);

        void begin_frame();
        void end_frame();

        void wait_idle() {
            this->queue.waitIdle();
        }

        void switch_presentation_mode(bool mpv_handle_pres) {
            if (mpv_handle_pres == this->mpv_handle_pres)
                return;

            auto lk = std::scoped_lock(this->render_mtx);

            std::printf("Switching presentation mode\n");
            this->queue.waitIdle();
            this->mpv_handle_pres = mpv_handle_pres;

            if (!mpv_handle_pres) {
                this->mpv_copy_fences  = {};
                this->cur_libmpv_image = -1;
            }
        }

        void unregister_texture(const Texture &tex) {
            this->free_descriptor_slot(tex.handle & utils::mask(20));
        }

    private:
        void mpv_render_thread_fn(std::stop_token token);

#ifdef __SWITCH__
        void rebuild_swapchain();
        static void applet_hook_cb(AppletHookType hook, void *param);
#endif

        int find_descriptor_slot();
        void free_descriptor_slot(int slot);

    public:
        std::uint32_t image_width = 1280, image_height = 720;

    private:
        mpv_render_context *mpv_gl = nullptr;

        std::jthread     mpv_render_thread;
        bool             mpv_handle_pres  = true;
        std::mutex       render_mtx;
        std::atomic_bool force_mpv_render = false;

        std::condition_variable mpv_redraw_condvar;
        std::mutex              mpv_redraw_mutex;
        std::atomic_int         mpv_redraw_count = 0;

        dk::UniqueDevice       dk;
        dk::UniqueQueue        queue;
        dk::UniqueMemBlock     cmdbuf_memblock;
        dk::UniqueCmdBuf       cmdbuf;
        dk::UniqueMemBlock     descriptor_memblock;
        dk::SamplerDescriptor *sampler_descs;
        dk::ImageDescriptor   *image_descs;
        std::array<std::uint64_t, 2> allocated_descriptors = {};

        dk::UniqueMemBlock                        image_memblock;
        std::array<dk::Image, NumSwapchainImages> swapchain_images;
        std::array<dk::Image, NumLibMpvImages>    mpv_images;
        std::array<dk::Fence, NumLibMpvImages>    mpv_copy_fences  = {};
        std::array<dk::Fence, NumLibMpvImages>    ui_render_fences = {};
        std::atomic_int                           cur_libmpv_image = -1;
        int                                       cur_slot         = 0;

        dk::UniqueSwapchain swapchain;
        std::atomic_bool    need_swapchain_rebuild = true;
        AppletHookCookie    applet_hook_cookie;
};

} // namespace sw
