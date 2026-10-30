// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/host_shaders/buffer_multi_copy_comp.h"
#include "gow3_toggles.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/texture_cache/texture_cache.h"
#include <boost/container/small_vector.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;
/// gow3: WipEout's render target fill (ExecuteFillShaderHLE).
static constexpr u64 FILL_SHADER_HASH = 0x900ef98d;

// gow3: the copy shader runs ~57 times per frame with ~1024 small ranges each. As one
// vkCmdCopyBuffer with that many regions it cost ~37 ns of GPU time per range (~2.1 ms per
// frame) plus the recording; buffer_multi_copy.comp copies all ranges in one dispatch.
static bool MultiCopy(Rasterizer& rasterizer, const VideoCore::Buffer* src,
                      const VideoCore::Buffer* dst, std::span<const vk::BufferCopy> copies) {
    if (copies.size() < 8 || Gow3Toggle::Disabled(Gow3Toggle::MultiCopyShader)) {
        return false;
    }
    u64 src_min = ~0ull, src_max = 0, dst_min = ~0ull, dst_max = 0;
    for (const auto& copy : copies) {
        if ((copy.srcOffset | copy.dstOffset | copy.size) & 3) {
            return false; // dword copies only
        }
        src_min = std::min(src_min, copy.srcOffset);
        src_max = std::max(src_max, copy.srcOffset + copy.size);
        dst_min = std::min(dst_min, copy.dstOffset);
        dst_max = std::max(dst_max, copy.dstOffset + copy.size);
    }
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = runtime.GetScheduler();
    const auto& instance = runtime.GetInstance();
    const u64 align = instance.StorageMinAlignment();
    src_min = Common::AlignDown(src_min, align);
    dst_min = Common::AlignDown(dst_min, align);

    struct Pipeline {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    static Pipeline pipe = [&] {
        const auto device = instance.GetDevice();
        Pipeline p;
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < 3; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        p.set_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(u32)};
        p.layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*p.set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(BUFFER_MULTI_COPY_COMP, device);
        p.pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *p.layout,
                }));
        device.destroyShaderModule(module);
        return p;
    }();

    // The region table goes into the stream buffer (host writes, visible at submission).
    thread_local std::vector<u32> regions;
    regions.resize(copies.size() * 4);
    for (size_t i = 0; i < copies.size(); ++i) {
        regions[i * 4 + 0] = static_cast<u32>((copies[i].srcOffset - src_min) / 4);
        regions[i * 4 + 1] = static_cast<u32>((copies[i].dstOffset - dst_min) / 4);
        regions[i * 4 + 2] = static_cast<u32>(copies[i].size / 4);
        regions[i * 4 + 3] = 0;
    }
    auto& stream = rasterizer.GetBufferCache().GetStreamBuffer();
    const u64 table_size = regions.size() * sizeof(u32);
    const u64 table_offset = stream.Copy(regions.data(), table_size, align);

    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xC0B1ull, [] { return std::string{"copy shader HLE: barrier + dispatch"}; });
    }
    scheduler.EndRendering();
    const u64 src_size = src_max - src_min, dst_size = dst_max - dst_min;
    if (runtime.IsBufferAccessed(src, src_min, src_size) ||
        runtime.IsBufferAccessed(dst, dst_min, dst_size, true)) {
        runtime.FlushBarriers();
    }
    const u32 count = static_cast<u32>(copies.size());
    scheduler.Record([src = src->Handle(), dst = dst->Handle(), table = stream.Handle(), src_min,
                      src_size, dst_min, dst_size, table_offset, table_size,
                      count](vk::CommandBuffer cmdbuf) {
        if (Vulkan::Scheduler::FullBarriers()) { // gow3: GOW3_FULL_BARRIERS=1
            Vulkan::Scheduler::RecordFullBarrier(cmdbuf);
        }
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {src, src_min, src_size},
            {dst, dst_min, dst_size},
            {table, table_offset, table_size},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < 3; ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipe.layout, 0, writes);
        cmdbuf.pushConstants(*pipe.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(count),
                             &count);
        cmdbuf.dispatch(count, 1, 1);
        if (Vulkan::Scheduler::FullBarriers()) {
            Vulkan::Scheduler::RecordFullBarrier(cmdbuf);
        }
    });
    runtime.AccessBuffer(src, src_min, src_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(dst, dst_min, dst_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
    return true;
}

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    // gow3: 64 KiB instead of 64 MiB. The copies are a few KiB spread over up to 57 MiB, and
    // each batch synchronizes (uploads, marks GPU-modified) its whole range: GPU time of the copy
    // shader 1.5 -> 0.6 ms/frame, GPU busy 85% -> 77%, frame rate no lower.
    // GOW3_COPY_MERGE_KB overrides it.
    static const vk::DeviceSize MaxDistanceForMerge = [] {
        const char* env = std::getenv("GOW3_COPY_MERGE_KB");
        return env ? vk::DeviceSize(std::strtoull(env, nullptr, 10)) * 1024 : vk::DeviceSize(64_KB);
    }();
    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.srcOffset;
        auto src_offset_max = copy.srcOffset + copy.size;
        auto dst_offset_min = copy.dstOffset;
        auto dst_offset_max = copy.dstOffset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // Obtain buffers for the total source and destination ranges.
        const auto [src_buf, src_buf_offset] = buffer_cache.ObtainBuffer(
            src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min, false);
        const auto [dst_buf, dst_buf_offset] = buffer_cache.ObtainBuffer(
            dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min, true);

        // Apply found buffer base.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - src_offset_min + src_buf_offset;
            copy.dstOffset = copy.dstOffset - dst_offset_min + dst_buf_offset;
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        if (!MultiCopy(rasterizer, src_buf, dst_buf, vk_copies)) {
            runtime.CopyBuffer(src_buf, dst_buf, vk_copies);
        }
        batch_start = batch_end;
    }

    return true;
}

