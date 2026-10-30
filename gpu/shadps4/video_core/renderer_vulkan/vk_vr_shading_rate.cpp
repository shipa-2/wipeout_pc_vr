// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_vr_shading_rate.h"
#include <vk_mem_alloc.h>

namespace Vulkan::VrShadingRate {
namespace {

std::atomic<u32> eye_width{0}, eye_height{0};

struct Attachment {
    u32 width = 0, height = 0;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    vk::UniqueImageView view;
};
std::mutex attachment_mutex;
Attachment attachment;

float Setting(const char* name, float fallback) {
    const char* env = std::getenv(name);
    return env ? std::strtof(env, nullptr) : fallback;
}

/// The rates of a width x height attachment: (log2 width << 2) | log2 height of the block.
std::vector<u8> Pattern(u32 width, u32 height) {
    static const float inner = Setting("GOW3_VR_VRS_INNER", 0.55f);
    static const float outer = Setting("GOW3_VR_VRS_OUTER", 0.80f);
    std::vector<u8> rates(size_t(width) * height);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const float dx = (x + 0.5f) / width * 2.0f - 1.0f;
            const float dy = (y + 0.5f) / height * 2.0f - 1.0f;
            const float r = std::sqrt(dx * dx + dy * dy);
            rates[size_t(y) * width + x] = r < inner ? 0 : r < outer ? (1 << 2) : (1 << 2) | 1;
        }
    }
    return rates;
}

} // namespace

void NoteEyeSize(u32 width, u32 height) {
    eye_width.store(width, std::memory_order_relaxed);
    eye_height.store(height, std::memory_order_relaxed);
}

vk::ImageView AttachmentFor(const Instance& instance, Scheduler& scheduler, u32 width, u32 height,
                            u32 layers) {
    if (!instance.IsFragmentShadingRateEnabled() || layers != 1 || width < 256 ||
        width != eye_width.load(std::memory_order_relaxed) ||
        height != eye_height.load(std::memory_order_relaxed)) {
        return {};
    }
    std::scoped_lock lock{attachment_mutex};
    const vk::Extent2D texel = instance.ShadingRateTexelSize();
    const u32 w = (width + texel.width - 1) / texel.width;
    const u32 h = (height + texel.height - 1) / texel.height;
    if (attachment.view && attachment.width == w && attachment.height == h) {
        return *attachment.view;
    }
    const VmaAllocator allocator = instance.GetAllocator();
    if (attachment.image) {
        // The old one may still be read by work in flight: it stays (an eye size changes
        // once or twice a session).
        attachment.view.release();
        attachment.image = VK_NULL_HANDLE;
    }
    const VkImageCreateInfo image_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UINT,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(allocator, &image_ci, &alloc_ci, &attachment.image, &attachment.allocation,
                       nullptr) != VK_SUCCESS) {
        attachment.image = VK_NULL_HANDLE;
        return {};
    }
    const vk::Device device = instance.GetDevice();
    attachment.view = device.createImageViewUnique(vk::ImageViewCreateInfo{
                                                       .image = attachment.image,
                                                       .viewType = vk::ImageViewType::e2D,
                                                       .format = vk::Format::eR8Uint,
                                                       .subresourceRange =
                                                           {vk::ImageAspectFlagBits::eColor, 0,
                                                            1, 0, 1},
                                                   })
                          .value;
    attachment.width = w;
    attachment.height = h;

    // The pattern through a host buffer, freed once the GPU has copied it.
    const std::vector<u8> rates = Pattern(w, h);
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = rates.size(),
                                       .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    const VmaAllocationCreateInfo staging_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation buffer_allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator, &buffer_ci, &staging_ci, &buffer, &buffer_allocation, &info) !=
        VK_SUCCESS) {
        return {};
    }
    std::memcpy(info.pMappedData, rates.data(), rates.size());
    vmaFlushAllocation(allocator, buffer_allocation, 0, VK_WHOLE_SIZE);
    scheduler.Record([image = vk::Image{attachment.image}, buffer = vk::Buffer{buffer}, w,
                      h](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::ImageMemoryBarrier2 to_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});
        cmdbuf.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal,
                                 vk::BufferImageCopy{
                                     .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     .imageExtent = {w, h, 1},
                                 });
        const vk::ImageMemoryBarrier2 to_rate{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShadingRateAttachmentKHR,
            .dstAccessMask = vk::AccessFlagBits2::eFragmentShadingRateAttachmentReadKHR,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_rate});
    });
    scheduler.DeferPriorityOperation(
        [allocator, buffer, buffer_allocation] { vmaDestroyBuffer(allocator, buffer, buffer_allocation); });
    std::printf("GPU: VR eye shading rates for %ux%u (%ux%u blocks)\n", width, height, w, h);
    return *attachment.view;
}

} // namespace Vulkan::VrShadingRate
