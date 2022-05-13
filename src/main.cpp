#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <mpv/client.h>
#include <mpv/render_dk3d.h>

#ifdef __SWITCH__
#   include <switch.h>
#   include <deko3d.hpp>
#endif

using namespace std::chrono_literals;

#define NUM_SWAPCHAIN_IMAGES 3

struct DrawContext {
    dk::Device &dk;
    dk::Queue  &queue;

    int image_width = 1280, image_height = 720;
    dk::UniqueMemBlock image_memblock = {};
    std::array<dk::Image, NUM_SWAPCHAIN_IMAGES> images = {};
    dk::UniqueSwapchain swapchain = {};
    std::atomic_bool need_swapchain_rebuild = true;

    mpv_render_context *mpv_gl;

    std::condition_variable redraw_condvar = {};
    std::mutex redraw_mutex = {};
    std::atomic_int redraw_count = 0;
};

static void die(const char *msg) {
    std::fprintf(stderr, "%s\n", msg);
    std::exit(1);
}

namespace {

#ifdef __SWITCH__
AppletHookCookie applet_hook_cookie;
#endif

} // namespace

#ifdef __SWITCH__
extern "C" void userAppInit(void) {
    appletLockExit();

    socketInitializeDefault();
    nxlinkStdio();

    plInitialize(PlServiceType_User);
}

extern "C" void userAppExit() {
    plExit();

    socketExit();

    appletUnlockExit();
}

void applet_hook_cb(AppletHookType hook, void *param) {
    auto *ctx = static_cast<DrawContext *>(param);
	switch (hook) {
        case AppletHookType_OnOperationMode:
            ctx->need_swapchain_rebuild = true;
        default:
            break;
	}
}
#endif

void rebuild_swapchain(DrawContext &ctx) {
    if (appletGetOperationMode() == AppletOperationMode_Console)
        ctx.image_width = 1920, ctx.image_height = 1080;
    else
        ctx.image_width = 1280, ctx.image_height = 720;

    std::printf("Rebuilding swapchain: %ux%u\n", ctx.image_width, ctx.image_height);

    // TODO: Remove once a release of deko3d has been published with #fd315f0
    ctx.swapchain      = nullptr;
    ctx.image_memblock = nullptr;

    dk::ImageLayout fb_layout;
    dk::ImageLayoutMaker(ctx.dk)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(ctx.image_width, ctx.image_height)
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine)
        .initialize(fb_layout);

    std::size_t fb_size  = fb_layout.getSize();
    std::size_t fb_align = fb_layout.getAlignment();
    fb_size = (fb_size + fb_align - 1) & ~(fb_align - 1);

    ctx.image_memblock = dk::MemBlockMaker(ctx.dk, NUM_SWAPCHAIN_IMAGES * fb_size)
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();

    std::array<const DkImage *, NUM_SWAPCHAIN_IMAGES> swapchain_images;
    for (int i = 0; i < NUM_SWAPCHAIN_IMAGES; ++i) {
        ctx.images[i].initialize(fb_layout, ctx.image_memblock, i * fb_size);
        swapchain_images[i] = &ctx.images[i];
    }

    ctx.swapchain = dk::SwapchainMaker(ctx.dk, nwindowGetDefault(), swapchain_images)
        .create();

    ctx.need_swapchain_rebuild = false;
}

void render_thread_fn(std::stop_token token, DrawContext &ctx) {
    while (!token.stop_requested()) {
        if (!ctx.redraw_count) {
            std::unique_lock lk(ctx.redraw_mutex);
            if (ctx.redraw_condvar.wait_for(lk, 100ms) == std::cv_status::timeout)
                continue;
        }

        if (ctx.need_swapchain_rebuild)
            rebuild_swapchain(ctx);

        ctx.redraw_count--;
        if (!(mpv_render_context_update(ctx.mpv_gl) & MPV_RENDER_UPDATE_FRAME))
            continue;

        int slot;
        dk::Fence fence;
        ctx.swapchain.acquireImage(slot, fence);

        mpv_deko3d_fbo fbo = {
            .tex    = &ctx.images[slot],
            .fence  = &fence,
            .w      = ctx.image_width,
            .h      = ctx.image_height,
            .format = DkImageFormat_RGBA8_Unorm,
        };

        int flip_y = 1;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_DEKO3D_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y,     &flip_y},
            {},
        };

        mpv_render_context_render(ctx.mpv_gl, params);

        fence.wait();

        ctx.queue.presentImage(ctx.swapchain, slot);
        mpv_render_context_report_swap(ctx.mpv_gl);
    }
}