// gow3: WipEout's fill shader (FILL_SHADER_HASH): dst[i] = src[i & cb[1]] for
// i < min(cb[0], user SGPR 6). WipEout clears its render targets this way, 8 MB a dispatch, a
// layer in a run of consecutive dispatches. With the game's memory in place the zeros went over
// PCIe and the image was uploaded again afterwards. A run of zeros over whole layers of the
// images in that memory becomes a clear of those layers: done when the run reaches a layer's
// first byte, the run's later dispatches inside it are skipped. Anything else (another pattern,
// a run starting inside a layer, memory no image has) runs the shader itself. GOW3_FILL_CLEAR=0:
// always the shader; GOW3_FILL_TRACE=1: what each fill does.
/// The clear colour of a `format` texel whose every dword is `value` (the fill's pattern), for the
/// formats whose texels are whole dwords; none for the others.
static std::optional<vk::ClearColorValue> FillColor(vk::Format format, u32 value) {
    if (value == 0) {
        return vk::ClearColorValue{}; // zero bits are zero in every format
    }
    const auto half = [](u32 h) {
        const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
        float f = exp == 0 ? std::ldexp(float(man), -24)
                  : exp == 31 ? (man ? std::numeric_limits<float>::quiet_NaN()
                                     : std::numeric_limits<float>::infinity())
                              : std::ldexp(float(man | 1024), int(exp) - 25);
        return sign ? -f : f;
    };
    const auto byte = [value](u32 i) { return float((value >> (8 * i)) & 255) / 255.0f; };
    vk::ClearColorValue color{};
    switch (format) {
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR16G16Sfloat:
        color.float32 = std::array{half(value & 0xffff), half(value >> 16), half(value & 0xffff),
                                   half(value >> 16)};
        return color;
    case vk::Format::eR32Sfloat:
    case vk::Format::eR32G32Sfloat:
    case vk::Format::eR32G32B32A32Sfloat: {
        const float f = std::bit_cast<float>(value);
        color.float32 = std::array{f, f, f, f};
        return color;
    }
    case vk::Format::eR32Uint:
    case vk::Format::eR32G32Uint:
    case vk::Format::eR32G32B32A32Uint:
        color.uint32 = std::array{value, value, value, value};
        return color;
    case vk::Format::eR8G8B8A8Unorm:
        color.float32 = std::array{byte(0), byte(1), byte(2), byte(3)};
        return color;
    case vk::Format::eB8G8R8A8Unorm:
        color.float32 = std::array{byte(2), byte(1), byte(0), byte(3)};
        return color;
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Srgb: {
        // A clear of an sRGB image takes linear values and encodes them: decoded here, so that
        // the bytes come out as the fill's (alpha is linear).
        const auto linear = [&](u32 i) {
            const float c = byte(i);
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        };
        const bool bgr = format == vk::Format::eB8G8R8A8Srgb;
        color.float32 = std::array{linear(bgr ? 2 : 0), linear(1), linear(bgr ? 0 : 2), byte(3)};
        return color;
    }
    default:
        return std::nullopt; // packed and others: not decoded here
    }
}

