// Copyright 2026 VIMA contributors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

namespace gfxstream::host::vk {

enum VimaCapabilityExposureBit : uint64_t {
    kVimaExposeGeometryShader = 1ull << 0,
    kVimaExposeCustomBorderColor = 1ull << 1,
    kVimaExposeBorderColorSwizzle = 1ull << 2,
    kVimaExposeTransformFeedback = 1ull << 3,
    kVimaExposePrimitivesGeneratedQuery = 1ull << 4,
    kVimaExposeAllCapabilities = (1ull << 5) - 1,
};

inline bool vimaCapabilityExposureAllowed(uint64_t mask, uint64_t bit) {
    return (mask & bit) != 0;
}

inline uint64_t vimaExtensionCapabilityBit(const char* name) {
    if (strcmp(name, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME) == 0) {
        return kVimaExposeCustomBorderColor;
    }
    if (strcmp(name, VK_EXT_BORDER_COLOR_SWIZZLE_EXTENSION_NAME) == 0) {
        return kVimaExposeBorderColorSwizzle;
    }
    if (strcmp(name, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) == 0) {
        return kVimaExposeTransformFeedback;
    }
    if (strcmp(name, VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME) == 0) {
        return kVimaExposePrimitivesGeneratedQuery;
    }
    return 0;
}

inline void filterVimaDeviceExtensions(std::vector<VkExtensionProperties>* extensions,
                                       uint64_t allowedMask) {
    extensions->erase(
        std::remove_if(extensions->begin(), extensions->end(), [&](const VkExtensionProperties& ext) {
            const uint64_t bit = vimaExtensionCapabilityBit(ext.extensionName);
            return bit != 0 && !vimaCapabilityExposureAllowed(allowedMask, bit);
        }),
        extensions->end());
}

inline void filterVimaPhysicalDeviceFeatures(VkPhysicalDeviceFeatures2* features,
                                             uint64_t allowedMask) {
    if (!vimaCapabilityExposureAllowed(allowedMask, kVimaExposeGeometryShader)) {
        features->features.geometryShader = VK_FALSE;
    }

    auto* current = reinterpret_cast<VkBaseOutStructure*>(features->pNext);
    while (current != nullptr) {
        switch (current->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
                if (!vimaCapabilityExposureAllowed(allowedMask, kVimaExposeCustomBorderColor)) {
                    auto* value = reinterpret_cast<VkPhysicalDeviceCustomBorderColorFeaturesEXT*>(current);
                    value->customBorderColors = VK_FALSE;
                    value->customBorderColorWithoutFormat = VK_FALSE;
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT: {
                if (!vimaCapabilityExposureAllowed(allowedMask, kVimaExposeBorderColorSwizzle)) {
                    auto* value = reinterpret_cast<VkPhysicalDeviceBorderColorSwizzleFeaturesEXT*>(current);
                    value->borderColorSwizzle = VK_FALSE;
                    value->borderColorSwizzleFromImage = VK_FALSE;
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
                if (!vimaCapabilityExposureAllowed(allowedMask, kVimaExposeTransformFeedback)) {
                    auto* value = reinterpret_cast<VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(current);
                    value->transformFeedback = VK_FALSE;
                    value->geometryStreams = VK_FALSE;
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT: {
                if (!vimaCapabilityExposureAllowed(allowedMask,
                                                   kVimaExposePrimitivesGeneratedQuery)) {
                    auto* value = reinterpret_cast<VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT*>(current);
                    value->primitivesGeneratedQuery = VK_FALSE;
                    value->primitivesGeneratedQueryWithRasterizerDiscard = VK_FALSE;
                    value->primitivesGeneratedQueryWithNonZeroStreams = VK_FALSE;
                }
                break;
            }
            default:
                break;
        }
        current = current->pNext;
    }
}

}  // namespace gfxstream::host::vk
