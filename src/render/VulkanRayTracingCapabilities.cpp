#include "render/VulkanRayTracingCapabilities.hpp"

#include <algorithm>
#include <stdexcept>

namespace lumin::render {
    namespace {

        constexpr std::string_view accelerationStructureExtension = "VK_KHR_acceleration_structure";
        constexpr std::string_view rayTracingPipelineExtension = "VK_KHR_ray_tracing_pipeline";
        constexpr std::string_view deferredHostOperationsExtension = "VK_KHR_deferred_host_operations";
        constexpr std::string_view rayQueryExtension = "VK_KHR_ray_query";
        constexpr std::string_view computeShaderDerivativesExtension = "VK_KHR_compute_shader_derivatives";

        [[nodiscard]] bool containsExtension(std::span<const std::string_view> extensions,
                                             std::string_view required) noexcept {
            return std::find(extensions.begin(), extensions.end(), required) != extensions.end();
        }

        [[nodiscard]] bool validMode(RayTracingMode mode) noexcept {
            switch (mode) {
            case RayTracingMode::Auto:
            case RayTracingMode::On:
            case RayTracingMode::Off:
                return true;
            }
            return false;
        }

        [[nodiscard]] RayTracingDiagnostic diagnosticFor(VulkanRayTracingRequirement requirement) {
            switch (requirement) {
            case VulkanRayTracingRequirement::AccelerationStructureExtension:
            case VulkanRayTracingRequirement::RayTracingPipelineExtension:
            case VulkanRayTracingRequirement::DeferredHostOperationsExtension:
            case VulkanRayTracingRequirement::ComputeShaderDerivativesExtension:
                return RayTracingDiagnostic{RayTracingDiagnosticCode::MissingDeviceExtension,
                                            std::string(vulkanRayTracingRequirementName(requirement))};
            case VulkanRayTracingRequirement::BufferDeviceAddressFeature:
            case VulkanRayTracingRequirement::ShaderStorageBufferArrayNonUniformIndexingFeature:
            case VulkanRayTracingRequirement::DescriptorBindingPartiallyBoundFeature:
            case VulkanRayTracingRequirement::AccelerationStructureFeature:
            case VulkanRayTracingRequirement::RayTracingPipelineFeature:
            case VulkanRayTracingRequirement::ComputeDerivativeGroupQuadsFeature:
                return RayTracingDiagnostic{RayTracingDiagnosticCode::MissingDeviceFeature,
                                            std::string(vulkanRayTracingRequirementName(requirement))};
            case VulkanRayTracingRequirement::RayRecursionDepth:
                return RayTracingDiagnostic{RayTracingDiagnosticCode::InsufficientDeviceProperty,
                                            std::string(vulkanRayTracingRequirementName(requirement))};
            }
            throw std::invalid_argument("Invalid Vulkan ray tracing requirement.");
        }

        [[nodiscard]] std::string_view diagnosticPrefix(RayTracingDecisionStatus status) noexcept {
            switch (status) {
            case RayTracingDecisionStatus::Enabled:
                return "Ray tracing enabled";
            case RayTracingDecisionStatus::Disabled:
                return "Ray tracing disabled";
            case RayTracingDecisionStatus::Fallback:
                return "Ray tracing AUTO fallback to raster";
            case RayTracingDecisionStatus::Rejected:
                return "Ray tracing request rejected";
            }
            return "Invalid ray tracing decision";
        }

        [[nodiscard]] std::string formatDiagnostic(const RayTracingDiagnostic& diagnostic) {
            switch (diagnostic.code) {
            case RayTracingDiagnosticCode::DisabledByBuildPolicy:
                return "build policy is OFF";
            case RayTracingDiagnosticCode::DisabledByRuntimePolicy:
                return "runtime policy is OFF";
            case RayTracingDiagnosticCode::ConflictingPolicies:
                return "build policy OFF conflicts with runtime policy ON";
            case RayTracingDiagnosticCode::ImplementationUnavailable:
                return "RT implementation is absent from this build";
            case RayTracingDiagnosticCode::MissingDeviceExtension:
                return "missing Vulkan device extension " + diagnostic.subject;
            case RayTracingDiagnosticCode::MissingDeviceFeature:
                return "missing Vulkan feature " + diagnostic.subject;
            case RayTracingDiagnosticCode::InsufficientDeviceProperty:
                return "insufficient Vulkan device property " + diagnostic.subject;
            }
            return "unknown diagnostic " + diagnostic.subject;
        }

    } // namespace

