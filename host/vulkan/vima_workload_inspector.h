#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace gfxstream {
namespace host {
namespace vk {

enum VimaInspectorPipelineStageBits : uint8_t {
    kVimaInspectorGeometry = 1u << 0,
    kVimaInspectorTessControl = 1u << 1,
    kVimaInspectorTessEvaluation = 1u << 2,
};

class VimaWorkloadInspector {
   public:
    static VimaWorkloadInspector& get();

    void setDecoderContext(const char* processName, uint64_t puid);
    void setHostIdentity(const std::string& name, uint32_t vendorId, uint32_t deviceId,
                         const std::string& driverInfo);
    void instanceCreated(VkInstance instance, const std::string& application,
                         const std::string& engine, bool angle);
    void instanceDestroyed(VkInstance instance);
    void probeQuery(VkInstance instance, const std::string& api, const std::string& result);
    void geometryShaderFeatureQuery(VkInstance instance, bool advertised);
    void capabilityExtensionQuery(VkInstance instance, const std::string& extension, bool advertised);
    void capabilityFeatureQuery(VkInstance instance, const std::string& feature, bool advertised);
    void deviceCreated(VkDevice device, VkInstance instance);
    void deviceDestroyed(VkDevice device);
    void pipelineCreated(VkDevice device, uint8_t stageBits);
    void queueSubmitted(VkDevice device, bool graphicsDraw, uint8_t stageBits);
    void vulkanErrorForDevice(VkDevice device, const std::string& operation, VkResult result);
    void vulkanErrorForCurrentProcess(const std::string& operation, VkResult result);
    void decoderError(const std::string& message);
    void deviceLost();
    std::string snapshot();

   private:
    VimaWorkloadInspector();
    struct Impl;
    Impl* mImpl;
};

}  // namespace vk
}  // namespace host
}  // namespace gfxstream

extern "C" __attribute__((visibility("default"))) size_t
gfxstream_vima_inspector_snapshot(char* buffer, size_t capacity);
