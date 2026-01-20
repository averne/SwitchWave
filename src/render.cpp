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
#include <vector>
#include <utility>

#include <imgui.h>
#include <imgui_deko3d.h>

#include "utils.hpp"

#include "render.hpp"

namespace sw {

using namespace std::chrono_literals;

void Renderer::applet_hook_cb(AppletHookType hook, void *param) {
    auto *self = static_cast<Renderer *>(param);
    switch (hook) {
        case AppletHookType_OnOperationMode:
            self->need_swapchain_rebuild = true;
            break;
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

    this->swapchain      = nullptr;
    this->image_memblock = nullptr;

    dk::ImageLayout fb_layout;
    dk::ImageLayoutMaker(this->dk)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(this->image_width, this->image_height)
        .setFlags(DkImageFlags_HwCompression | DkImageFlags_UsageRender |
            DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine)
        .initialize(fb_layout);

    auto fb_size = utils::align_up(fb_layout.getSize(), fb_layout.getAlignment());
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
    dk::Fence done_fence, ready_fence;

    while (true) {
        if (!this->mpv_redraw_count) {
            auto lk = std::unique_lock(this->mpv_redraw_mutex);
            this->mpv_redraw_condvar.wait(lk);
        }

        if (token.stop_requested())
            break;

        this->mpv_redraw_count--;
        if (!this->mpv_gl || !((mpv_render_context_update(this->mpv_gl) & MPV_RENDER_UPDATE_FRAME) || this->force_mpv_render))
            continue;

        this->force_mpv_render = false;

        {
            auto lk = std::scoped_lock(this->render_mtx);

            // Duplicate check in case the context was destroyed before we acquire the mutex
            if (!this->mpv_gl)
                continue;

            if (this->mpv_handle_pres && this->need_swapchain_rebuild)
                this->rebuild_swapchain();

            int slot;
            if (this->mpv_handle_pres) {
                this->swapchain.acquireImage(slot, ready_fence);
                done_fence.wait();
            } else {
                slot = (this->cur_libmpv_image.load() + 1) % Renderer::NumLibMpvImages;
            }

            mpv_deko3d_fbo fbo = {
                .tex         = this->mpv_handle_pres ? &this->swapchain_images[slot] : &this->mpv_images[slot],
                .ready_fence = this->mpv_handle_pres ? &ready_fence : &this->mpv_copy_fences[slot],
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

            // Wait for the rendering to complete before presenting
            if (this->mpv_handle_pres) {
                this->queue.waitFence(done_fence);
                this->queue.presentImage(this->swapchain, slot);
            } else {
                done_fence.wait();
                this->cur_libmpv_image = slot;
            }

            mpv_render_context_report_swap(this->mpv_gl);
        }
    }
}

int Renderer::initialize() {
    this->dk = dk::DeviceMaker()
        .setFlags(DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft)
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

    this->descriptor_memblock = dk::MemBlockMaker(this->dk,
            utils::align_up(Renderer::MaxNumDescriptors * (sizeof(dk::SamplerDescriptor) + sizeof(dk::ImageDescriptor)),
                DK_MEMBLOCK_ALIGNMENT))
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    if (!this->descriptor_memblock)
        return -1;

    this->sampler_descs = static_cast<dk::SamplerDescriptor *>(this->descriptor_memblock.getCpuAddr());
    this->image_descs   = reinterpret_cast<dk::ImageDescriptor *>(this->sampler_descs + Renderer::MaxNumDescriptors);

    dk::Fence fence;
    this->cmdbuf.bindSamplerDescriptorSet(this->descriptor_memblock.getGpuAddr(), Renderer::MaxNumDescriptors);
    this->cmdbuf.bindImageDescriptorSet(this->descriptor_memblock.getGpuAddr() + 64 * sizeof(dk::SamplerDescriptor),
        Renderer::MaxNumDescriptors);
    this->cmdbuf.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
    this->cmdbuf.signalFence(fence);
    this->queue.submitCommands(this->cmdbuf.finishList());
    this->queue.flush();

    this->rebuild_swapchain();

    appletHook(&this->applet_hook_cookie, applet_hook_cb, this);

    auto slot = this->find_descriptor_slot();
    ImGui::deko3d::init(this->dk, this->queue, this->cmdbuf,
        this->sampler_descs[slot], this->image_descs[slot],
        dkMakeTextureHandle(slot, slot), Renderer::NumSwapchainImages);

    this->mpv_render_thread = std::jthread(&Renderer::mpv_render_thread_fn, this);

    // Wait for the descriptor sets to finish uploading
    fence.wait();

    return 0;
}

Renderer::~Renderer() {
    this->mpv_render_thread.request_stop();
    this->mpv_redraw_condvar.notify_all();

    this->queue.waitIdle();

    ImGui::deko3d::exit();
}

int Renderer::create_mpv_render_context(LibmpvController &lmpv) {
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

    MPV_CALL(mpv_render_context_create(&this->mpv_gl, lmpv.get_handle(), params));

    mpv_render_context_set_update_callback(mpv_gl,
        +[](void *user) {
            auto self = static_cast<Renderer *>(user);
            auto lk = std::scoped_lock(self->mpv_redraw_mutex);
            self->mpv_redraw_count++;
            self->mpv_redraw_condvar.notify_one();
        },
        this
    );

    return 0;
}

void Renderer::destroy_mpv_render_context() {
    auto lk = std::scoped_lock(this->render_mtx);

    auto *tmp = std::exchange(this->mpv_gl, nullptr);

    this->queue.waitIdle();

    mpv_render_context_free(tmp);

    this->cur_libmpv_image = -1;
}

int Renderer::find_descriptor_slot() {
    int slot = -1;
    for (auto &pos: this->allocated_descriptors) {
        if (pos == -1ull)
            continue;
        slot = __builtin_ctzll(~pos);
        pos |= (1ull << slot);
        break;
    }
    return slot;
}

void Renderer::free_descriptor_slot(int slot) {
    this->allocated_descriptors[slot / 64] &= ~(1ull << (slot % 64));
}

Renderer::Texture Renderer::create_texture(int width, int height,
        DkImageFormat format, std::uint32_t flags) {
    auto desc_slot = this->find_descriptor_slot();
    if (desc_slot < 0) {
        std::printf("Failed to allocate descriptor slot for image\n");
        return {};
    }

    dk::ImageLayout layout;
    dk::ImageLayoutMaker(this->dk)
        .setFlags(flags)
        .setFormat(format)
        .setDimensions(width, height)
        .initialize(layout);

    dk::UniqueMemBlock out_memblock = dk::MemBlockMaker(this->dk,
            utils::align_up(layout.getSize(), std::max(layout.getAlignment(), std::uint32_t(DK_MEMBLOCK_ALIGNMENT))))
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    if (!out_memblock) {
        std::printf("Failed to allocate memblock for image\n");
        return {};
    }

    dk::Image out_image;
    out_image.initialize(layout, out_memblock, 0);

    auto out_view = dk::ImageView(out_image);
    auto sampler = dk::Sampler()
        .setFilter(DkFilter_Linear, DkFilter_Linear)
        .setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);

    this->sampler_descs[desc_slot].initialize(sampler);
    this->image_descs  [desc_slot].initialize(out_view);
    auto out_handle = dkMakeTextureHandle(desc_slot, desc_slot);

    this->cmdbuf.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
    this->queue.submitCommands(this->cmdbuf.finishList());

    return Texture(out_image, std::move(out_memblock), out_handle);
}

Renderer::Texture Renderer::load_texture(std::string_view path, int width, int height,
        DkImageFormat format, std::uint32_t flags) {
    auto texture = this->create_texture(width, height, format, flags);
    if (texture.handle == static_cast<DkResHandle>(-1))
        return {};

    auto *fp = std::fopen(path.data(), "rb");
    if (!fp) {
        std::printf("Failed to open %s\n", path.data());
        return {};
    }
    SW_SCOPEGUARD([&fp] { std::fclose(fp); });

    std::fseek(fp, 0, SEEK_END);
    auto fsize = std::ftell(fp);
    std::rewind(fp);

    dk::UniqueMemBlock transfer = dk::MemBlockMaker(this->dk, utils::align_up(fsize, DK_MEMBLOCK_ALIGNMENT))
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();

    if (std::fread(transfer.getCpuAddr(), fsize, 1, fp) != 1) {
        std::printf("Failed to read %s\n", path.data());
        return {};
    }

    this->cmdbuf.copyBufferToImage(DkCopyBuf{transfer.getGpuAddr()}, dk::ImageView(texture.image),
        DkImageRect{0, 0, 0, std::uint32_t(width), std::uint32_t(height), 1});
    this->queue.submitCommands(this->cmdbuf.finishList());
    this->queue.waitIdle();

    return Texture(texture.image, std::move(texture.memblock), texture.handle);
}

void Renderer::begin_frame() {
    if (this->need_swapchain_rebuild) {
        {
            auto lk = std::scoped_lock(this->render_mtx);
            this->rebuild_swapchain();
        }

        this->force_mpv_render = true;

        {
            auto lk = std::scoped_lock(this->mpv_redraw_mutex);
            this->mpv_redraw_count++;
            this->mpv_redraw_condvar.notify_one();
        }
    }

    this->cur_slot = this->queue.acquireImage(this->swapchain);

    this->cmdbuf.clear();
    this->cmdbuf.addMemory(this->cmdbuf_memblock, this->cur_slot * Renderer::CmdBufSize, Renderer::CmdBufSize);

    this->ui_render_fences[this->cur_slot].wait();

    auto dst_view = dk::ImageView(this->swapchain_images[this->cur_slot]);
    this->cmdbuf.bindRenderTargets(&dst_view);
    this->cmdbuf.setViewports(0, DkViewport{ 0.0f, 0.0f, static_cast<float>(this->image_width), static_cast<float>(this->image_height) });
    this->cmdbuf.setScissors(0, DkScissor{ 0, 0, this->image_width, this->image_height });
    this->cmdbuf.clearColor(0, DkColorMask_RGBA, 0, 0, 0);

    this->queue.submitCommands(this->cmdbuf.finishList());
}

void Renderer::end_frame() {
    ImGui::Render();

    auto libmpv_slot = this->cur_libmpv_image.load();
    if (libmpv_slot >= 0) {
        this->cmdbuf.copyImage(
            dk::ImageView(this->mpv_images[libmpv_slot]),          DkImageRect{ 0, 0, 0, this->image_width, this->image_height, 1 },
            dk::ImageView(this->swapchain_images[this->cur_slot]), DkImageRect{ 0, 0, 0, this->image_width, this->image_height, 1 });
        this->cmdbuf.signalFence(this->mpv_copy_fences[libmpv_slot]);
        this->queue.submitCommands(this->cmdbuf.finishList());
    }

    ImGui::deko3d::render(this->dk, this->queue, this->cmdbuf, this->cur_slot);
    this->queue.signalFence(this->ui_render_fences[this->cur_slot]);

    this->queue.presentImage(this->swapchain, this->cur_slot);
}

} // namespace sw
