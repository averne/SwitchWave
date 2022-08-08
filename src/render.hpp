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

namespace ampnx {

class Renderer {
    public:
        constexpr static auto NumSwapchainImages = 2;
        constexpr static auto NumLibMpvImages    = 3;
        constexpr static auto CmdBufSize = 0x4000;

    public:
        ~Renderer();

        int initialize(LibmpvController &mpv);

        void begin_frame();
        void end_frame();

    private:
        void mpv_render_thread_fn(std::stop_token token);

#ifdef __SWITCH__
        void rebuild_swapchain();
        static void applet_hook_cb(AppletHookType hook, void *param);
#endif

    private:
        std::uint32_t image_width = 1280, image_height = 720;

        mpv_render_context *mpv_gl;

        std::jthread mpv_render_thread;

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

        dk::UniqueSwapchain swapchain;
        std::atomic_bool    need_swapchain_rebuild = true;
        std::mutex          swapchain_rebuild_mtx;
        AppletHookCookie    applet_hook_cookie;

        dk::UniqueMemBlock                        image_memblock;
        std::array<dk::Image, NumSwapchainImages> swapchain_images;
        std::array<dk::Image, NumLibMpvImages>    mpv_images;
        std::array<dk::Fence, NumLibMpvImages>    mpv_copy_fences;
        std::atomic_int                           cur_libmpv_image = -1;
};

} // namespace ampnx
