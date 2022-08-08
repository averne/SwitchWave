#include "imgui/imgui.h"
#include "imgui_impl_hos/imgui_deko3d.h"

#include "render.hpp"

namespace ampnx {

using namespace std::chrono_literals;

void Renderer::applet_hook_cb(AppletHookType hook, void *param) {
    auto *self = static_cast<Renderer *>(param);
    switch (hook) {
        case AppletHookType_OnOperationMode:
            self->need_swapchain_rebuild = true;
        default:
            break;
    }
}

void Renderer::rebuild_swapchain() {
    if (appletGetOperationMode() == AppletOperationMode_Console)
        this->image_width = 1920, this->image_height = 1080;
    else
        this->image_width = 1280, this->image_height = 720;

    std::printf("Rebuilding swapchain: %ux%u\n", this->image_width, this->image_height);

    this->queue.waitIdle();

    // TODO: Remove once a release of deko3d has been published with #fd315f0
    this->swapchain      = nullptr;
    this->image_memblock = nullptr;

    dk::ImageLayout fb_layout;
    dk::ImageLayoutMaker(this->dk)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(this->image_width, this->image_height)
        .setFlags(DkImageFlags_HwCompression | DkImageFlags_UsageRender |
            DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine)
        .initialize(fb_layout);

    std::size_t fb_size  = fb_layout.getSize();
    std::size_t fb_align = fb_layout.getAlignment();
    fb_size = (fb_size + fb_align - 1) & ~(fb_align - 1);

    this->image_memblock = dk::MemBlockMaker(this->dk, (Renderer::NumSwapchainImages + Renderer::NumLibMpvImages) * fb_size)
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();

    std::array<const DkImage *, Renderer::NumSwapchainImages> swapchain_images;
    for (int i = 0; i < Renderer::NumSwapchainImages; ++i) {
        this->swapchain_images[i].initialize(fb_layout, this->image_memblock, i * fb_size);
        swapchain_images[i] = &this->swapchain_images[i];
    }

    this->swapchain = dk::SwapchainMaker(this->dk, nwindowGetDefault(), swapchain_images)
        .create();

    for (int i = 0; i < Renderer::NumLibMpvImages; ++i)
        this->mpv_images[i].initialize(fb_layout, this->image_memblock, (i + Renderer::NumSwapchainImages) * fb_size);

    this->need_swapchain_rebuild = false;
    this->cur_libmpv_image       = -1;
}

void Renderer::mpv_render_thread_fn(std::stop_token token) {
    while (!token.stop_requested()) {
        if (!this->mpv_redraw_count) {
            std::unique_lock lk(this->mpv_redraw_mutex);
            if (this->mpv_redraw_condvar.wait_for(lk, 100ms) == std::cv_status::timeout)
                continue;
        }

        this->mpv_redraw_count--;
        if (!(mpv_render_context_update(this->mpv_gl) & MPV_RENDER_UPDATE_FRAME))
            continue;

        std::scoped_lock lk(this->swapchain_rebuild_mtx);

        auto next_slot = (this->cur_libmpv_image.load() + 1) % Renderer::NumLibMpvImages;

        dk::Fence done_fence;
        mpv_deko3d_fbo fbo = {
            .tex         = &this->mpv_images[next_slot],
            .ready_fence = &this->mpv_copy_fences[next_slot],
            .done_fence  = &done_fence,
            .w           = static_cast<int>(this->image_width),
            .h           = static_cast<int>(this->image_height),
            .format      = DkImageFormat_RGBA8_Unorm,
        };

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_DEKO3D_FBO, &fbo},
            {},
        };

        mpv_render_context_render(this->mpv_gl, params);

        // Wait for the rendering to complete
        done_fence.wait();

        this->cur_libmpv_image = next_slot;
    }
}