static bool ExecuteFillShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_FILL_CLEAR");
        return !env || env[0] != '0';
    }();
    static const bool trace = std::getenv("GOW3_FILL_TRACE") != nullptr;
    if (!enabled || info.buffers.size() < 3) {
        return false;
    }
    const auto cb_sharp = info.buffers[0].GetSharp(info);
    const auto src_sharp = info.buffers[1].GetSharp(info);
    const auto dst_sharp = info.buffers[2].GetSharp(info);
    const auto user_data = info.UserData();
    if (!cb_sharp.base_address || !src_sharp.base_address || !dst_sharp.base_address ||
        user_data.size() <= 6 || dst_sharp.GetStride() != 4 || src_sharp.GetStride() != 4) {
        return false;
    }
    const auto* cb = reinterpret_cast<const u32*>(cb_sharp.base_address);
    const u32 mask = cb[1];
    const u64 threads = u64(cs_program.dim_x) * cs_program.num_thread_x.full;
    const u64 count = std::min<u64>({cb[0], user_data[6], threads});
    if (mask >= 64 || count == 0) {
        return false;
    }
    const auto* pattern = reinterpret_cast<const u32*>(src_sharp.base_address);
    const u32 value = pattern[0];
    bool uniform = true;
    for (u32 i = 1; i <= mask; ++i) {
        uniform &= pattern[i] == value;
    }
    const VAddr begin = dst_sharp.base_address, end = begin + count * 4;

    // The run of consecutive fills, and the layers it has cleared.
    struct Cleared {
        VideoCore::ImageId id;
        u32 layer;
    };
    static VAddr run_end = 0;
    static std::vector<Cleared> cleared;
    if (begin != run_end) {
        cleared.clear();
    }
    struct Plan {
        VideoCore::Image* image;
        u32 layer;
        VideoCore::ImageId id;
        vk::ClearColorValue color;
    };
    boost::container::small_vector<Plan, 4> plan;
    bool handled = uniform;
    VAddr covered_lo = ~0ull, covered_hi = 0;
    auto& texture_cache = rasterizer.GetTextureCache();
    if (handled) {
        texture_cache.ForEachImageInRegion(begin, end - begin, [&](VideoCore::ImageId id,
                                                                    VideoCore::Image& image) {
            const auto& image_info = image.info;
            const u32 layers = image_info.resources.layers;
            if (trace) {
                std::printf("  image %#llx+%#x %ux%u fmt %d layers %u levels %u depth %d\n",
                            (unsigned long long)image_info.guest_address, image_info.guest_size,
                            image_info.size.width, image_info.size.height,
                            int(image_info.pixel_format), layers, image_info.resources.levels,
                            int(image_info.props.is_depth));
            }
            const auto color = FillColor(image_info.pixel_format, value);
            if (image_info.props.is_depth || image_info.resources.levels != 1 || layers == 0 ||
                image_info.guest_size % layers != 0 || !color) {
                handled = false;
                return;
            }
            const u64 layer_bytes = image_info.guest_size / layers;
            for (u32 layer = 0; layer < layers; ++layer) {
                const VAddr lo = image_info.guest_address + layer * layer_bytes;
                const VAddr hi = lo + layer_bytes;
                if (hi <= begin || lo >= end) {
                    continue;
                }
                covered_lo = std::min(covered_lo, std::max(lo, begin));
                covered_hi = std::max(covered_hi, std::min(hi, end));
                const bool done = std::ranges::any_of(cleared, [&](const Cleared& c) {
                    return c.id == id && c.layer == layer;
                });
                if (done) {
                    continue;
                }
                if (lo < begin) {
                    handled = false; // the run did not start at this layer's first byte
                    return;
                }
                plan.push_back({&image, layer, id, *color});
            }
        });
        // Every byte of the fill in a layer of an image.
        handled = handled && covered_lo == begin && covered_hi == end;
    }
    if (trace) {
        std::printf("Fill shader: %#llx+%#llx pattern %u dwords %s %#x, %zu layers to clear, %s\n",
                    (unsigned long long)begin, (unsigned long long)(end - begin), mask + 1,
                    uniform ? "uniform" : "mixed", value, plan.size(), handled ? "cleared" : "shader");
    }
    if (!handled) {
        run_end = 0;
        cleared.clear();
        return false;
    }
    auto& runtime = rasterizer.GetRuntime();
    for (const auto& step : plan) {
        const VideoCore::SubresourceRange range{.base = {.level = 0, .layer = step.layer},
                                                .extent = {.levels = 1, .layers = 1}};
        runtime.ClearImage(step.image, range, vk::ClearValue{step.color});
        cleared.push_back({step.id, step.layer});
    }
    run_end = end;
    return true;
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    case FILL_SHADER_HASH:
        return ExecuteFillShaderHLE(info, cs_program, rasterizer);
    default:
        return false;
    }
}

} // namespace Vulkan
