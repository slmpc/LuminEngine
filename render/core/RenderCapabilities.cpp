#include "render/core/RenderCapabilities.hpp"

namespace lumin::render::core {

    std::string_view renderCapabilityName(RenderCapability capability) noexcept {
        switch (capability) {
        case RenderCapability::Graphics:
            return "Graphics";
        case RenderCapability::Compute:
            return "Compute";
        case RenderCapability::DynamicRendering:
            return "DynamicRendering";
        case RenderCapability::DescriptorIndexing:
            return "DescriptorIndexing";
        case RenderCapability::BufferDeviceAddress:
            return "BufferDeviceAddress";
        case RenderCapability::AccelerationStructure:
            return "AccelerationStructure";
        case RenderCapability::RayTracingPipeline:
            return "RayTracingPipeline";
        case RenderCapability::RayQuery:
            return "RayQuery";
        case RenderCapability::ShaderFloat16:
            return "ShaderFloat16";
        case RenderCapability::ShaderInt64:
            return "ShaderInt64";
        case RenderCapability::SubgroupOperations:
            return "SubgroupOperations";
        case RenderCapability::AtomicFloat32:
            return "AtomicFloat32";
        case RenderCapability::Nrd:
            return "Nrd";
        case RenderCapability::Sharc:
            return "Sharc";
        case RenderCapability::Count:
            return "Invalid";
        }
        return "Invalid";
    }

} // namespace lumin::render::core
