#include "vima_workload_inspector.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gfxstream {
namespace host {
namespace vk {
namespace {

thread_local std::string sProcessName;
thread_local uint64_t sPuid = 0;

uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string json(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c)
                        << std::dec;
                } else {
                    out << c;
                }
        }
    }
    out << '"';
    return out.str();
}

template <typename T>
std::string handleString(T handle) {
    std::ostringstream out;
    out << "0x" << std::hex << reinterpret_cast<uintptr_t>(handle);
    return out.str();
}

}  // namespace

struct VimaWorkloadInspector::Impl {
    struct Instance {
        VkInstance handle = VK_NULL_HANDLE;
        uint64_t puid = 0;
        std::string process;
        std::string application;
        std::string engine;
        bool angle = false;
        uint64_t createdMs = 0;
        uint64_t createdNs = 0;
        uint32_t probeSequence = 0;
        uint64_t destroyedMs = 0;
        uint32_t devices = 0;
        uint64_t submissions = 0;
        uint64_t graphicsSubmissions = 0;
        bool geometryShaderQueried = false;
        bool geometryShaderAdvertised = false;
        std::unordered_map<std::string, bool> capabilityExtensions;
        std::unordered_map<std::string, bool> capabilityFeatures;
    };
    struct Device {
        VkDevice handle = VK_NULL_HANDLE;
        size_t instance = 0;
        uint64_t createdMs = 0;
        uint64_t destroyedMs = 0;
        uint64_t submissions = 0;
        uint64_t graphicsSubmissions = 0;
        uint32_t geometryCreated = 0;
        uint32_t tessControlCreated = 0;
        uint32_t tessEvaluationCreated = 0;
        uint8_t submittedStages = 0;
    };

    std::mutex mutex;
    std::string package;
    std::unordered_map<uint64_t, std::string> processByPuid;
    std::unordered_set<std::string> processes;
    std::vector<Instance> instances;
    std::vector<Device> devices;
    std::unordered_map<VkInstance, size_t> liveInstances;
    std::unordered_map<VkDevice, size_t> liveDevices;
    std::vector<std::string> vulkanErrors;
    std::vector<std::string> decoderErrors;
    bool lost = false;
    std::string hostName;
    std::string driverInfo;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;

    bool matchesLocked() {
        if (package.empty()) return false;
        std::string process = sProcessName;
        if (!process.empty()) processByPuid[sPuid] = process;
        if (process.empty()) {
            auto it = processByPuid.find(sPuid);
            if (it != processByPuid.end()) process = it->second;
        }
        bool match = process == package ||
                     (process.size() > package.size() &&
                      process.compare(0, package.size(), package) == 0 &&
                      process[package.size()] == ':');
        // The virtio context name used before render-control metadata arrives is
        // `<process>-<pid>`. Accept that transport suffix, but no arbitrary prefix match.
        if (!match && process.size() > package.size() + 1 &&
            process.compare(0, package.size(), package) == 0 &&
            process[package.size()] == '-') {
            match = process.find_first_not_of("0123456789", package.size() + 1) ==
                    std::string::npos;
        }
        if (match) processes.insert(process);
        return match;
    }

    void addError(std::vector<std::string>& target, std::string message) {
        if (target.size() < 100) target.emplace_back(std::move(message));
    }
};

VimaWorkloadInspector::VimaWorkloadInspector() : mImpl(new Impl()) {
    if (const char* package = std::getenv("VIMA_INSPECT_PACKAGE")) mImpl->package = package;
}

VimaWorkloadInspector& VimaWorkloadInspector::get() {
    static VimaWorkloadInspector instance;
    return instance;
}

void VimaWorkloadInspector::setDecoderContext(const char* processName, uint64_t puid) {
    sProcessName = processName ? processName : "";
    sPuid = puid;
    if (sProcessName.empty()) return;
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    mImpl->processByPuid[puid] = sProcessName;
}

void VimaWorkloadInspector::setHostIdentity(const std::string& name, uint32_t vendorId,
                                            uint32_t deviceId,
                                            const std::string& driverInfo) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    mImpl->hostName = name;
    mImpl->vendorId = vendorId;
    mImpl->deviceId = deviceId;
    mImpl->driverInfo = driverInfo;
}

void VimaWorkloadInspector::instanceCreated(VkInstance instance, const std::string& application,
                                            const std::string& engine, bool angle) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    if (!mImpl->matchesLocked()) {
        if (!mImpl->package.empty() && application == mImpl->package) {
            std::fprintf(stderr,
                         "VIMA inspector: applicationName matched but process metadata did not "
                         "(process=%s puid=%llu); instance excluded\n",
                         sProcessName.c_str(), static_cast<unsigned long long>(sPuid));
        }
        return;
    }
    Impl::Instance record;
    record.handle = instance;
    record.puid = sPuid;
    record.process = sProcessName.empty() ? mImpl->processByPuid[sPuid] : sProcessName;
    record.application = application;
    record.engine = engine;
    record.angle = angle;
    record.createdMs = nowMs();
    record.createdNs = nowNs();
    mImpl->instances.push_back(std::move(record));
    mImpl->liveInstances[instance] = mImpl->instances.size() - 1;
}