    bool VulkanRayTracingSupport::supportsRayTracingPipeline() const noexcept {
        return rayTracingPipelineUsable;
    }

    bool VulkanRayTracingSupport::supportsRayQuery() const noexcept {
        return rayQueryUsable;
    }

    bool VulkanRayTracingSupport::supportsSharcShaderStorage() const noexcept {
        return sharcShaderStorageUsable;
    }

    bool RayTracingDecision::enabled() const noexcept {
        return status == RayTracingDecisionStatus::Enabled;
    }

    bool RayTracingDecision::rejected() const noexcept {
        return status == RayTracingDecisionStatus::Rejected;
    }

    bool RayTracingDecision::enablesRayQuery() const noexcept {
        return enabled() && tier == VulkanRayTracingTier::RayTracingPipelineWithRayQuery;
    }

    VulkanRayTracingExtensionSupport
    inspectVulkanRayTracingExtensions(std::span<const std::string_view> availableExtensions) noexcept {
        return VulkanRayTracingExtensionSupport{
            .accelerationStructure = containsExtension(availableExtensions, accelerationStructureExtension),
            .rayTracingPipeline = containsExtension(availableExtensions, rayTracingPipelineExtension),
            .deferredHostOperations = containsExtension(availableExtensions, deferredHostOperationsExtension),
            .rayQuery = containsExtension(availableExtensions, rayQueryExtension),
            .computeShaderDerivatives = containsExtension(availableExtensions, computeShaderDerivativesExtension),
        };
    }

    VulkanRayTracingSupport evaluateVulkanRayTracingSupport(const VulkanRayTracingDeviceProbe& probe) {
        VulkanRayTracingSupport result;
        result.bufferDeviceAddressUsable = probe.features.bufferDeviceAddress;
        result.accelerationStructureUsable = probe.extensions.accelerationStructure &&
                                             probe.extensions.deferredHostOperations &&
                                             probe.features.bufferDeviceAddress && probe.features.accelerationStructure;
        result.rayTracingPipelineUsable =
            result.accelerationStructureUsable && probe.extensions.rayTracingPipeline &&
            probe.extensions.computeShaderDerivatives && probe.features.shaderStorageBufferArrayNonUniformIndexing &&
            probe.features.descriptorBindingPartiallyBound && probe.features.rayTracingPipeline &&
            probe.features.computeDerivativeGroupQuads && probe.maxRayRecursionDepth >= 2;
        result.rayQueryUsable =
            result.accelerationStructureUsable && probe.extensions.rayQuery && probe.features.rayQuery;
        result.sharcShaderStorageUsable = probe.features.shaderInt64 && probe.features.shaderFloat16 &&
                                          probe.features.uniformAndStorageBuffer16BitAccess &&
                                          probe.features.storageBuffer16BitAccess;
        result.maxRayRecursionDepth = probe.maxRayRecursionDepth;

        if (!probe.extensions.accelerationStructure) {
            result.missingPipelineRequirements.push_back(VulkanRayTracingRequirement::AccelerationStructureExtension);
        }
        if (!probe.extensions.rayTracingPipeline) {
            result.missingPipelineRequirements.push_back(VulkanRayTracingRequirement::RayTracingPipelineExtension);
        }
        if (!probe.extensions.deferredHostOperations) {
            result.missingPipelineRequirements.push_back(VulkanRayTracingRequirement::DeferredHostOperationsExtension);
        }
        if (!probe.extensions.computeShaderDerivatives) {
            result.missingPipelineRequirements.push_back(
                VulkanRayTracingRequirement::ComputeShaderDerivativesExtension);
        }
        if (!probe.features.bufferDeviceAddress) {
            result.missingPipelineRequirements.push_back(VulkanRayTracingRequirement::BufferDeviceAddressFeature);
        }
        if (!probe.features.shaderStorageBufferArrayNonUniformIndexing) {
            result.missingPipelineRequirements.push_back(
                VulkanRayTracingRequirement::ShaderStorageBufferArrayNonUniformIndexingFeature);
        }
        if (!probe.features.descriptorBindingPartiallyBound) {
            result.missingPipelineRequirements.push_back(
                VulkanRayTracingRequirement::DescriptorBindingPartiallyBoundFeature);
        }
        if (probe.extensions.accelerationStructure && !probe.features.accelerationStructure) {
            result.missingPipelineRequirements.push_back(VulkanRayTracingRequirement::AccelerationStructureFeature);
        }
        if (probe.extensions.rayTracingPipeline && !probe.features.rayTracingPipeline) {
            result.missingPipelineRequirements.push_back(VulkanRayTracingRequirement::RayTracingPipelineFeature);
        }
        if (probe.extensions.computeShaderDerivatives && !probe.features.computeDerivativeGroupQuads) {
            result.missingPipelineRequirements.push_back(
                VulkanRayTracingRequirement::ComputeDerivativeGroupQuadsFeature);
        }
        if (probe.extensions.rayTracingPipeline && probe.maxRayRecursionDepth < 2) {
            result.missingPipelineRequirements.push_back(VulkanRayTracingRequirement::RayRecursionDepth);
        }

        if (result.rayTracingPipelineUsable && result.rayQueryUsable) {
            result.tier = VulkanRayTracingTier::RayTracingPipelineWithRayQuery;
        } else if (result.rayTracingPipelineUsable) {
            result.tier = VulkanRayTracingTier::RayTracingPipeline;
        } else if (result.rayQueryUsable) {
            result.tier = VulkanRayTracingTier::RayQuery;
        }

        return result;
    }