int Renderer::initialize(LibmpvController &mpv) {
    this->dk = dk::DeviceMaker()
        .setFlags(DkDeviceFlags_OriginUpperLeft)
        .create();
    if (!this->dk)
        return -1;

    this->queue = dk::QueueMaker(this->dk)
        // Give this queue a high priority to help render the ui smoothly even if libmpv is hogging the gpu
        .setFlags(DkQueueFlags_Graphics | DkQueueFlags_DisableZcull | DkQueueFlags_HighPrio)
        .create();
    if (!this->queue)
        return -1;

    this->cmdbuf_memblock = dk::MemBlockMaker(this->dk, Renderer::NumSwapchainImages * Renderer::CmdBufSize)
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    if (!this->cmdbuf_memblock)
        return -1;

    this->cmdbuf = dk::CmdBufMaker(this->dk)
        .create();
    if (!this->cmdbuf)
        return -1;

    cmdbuf.addMemory(this->cmdbuf_memblock, 0, Renderer::CmdBufSize);

    this->descriptor_memblock = dk::MemBlockMaker(this->dk, 0x1000)
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    if (!this->descriptor_memblock)
        return -1;

    this->sampler_descs = static_cast<dk::SamplerDescriptor *>(this->descriptor_memblock.getCpuAddr());
    this->image_descs   = reinterpret_cast<dk::ImageDescriptor *>(this->sampler_descs + 64);

	this->cmdbuf.bindSamplerDescriptorSet(this->descriptor_memblock.getGpuAddr(), 1);
	this->cmdbuf.bindImageDescriptorSet(this->descriptor_memblock.getGpuAddr() + 64 * sizeof(dk::SamplerDescriptor), 1);
    this->cmdbuf.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
    this->queue.submitCommands(this->cmdbuf.finishList());
    this->queue.waitIdle();

    mpv_deko3d_init_params dk_init = {
        .device = this->dk,
    };

    int advanced_control = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_DEKO3D)},
        {MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS, &dk_init},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {},
    };

    MPV_CALL(mpv_render_context_create(&this->mpv_gl, mpv.get_handle(), params));

    mpv_render_context_set_update_callback(mpv_gl,
        +[](void *user) {
            auto self = static_cast<Renderer *>(user);
            std::unique_lock lk(self->mpv_redraw_mutex);
            self->mpv_redraw_count++;
            self->mpv_redraw_condvar.notify_one();
        },
        this
    );

    this->rebuild_swapchain();

    appletHook(&this->applet_hook_cookie, applet_hook_cb, this);

    imgui::deko3d::init(this->dk, this->queue, this->cmdbuf,
        this->sampler_descs[0], this->image_descs[0],
        dkMakeTextureHandle(0, 0), Renderer::NumSwapchainImages);

    this->mpv_render_thread = std::jthread(&Renderer::mpv_render_thread_fn, this);

    return 0;
}

Renderer::~Renderer() {
    // Terminate render thread before destroying the mpv graphics context
    this->mpv_render_thread.request_stop();
    this->mpv_redraw_condvar.notify_all();
    this->mpv_render_thread.join();

    this->queue.waitIdle();
    this->swapchain.destroy();
    this->queue.destroy();

    imgui::deko3d::exit();

    mpv_render_context_free(this->mpv_gl);
}

void Renderer::begin_frame() {
    if (this->need_swapchain_rebuild) {
        std::scoped_lock lk(this->swapchain_rebuild_mtx);
        this->rebuild_swapchain();
    }

    ImGui::NewFrame();
}

void Renderer::end_frame() {
    ImGui::Render();

    int slot = this->queue.acquireImage(this->swapchain);

    this->cmdbuf.clear();
    this->cmdbuf.addMemory(this->cmdbuf_memblock, slot * Renderer::CmdBufSize, Renderer::CmdBufSize);

    auto dst_view = dk::ImageView(this->swapchain_images[slot]);
    this->cmdbuf.bindRenderTargets(&dst_view);
    this->cmdbuf.setScissors(0, DkScissor{0, 0, this->image_width, this->image_height});

    int libmpv_slot = this->cur_libmpv_image.load();
    if (libmpv_slot >= 0) {
        auto src_view = dk::ImageView(this->mpv_images[libmpv_slot]);
        this->cmdbuf.blitImage(src_view, DkImageRect{0, 0, 0, this->image_width, this->image_height, 1},
            dst_view, DkImageRect{0, 0, 0, this->image_width, this->image_height, 1}, DkBlitFlag_ModeBlit, 0);
        this->cmdbuf.signalFence(this->mpv_copy_fences[libmpv_slot]);
    } else {
        this->cmdbuf.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f);
    }

    this->queue.submitCommands(this->cmdbuf.finishList());

    imgui::deko3d::render(this->dk, this->queue, this->cmdbuf, slot);

    this->queue.presentImage(this->swapchain, slot);
    mpv_render_context_report_swap(this->mpv_gl);
}

} // namespace ampnx