void VimaWorkloadInspector::probeQuery(VkInstance instance, const std::string& api,
                                       const std::string& result) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    auto live = mImpl->liveInstances.find(instance);
    if (live == mImpl->liveInstances.end()) return;
    auto& record = mImpl->instances[live->second];
    if (record.angle || record.application != "FortniteGame" ||
        record.engine != "UnrealEngine6.0") return;
    const uint64_t elapsedUs = (nowNs() - record.createdNs) / 1000;
    std::fprintf(stderr, "VIMA Vulkan probe +%llu us #%u %s %s\n",
                 static_cast<unsigned long long>(elapsedUs), ++record.probeSequence,
                 api.c_str(), result.c_str());
}

void VimaWorkloadInspector::geometryShaderFeatureQuery(VkInstance instance, bool advertised) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    auto live = mImpl->liveInstances.find(instance);
    if (live == mImpl->liveInstances.end()) return;
    auto& record = mImpl->instances[live->second];
    record.geometryShaderQueried = true;
    record.geometryShaderAdvertised = advertised;
}

void VimaWorkloadInspector::capabilityExtensionQuery(VkInstance instance,
                                                       const std::string& extension,
                                                       bool advertised) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    auto live = mImpl->liveInstances.find(instance);
    if (live == mImpl->liveInstances.end()) return;
    mImpl->instances[live->second].capabilityExtensions[extension] = advertised;
}

void VimaWorkloadInspector::capabilityFeatureQuery(VkInstance instance,
                                                    const std::string& feature,
                                                    bool advertised) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    auto live = mImpl->liveInstances.find(instance);
    if (live == mImpl->liveInstances.end()) return;
    mImpl->instances[live->second].capabilityFeatures[feature] = advertised;
}

void VimaWorkloadInspector::instanceDestroyed(VkInstance instance) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    auto it = mImpl->liveInstances.find(instance);
    if (it == mImpl->liveInstances.end()) return;
    mImpl->instances[it->second].destroyedMs = nowMs();
    mImpl->liveInstances.erase(it);
}

void VimaWorkloadInspector::deviceCreated(VkDevice device, VkInstance instance) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    auto owner = mImpl->liveInstances.find(instance);
    if (owner == mImpl->liveInstances.end()) return;
    Impl::Device record;
    record.handle = device;
    record.instance = owner->second;
    record.createdMs = nowMs();
    mImpl->devices.push_back(record);
    mImpl->liveDevices[device] = mImpl->devices.size() - 1;
    mImpl->instances[owner->second].devices++;
}

void VimaWorkloadInspector::deviceDestroyed(VkDevice device) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    auto it = mImpl->liveDevices.find(device);
    if (it == mImpl->liveDevices.end()) return;
    mImpl->devices[it->second].destroyedMs = nowMs();
    mImpl->liveDevices.erase(it);
}

void VimaWorkloadInspector::pipelineCreated(VkDevice device, uint8_t bits) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    auto it = mImpl->liveDevices.find(device);
    if (it == mImpl->liveDevices.end()) return;
    auto& d = mImpl->devices[it->second];
    if (bits & kVimaInspectorGeometry) d.geometryCreated++;
    if (bits & kVimaInspectorTessControl) d.tessControlCreated++;
    if (bits & kVimaInspectorTessEvaluation) d.tessEvaluationCreated++;
}

void VimaWorkloadInspector::queueSubmitted(VkDevice device, bool graphicsDraw, uint8_t bits) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    auto it = mImpl->liveDevices.find(device);
    if (it == mImpl->liveDevices.end()) return;
    auto& d = mImpl->devices[it->second];
    auto& instance = mImpl->instances[d.instance];
    d.submissions++;
    instance.submissions++;
    if (graphicsDraw) {
        d.graphicsSubmissions++;
        instance.graphicsSubmissions++;
    }
    d.submittedStages |= bits;
}

void VimaWorkloadInspector::vulkanErrorForDevice(VkDevice device,
                                                  const std::string& operation,
                                                  VkResult result) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    if (mImpl->liveDevices.find(device) == mImpl->liveDevices.end()) return;
    mImpl->addError(mImpl->vulkanErrors,
                    operation + " returned " + std::to_string(static_cast<int>(result)));
}

void VimaWorkloadInspector::vulkanErrorForCurrentProcess(const std::string& operation,
                                                          VkResult result) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    if (!mImpl->matchesLocked()) return;
    mImpl->addError(mImpl->vulkanErrors,
                    operation + " returned " + std::to_string(static_cast<int>(result)));
}