    RayTracingDecision resolveRayTracingPolicy(const RayTracingPolicy& policy, bool implementationBuilt,
                                               const VulkanRayTracingSupport& deviceSupport) {
        if (!validMode(policy.build) || !validMode(policy.runtime)) {
            throw std::invalid_argument("Invalid ray tracing policy mode.");
        }

        RayTracingDecision result;

        if (policy.build == RayTracingMode::Off) {
            if (policy.runtime == RayTracingMode::On) {
                result.status = RayTracingDecisionStatus::Rejected;
                result.diagnostics.push_back(
                    RayTracingDiagnostic{RayTracingDiagnosticCode::ConflictingPolicies, "build=OFF,runtime=ON"});
            } else {
                result.status = RayTracingDecisionStatus::Disabled;
                result.diagnostics.push_back(
                    RayTracingDiagnostic{RayTracingDiagnosticCode::DisabledByBuildPolicy, "build=OFF"});
            }
            return result;
        }

        if (policy.runtime == RayTracingMode::Off) {
            if (policy.build == RayTracingMode::On && !implementationBuilt) {
                result.status = RayTracingDecisionStatus::Rejected;
                result.diagnostics.push_back(
                    RayTracingDiagnostic{RayTracingDiagnosticCode::ImplementationUnavailable, "implementation"});
            } else {
                result.status = RayTracingDecisionStatus::Disabled;
                result.diagnostics.push_back(
                    RayTracingDiagnostic{RayTracingDiagnosticCode::DisabledByRuntimePolicy, "runtime=OFF"});
            }
            return result;
        }

        if (!implementationBuilt) {
            result.status = policy.build == RayTracingMode::On || policy.runtime == RayTracingMode::On
                                ? RayTracingDecisionStatus::Rejected
                                : RayTracingDecisionStatus::Fallback;
            result.diagnostics.push_back(
                RayTracingDiagnostic{RayTracingDiagnosticCode::ImplementationUnavailable, "implementation"});
            return result;
        }

        if (!deviceSupport.supportsRayTracingPipeline()) {
            result.status = policy.runtime == RayTracingMode::On ? RayTracingDecisionStatus::Rejected
                                                                 : RayTracingDecisionStatus::Fallback;
            result.diagnostics.reserve(deviceSupport.missingPipelineRequirements.size());
            for (const VulkanRayTracingRequirement requirement : deviceSupport.missingPipelineRequirements) {
                result.diagnostics.push_back(diagnosticFor(requirement));
            }
            if (result.diagnostics.empty()) {
                result.diagnostics.push_back(RayTracingDiagnostic{RayTracingDiagnosticCode::MissingDeviceFeature,
                                                                  "complete Ray Tracing Pipeline capability bundle"});
            }
            return result;
        }

        result.status = RayTracingDecisionStatus::Enabled;
        result.tier = deviceSupport.supportsRayQuery() ? VulkanRayTracingTier::RayTracingPipelineWithRayQuery
                                                       : VulkanRayTracingTier::RayTracingPipeline;
        return result;
    }