int main(int argc, const char **argv) {
#ifdef __SWITCH__
    if (argc < 2)
        argc = 2, argv[1] = "/Videos/evangelion.mkv";
#endif

    std::printf("Starting player\n");

    dk::UniqueDevice dk = dk::DeviceMaker()
        .setFlags(DkDeviceFlags_OriginLowerLeft)
        .create();

    dk::UniqueQueue present_queue = dk::QueueMaker(dk)
        .setFlags(DkQueueFlags_Graphics)
        .create();

    mpv_handle *mpv = mpv_create();
    if (!mpv)
        die("context init failed");

    mpv_request_log_messages(mpv, "debug");

    mpv_set_option_string(mpv, "config", "yes");
    mpv_set_option_string(mpv, "config-dir", "/switch/AmpNX");

    if (mpv_initialize(mpv) < 0)
        die("mpv init failed");

    mpv_set_option_string(mpv, "hwdec", "auto");
    mpv_set_option_string(mpv, "hwdec-codecs", "mpeg1video,mpeg2video,mpeg4,h264,vp8,vp9");
    mpv_set_option_string(mpv, "framedrop", "decoder+vo");
    mpv_set_option_string(mpv, "vd-lavc-dr", "yes");
	mpv_set_option_string(mpv, "vd-lavc-threads", "4");
	mpv_set_option_string(mpv, "vd-lavc-skiploopfilter", "nonkey");
	mpv_set_option_string(mpv, "vd-lavc-skipframe", "nonref");
	mpv_set_option_string(mpv, "vd-lavc-framedrop", "nonref");
	mpv_set_option_string(mpv, "vd-lavc-fast", "yes");

    char *api_type = const_cast<char *>(MPV_RENDER_API_TYPE_DEKO3D);
    mpv_deko3d_init_params dk_init = {
        .device = dk,
    };
    int advanced_control = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, api_type},
        {MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS, &dk_init},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {},
    };

    mpv_render_context *mpv_gl;
    if (mpv_render_context_create(&mpv_gl, mpv, params) < 0)
        die("failed to initialize mpv GL context");

    DrawContext ctx = {
        .dk        = dk,
        .queue     = present_queue,
        .mpv_gl    = mpv_gl,
    };

    const char *cmd[] = {"loadfile", argv[1], NULL};
    mpv_command_async(mpv, 0, cmd);

    mpv_render_context_set_update_callback(mpv_gl,
        +[](void *user) {
            auto *ctx = static_cast<DrawContext *>(user);
            std::unique_lock lk(ctx->redraw_mutex);
            ctx->redraw_count++;
            ctx->redraw_condvar.notify_one();
        },
        &ctx
    );

    appletHook(&applet_hook_cookie, applet_hook_cb, &ctx);

    auto render_thread = std::jthread(render_thread_fn, std::ref(ctx));

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        mpv_event *mp_event = mpv_wait_event(mpv, 0.1);
        switch (mp_event->event_id) {
            case MPV_EVENT_LOG_MESSAGE:
                {
                    auto *msg = static_cast<mpv_event_log_message *>(mp_event->data);
                    std::printf("[%s]: %s", msg->prefix, msg->text);
                }
                break;
            case MPV_EVENT_NONE:
                break;
            default:
                std::printf("[event]: %s\n", mpv_event_name(mp_event->event_id));
                break;
        }

        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            goto done;

        if (padGetButtonsDown(&pad) & HidNpadButton_A) {
            const char *cmd[] = {"cycle", "pause", NULL};
            mpv_command_async(mpv, 0, cmd);
        }

        if (padGetButtonsDown(&pad) & HidNpadButton_Y) {
            const char *cmd[] = {"screenshot-to-file",
                "screenshot.png", "subtitles", NULL};
            std::printf("attempting to save screenshot to %s\n", cmd[1]);
            mpv_command_async(mpv, 0, cmd);
        }

        if (padGetButtonsDown(&pad) & (HidNpadButton_R | HidNpadButton_L)) {
            const char *time = (padGetButtonsDown(&pad) & HidNpadButton_R) ? "+5" : "-5";
            const char *cmd_[] = {"seek", time, "relative", NULL};
            mpv_command_async(mpv, 0, cmd_);
        }

    }

done:
    render_thread.request_stop();
    render_thread.join();

    mpv_render_context_free(mpv_gl);
    mpv_destroy(mpv);

    present_queue.waitIdle();

    std::printf("properly terminated\n");
    return 0;
}
