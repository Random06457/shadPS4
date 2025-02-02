// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include "common/assert.h"
#include "common/debug.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance)
    : instance{instance}, master_semaphore{instance}, command_pool{instance, &master_semaphore} {
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
    AllocateWorkerCommandBuffers();
}

Scheduler::~Scheduler() {
    std::free(profiler_scope);
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = 1,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = render_state.color_attachments.data(),
        .pDepthAttachment = render_state.has_depth ? &render_state.depth_attachment : nullptr,
        .pStencilAttachment = render_state.has_stencil ? &render_state.depth_attachment : nullptr,
    };

    current_cmdbuf.beginRendering(rendering_info);
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    current_cmdbuf.endRendering();

    boost::container::static_vector<vk::ImageMemoryBarrier, 9> barriers;
    for (size_t i = 0; i < render_state.num_color_attachments; ++i) {
        barriers.push_back(vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = render_state.color_images[i],
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
        });
    }
    if (render_state.has_depth) {
        barriers.push_back(vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
            .oldLayout = render_state.depth_attachment.imageLayout,
            .newLayout = render_state.depth_attachment.imageLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = render_state.depth_image,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eDepth |
                                  (render_state.has_stencil ? vk::ImageAspectFlagBits::eStencil
                                                            : vk::ImageAspectFlagBits::eNone),
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
        });
    }

    if (!barriers.empty()) {
        const auto src_stages =
            vk::PipelineStageFlagBits::eColorAttachmentOutput |
            (render_state.has_depth ? vk::PipelineStageFlagBits::eLateFragmentTests |
                                          vk::PipelineStageFlagBits::eEarlyFragmentTests
                                    : vk::PipelineStageFlagBits::eNone);
        current_cmdbuf.pipelineBarrier(src_stages, vk::PipelineStageFlagBits::eFragmentShader,
                                       vk::DependencyFlagBits::eByRegion, {}, {}, barriers);
    }
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::Wait(u64 tick) {
    if (tick >= master_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    master_semaphore.Wait(tick);
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    current_cmdbuf.begin(begin_info);

    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPallete::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = master_semaphore.NextTick();

    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }

    EndRendering();
    current_cmdbuf.end();

    const vk::Semaphore timeline = master_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = static_cast<u32>(info.wait_ticks.size()),
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = static_cast<u32>(info.signal_ticks.size()),
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = static_cast<u32>(info.wait_semas.size()),
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = static_cast<u32>(info.signal_semas.size()),
        .pSignalSemaphores = info.signal_semas.data(),
    };

    try {
        instance.GetGraphicsQueue().submit(submit_info, info.fence);
    } catch (vk::DeviceLostError& err) {
        if (instance.HasNvCheckpoints()) {
            const auto checkpoint_data = instance.GetGraphicsQueue().getCheckpointData2NV();
            for (const auto& cp : checkpoint_data) {
                LOG_CRITICAL(Render_Vulkan, "{}: {:#x}", vk::to_string(cp.stage),
                             reinterpret_cast<u64>(cp.pCheckpointMarker));
            }
        }
        UNREACHABLE_MSG("Device lost during submit: {}", err.what());
    }

    master_semaphore.Refresh();
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    while (!pending_ops.empty() && IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
    }
}

} // namespace Vulkan
