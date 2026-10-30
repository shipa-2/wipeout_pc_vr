// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: fixed foveated rendering of the VR eyes with a shading rate attachment
// (VK_KHR_fragment_shading_rate, GOW3_VR_VRS=1). A headset's lenses show the middle of each eye
// sharp and blur its edges; the title draws them all at its eye resolution. Render passes of
// the eye size get an attachment that shades the middle per pixel and the rest in 2x1 and 2x2
// blocks: the full-screen passes of WipEout's frame (lighting, post-processing) cost ~half there.
//   GOW3_VR_VRS_INNER=0.55  radius (of the eye's half extent) shaded per pixel
//   GOW3_VR_VRS_OUTER=0.80  radius from which 2x2 blocks; between them 2x1
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;

namespace VrShadingRate {

/// The eye texture size of the last headset frame (Presenter::PresentHmdFrame).
void NoteEyeSize(u32 width, u32 height);

/// The shading rate attachment view for a render pass of width x height and one layer, or a
/// null handle when the pass is not an eye's (or VRS is off). Made on first use; its contents
/// are uploaded by commands recorded on `scheduler` (outside a render pass).
vk::ImageView AttachmentFor(const Instance& instance, Scheduler& scheduler, u32 width, u32 height,
                            u32 layers);

} // namespace VrShadingRate
} // namespace Vulkan
