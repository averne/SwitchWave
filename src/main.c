#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <mpv/client.h>
#include <mpv/render_dk3d.h>

#ifdef __SWITCH__
#   include <switch.h>
#endif

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

#ifdef __SWITCH__
void userAppInit(void) {
    socketInitializeDefault();
    nxlinkStdio();
}

void userAppExit() {
    socketExit();
}
#endif

static bool redraw = false;

void on_mpv_render_update(void *user) {
    redraw = true;
}

int main(int argc, char **argv) {
#ifdef __SWITCH__
    if (argc < 2)
        argc = 2, argv[1] = "/Videos/nn.mp4";
#endif

    printf("Starting player\n");

    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);
    device_maker.flags = DkDeviceFlags_OriginLowerLeft;
    DkDevice dk = dkDeviceCreate(&device_maker);

    DkImageLayoutMaker fb_layout_maker;
    dkImageLayoutMakerDefaults(&fb_layout_maker, dk);
    fb_layout_maker.flags = DkImageFlags_UsageRender |
        DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
    fb_layout_maker.format = DkImageFormat_RGBA8_Unorm;
    fb_layout_maker.dimensions[0] = 1280;
    fb_layout_maker.dimensions[1] = 720;

    DkImageLayout fb_layout;
    dkImageLayoutInitialize(&fb_layout, &fb_layout_maker);

    size_t fb_size  = dkImageLayoutGetSize(&fb_layout);
    size_t fb_align = dkImageLayoutGetAlignment(&fb_layout);
    fb_size = (fb_size + fb_align - 1) & ~(fb_align - 1);

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, dk, 2 * fb_size);
    memblock_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    DkMemBlock fb_memblock = dkMemBlockCreate(&memblock_maker);

    DkImage const *swapchain_images[2];
    DkImage framebuffer_images[2];
    for (int i = 0; i < 2; ++i) {
        dkImageInitialize(&framebuffer_images[i], &fb_layout,
            fb_memblock, i * fb_size);
        swapchain_images[i] = &framebuffer_images[i];
    }

    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, dk);
    queue_maker.flags = DkQueueFlags_Graphics;
    DkQueue render_queue = dkQueueCreate(&queue_maker);

    DkSwapchainMaker swapchain_maker;
    dkSwapchainMakerDefaults(&swapchain_maker, dk, nwindowGetDefault(), swapchain_images, 2);
    DkSwapchain swapchain = dkSwapchainCreate(&swapchain_maker);

    mpv_handle *mpv = mpv_create();
    if (!mpv)
        die("context init failed");

    // Some minor options can only be set before mpv_initialize().
    if (mpv_initialize(mpv) < 0)
        die("mpv init failed");

    mpv_request_log_messages(mpv, "debug");

    mpv_set_option_string(mpv, "vd-lavc-dr", "yes");
    mpv_set_option_string(mpv, "hwdec", "auto");
    mpv_set_option_string(mpv, "hwdec-codecs", "mpeg1video,mpeg2video,mpeg4,h264,vp8,vp9");

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_DEKO3D},
        {MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS, &(mpv_deko3d_init_params){
            .device = dk,
        }},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &(int){1}},
        {0}
    };

    mpv_render_context *mpv_gl;
    if (mpv_render_context_create(&mpv_gl, mpv, params) < 0)
        die("failed to initialize mpv GL context");

    mpv_render_context_set_update_callback(mpv_gl, on_mpv_render_update, NULL);

    const char *cmd[] = {"loadfile", argv[1], NULL};
    mpv_command_async(mpv, 0, cmd);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        mpv_event *mp_event = mpv_wait_event(mpv, 0.01);
        switch (mp_event->event_id) {
            case MPV_EVENT_LOG_MESSAGE:
                mpv_event_log_message *msg = mp_event->data;
                while (msg) {
                    printf("[%s]: %s", msg->prefix, msg->text);
                    mp_event = mpv_wait_event(mpv, 0);
                    msg = mp_event->data;
                }
                break;
            default:
                printf("[event]: %s\n", mpv_event_name(mp_event->event_id));
                break;
            case MPV_EVENT_NONE:
                break;
        }

        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            goto done;

        if (redraw) {
            printf("Redrawing\n");

            int slot = dkQueueAcquireImage(render_queue, swapchain);

            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_DEKO3D_FBO, &(mpv_deko3d_fbo){
                    .tex = &framebuffer_images[slot],
                    .w = 1280, .h = 720,
                    .format = DkImageFormat_RGBA8_Unorm,
                }},
                {MPV_RENDER_PARAM_FLIP_Y, &(int){1}},
            };
            mpv_render_context_render(mpv_gl, params);
            redraw = false;

            dkQueuePresentImage(render_queue, swapchain, slot);
        }
    }

done:
    mpv_render_context_free(mpv_gl);
    mpv_destroy(mpv);

    dkQueueDestroy(render_queue);
    dkSwapchainDestroy(swapchain);
    dkMemBlockDestroy(fb_memblock);
    dkDeviceDestroy(dk);

    printf("properly terminated\n");
    return 0;
}