    std::vector<std::string_view> enabledVulkanRayTracingDeviceExtensions(const RayTracingDecision& decision) {
        if (!decision.enabled()) {
            return {};
        }

        std::vector<std::string_view> result{
            accelerationStructureExtension,
            rayTracingPipelineExtension,
            deferredHostOperationsExtension,
            computeShaderDerivativesExtension,
        };
        if (decision.enablesRayQuery()) {
            result.push_back(rayQueryExtension);
        }
        return result;
    }

    std::string_view vulkanRayTracingRequirementName(VulkanRayTracingRequirement requirement) noexcept {
        switch (requirement) {
        case VulkanRayTracingRequirement::AccelerationStructureExtension:
            return accelerationStructureExtension;
        case VulkanRayTracingRequirement::RayTracingPipelineExtension:
            return rayTracingPipelineExtension;
        case VulkanRayTracingRequirement::DeferredHostOperationsExtension:
            return deferredHostOperationsExtension;
        case VulkanRayTracingRequirement::ComputeShaderDerivativesExtension:
            return computeShaderDerivativesExtension;
        case VulkanRayTracingRequirement::BufferDeviceAddressFeature:
            return "bufferDeviceAddress";
        case VulkanRayTracingRequirement::ShaderStorageBufferArrayNonUniformIndexingFeature:
            return "shaderStorageBufferArrayNonUniformIndexing";
        case VulkanRayTracingRequirement::DescriptorBindingPartiallyBoundFeature:
            return "descriptorBindingPartiallyBound";
        case VulkanRayTracingRequirement::AccelerationStructureFeature:
            return "accelerationStructure";
        case VulkanRayTracingRequirement::RayTracingPipelineFeature:
            return "rayTracingPipeline";
        case VulkanRayTracingRequirement::ComputeDerivativeGroupQuadsFeature:
            return "computeDerivativeGroupQuads";
        case VulkanRayTracingRequirement::RayRecursionDepth:
            return "maxRayRecursionDepth >= 2";
        }
        return "Invalid";
    }

    std::string_view vulkanRayTracingTierName(VulkanRayTracingTier tier) noexcept {
        switch (tier) {
        case VulkanRayTracingTier::RasterOnly:
            return "RasterOnly";
        case VulkanRayTracingTier::RayQuery:
            return "RayQuery";
        case VulkanRayTracingTier::RayTracingPipeline:
            return "RayTracingPipeline";
        case VulkanRayTracingTier::RayTracingPipelineWithRayQuery:
            return "RayTracingPipelineWithRayQuery";
        }
        return "Invalid";
    }

    std::string formatRayTracingDecision(const RayTracingDecision& decision) {
        std::string result(diagnosticPrefix(decision.status));
        if (decision.enabled()) {
            result += " (";
            result += vulkanRayTracingTierName(decision.tier);
            result += ")";
        }

        for (std::size_t index = 0; index < decision.diagnostics.size(); ++index) {
            result += index == 0 ? ": " : "; ";
            result += formatDiagnostic(decision.diagnostics[index]);
        }
        result += '.';
        return result;
    }

} // namespace lumin::render