void VimaWorkloadInspector::decoderError(const std::string& message) {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    if (!mImpl->matchesLocked()) return;
    mImpl->addError(mImpl->decoderErrors, message);
}

void VimaWorkloadInspector::deviceLost() {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    if (mImpl->matchesLocked()) mImpl->lost = true;
}

std::string VimaWorkloadInspector::snapshot() {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    std::ostringstream out;
    out << "{\"package\":" << json(mImpl->package) << ",\"processes\":[";
    bool first = true;
    for (const auto& process : mImpl->processes) {
        if (!first) out << ',';
        first = false;
        out << json(process);
    }
    out << "],\"instances\":[";
    first = true;
    for (const auto& i : mImpl->instances) {
        if (!first) out << ',';
        first = false;
        const char* status = i.graphicsSubmissions ? "ACTIVE" : (i.devices ? "DEVICE_CREATED" : "PROBED");
        out << "{\"handle\":" << json(handleString(i.handle))
            << ",\"puid\":" << i.puid << ",\"process\":" << json(i.process)
            << ",\"applicationName\":" << json(i.application)
            << ",\"engineName\":" << json(i.engine)
            << ",\"angle\":" << (i.angle ? "true" : "false")
            << ",\"createdMs\":" << i.createdMs << ",\"destroyedMs\":" << i.destroyedMs
            << ",\"deviceCount\":" << i.devices << ",\"queueSubmissions\":" << i.submissions
            << ",\"graphicsSubmissions\":" << i.graphicsSubmissions
            << ",\"geometryShaderQueried\":" << (i.geometryShaderQueried ? "true" : "false")
            << ",\"geometryShaderAdvertised\":" << (i.geometryShaderAdvertised ? "true" : "false")
            << ",\"capabilityExtensions\":{";
        bool firstCapability = true;
        for (const auto& [name, value] : i.capabilityExtensions) {
            if (!firstCapability) out << ',';
            firstCapability = false;
            out << json(name) << ':' << (value ? "true" : "false");
        }
        out << "},\"capabilityFeatures\":{";
        firstCapability = true;
        for (const auto& [name, value] : i.capabilityFeatures) {
            if (!firstCapability) out << ',';
            firstCapability = false;
            out << json(name) << ':' << (value ? "true" : "false");
        }
        out << "},\"status\":" << json(status) << '}';
    }
    out << "],\"devices\":[";
    first = true;
    for (const auto& d : mImpl->devices) {
        if (!first) out << ',';
        first = false;
        out << "{\"handle\":" << json(handleString(d.handle))
            << ",\"instanceHandle\":" << json(handleString(mImpl->instances[d.instance].handle))
            << ",\"createdMs\":" << d.createdMs << ",\"destroyedMs\":" << d.destroyedMs
            << ",\"queueSubmissions\":" << d.submissions
            << ",\"graphicsSubmissions\":" << d.graphicsSubmissions
            << ",\"geometryCreated\":" << d.geometryCreated
            << ",\"tessControlCreated\":" << d.tessControlCreated
            << ",\"tessEvaluationCreated\":" << d.tessEvaluationCreated
            << ",\"geometrySubmitted\":" << ((d.submittedStages & kVimaInspectorGeometry) ? "true" : "false")
            << ",\"tessControlSubmitted\":" << ((d.submittedStages & kVimaInspectorTessControl) ? "true" : "false")
            << ",\"tessEvaluationSubmitted\":" << ((d.submittedStages & kVimaInspectorTessEvaluation) ? "true" : "false") << '}';
    }
    auto emitErrors = [&](const std::vector<std::string>& errors) {
        out << '[';
        bool firstError = true;
        for (const auto& error : errors) {
            if (!firstError) out << ',';
            firstError = false;
            out << json(error);
        }
        out << ']';
    };
    out << "],\"errors\":{\"vulkan\":";
    emitErrors(mImpl->vulkanErrors);
    out << ",\"decoder\":";
    emitErrors(mImpl->decoderErrors);
    out << ",\"deviceLost\":" << (mImpl->lost ? "true" : "false") << "},\"hostIdentity\":{"
        << "\"deviceName\":" << json(mImpl->hostName) << ",\"vendorID\":" << mImpl->vendorId
        << ",\"deviceID\":" << mImpl->deviceId << ",\"driverInfo\":" << json(mImpl->driverInfo)
        << "}}";
    return out.str();
}

}  // namespace vk
}  // namespace host
}  // namespace gfxstream

extern "C" size_t gfxstream_vima_inspector_snapshot(char* buffer, size_t capacity) {
    const std::string value =
        gfxstream::host::vk::VimaWorkloadInspector::get().snapshot();
    const size_t required = value.size() + 1;
    if (buffer && capacity) {
        const size_t copied = capacity < required ? capacity - 1 : value.size();
        std::memcpy(buffer, value.data(), copied);
        buffer[copied] = 0;
    }
    return required;
}
