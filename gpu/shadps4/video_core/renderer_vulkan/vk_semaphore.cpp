// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>
#include <fmt/format.h>
#include <limits>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

namespace Vulkan {

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
    std::scoped_lock lk{registry_mutex};
    registry.push_back(this);
}

Semaphore::~Semaphore() {
    std::scoped_lock lk{registry_mutex};
    std::erase(registry, this);
}

/// bbport (sotc): every timeline semaphore, for stall reports (a submission waiting on one
/// that never signals blocks the queue for all that follow).
std::mutex Semaphore::registry_mutex;
std::vector<Semaphore*> Semaphore::registry;

namespace {
std::mutex shaders_mutex;
std::unordered_map<u64, u32> open_shaders;
std::deque<std::pair<u64, std::vector<std::pair<u64, u32>>>> closed_shaders;
} // namespace

void Semaphore::NoteShader(u64 hash) {
    std::scoped_lock lk{shaders_mutex};
    ++open_shaders[hash];
}

void Semaphore::CloseSubmission(u64 tick) {
    std::scoped_lock lk{shaders_mutex};
    closed_shaders.emplace_back(tick, std::vector<std::pair<u64, u32>>(open_shaders.begin(),
                                                                       open_shaders.end()));
    open_shaders.clear();
    while (closed_shaders.size() > 16) {
        closed_shaders.pop_front();
    }
}

void Semaphore::ReportShaders(u64 tick) {
    std::scoped_lock lk{shaders_mutex};
    for (const auto& [t, shaders] : closed_shaders) {
        if (t != tick) {
            continue;
        }
        std::string line = fmt::format("  submission {} ran {} programs:", t, shaders.size());
        for (const auto& [hash, count] : shaders) {
            line += fmt::format(" {:08x}x{}", hash, count);
        }
        std::puts(line.c_str());
    }
}

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    // bbport (sotc): a wait that lasts seconds is reported (hang diagnosis).
    u32 stalls = 0;
    vk::Result result;
    while ((result = instance.GetDevice().waitSemaphores(&wait_info, 3'000'000'000ull)) !=
           vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            instance.ReportDeviceLost();
            ASSERT_MSG(false, "Device lost while waiting for tick {}", tick);
        }
        auto [res, counter] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        std::printf("Semaphore stall #%u: waiting tick %llu, gpu counter %llu, next %llu\n",
                    ++stalls, (unsigned long long)tick, (unsigned long long)counter,
                    (unsigned long long)CurrentTick());
        if (stalls == 1) {
            ReportShaders(tick);
            std::scoped_lock lk{registry_mutex};
            for (Semaphore* s : registry) {
                auto [r, value] = instance.GetDevice().getSemaphoreCounterValue(s->Handle());
                std::printf("  timeline semaphore %p: gpu %llu, last asked %llu%s\n", (void*)s,
                            (unsigned long long)value, (unsigned long long)(s->CurrentTick() - 1),
                            value + 1 < s->CurrentTick() ? "  <- behind" : "");
            }
        }
        std::fflush(stdout);
    }
    Refresh();
}

} // namespace Vulkan
