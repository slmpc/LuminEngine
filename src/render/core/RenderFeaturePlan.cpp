#include "render/core/RenderFeaturePlan.hpp"

#include <cstddef>
#include <functional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lumin::render::core {

    namespace {

        struct FeatureGraph {
            std::unordered_map<std::string, std::size_t> indexById;
            std::vector<std::size_t> order;
        };

        [[nodiscard]] std::string capabilityList(CapabilitySet capabilities) {
            std::ostringstream stream;
            bool first = true;
            for (std::size_t index = 0; index < static_cast<std::size_t>(RenderCapability::Count); ++index) {
                const auto capability = static_cast<RenderCapability>(index);
                if (!capabilities.contains(capability)) {
                    continue;
                }
                if (!first) {
                    stream << ", ";
                }
                first = false;
                stream << renderCapabilityName(capability);
            }
            return stream.str();
        }

        [[nodiscard]] FeatureGraph buildFeatureGraph(std::span<const FeatureDescriptor> descriptors) {
            FeatureGraph graph;
            graph.indexById.reserve(descriptors.size());
            std::unordered_map<std::size_t, std::string> historyOwnerByDomain;
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                const FeatureDescriptor& descriptor = descriptors[index];
                const bool inserted = graph.indexById.emplace(descriptor.id.value(), index).second;
                if (!inserted) {
                    throw std::invalid_argument(
                        "Render feature plan contains duplicate feature id: " + descriptor.id.value() + ".");
                }

                if (descriptor.missingRequirementPolicy != MissingRequirementPolicy::DisableFeature &&
                    descriptor.missingRequirementPolicy != MissingRequirementPolicy::RejectPlan) {
                    throw std::invalid_argument("Render feature '" + descriptor.id.value() +
                                                "' contains an invalid missing requirement policy.");
                }

                const CapabilitySet overlap =
                    descriptor.requiredCapabilities.intersection(descriptor.optionalCapabilities);
                if (!overlap.empty()) {
                    throw std::invalid_argument(
                        "Render feature '" + descriptor.id.value() +
                        "' declares capabilities as both required and optional: " + capabilityList(overlap) + ".");
                }

                std::unordered_set<std::size_t> historyDomains;
                for (const HistoryDomain domain : descriptor.historyDomains) {
                    const std::size_t domainValue = static_cast<std::size_t>(domain);
                    if (domainValue >= static_cast<std::size_t>(HistoryDomain::Count)) {
                        throw std::invalid_argument("Render feature '" + descriptor.id.value() +
                                                    "' contains an invalid history domain.");
                    }
                    if (!historyDomains.insert(domainValue).second) {
                        throw std::invalid_argument("Render feature '" + descriptor.id.value() +
                                                    "' contains a duplicate history domain.");
                    }
                    const auto [owner, insertedOwner] =
                        historyOwnerByDomain.emplace(domainValue, descriptor.id.value());
                    if (!insertedOwner) {
                        throw std::invalid_argument("Render features '" + owner->second + "' and '" +
                                                    descriptor.id.value() +
                                                    "' both claim ownership of the same history domain.");
                    }
                }
            }

            std::vector<std::vector<std::size_t>> dependants(descriptors.size());
            std::vector<std::size_t> indegree(descriptors.size(), 0);
            for (std::size_t featureIndex = 0; featureIndex < descriptors.size(); ++featureIndex) {
                const FeatureDescriptor& descriptor = descriptors[featureIndex];
                std::unordered_set<std::string> dependencies;
                for (const FeatureId& dependency : descriptor.dependencies) {
                    const auto dependencyIterator = graph.indexById.find(dependency.value());
                    if (dependencyIterator == graph.indexById.end()) {
                        throw std::invalid_argument("Render feature '" + descriptor.id.value() +
                                                    "' references missing dependency '" + dependency.value() + "'.");
                    }
                    if (!dependencies.insert(dependency.value()).second) {
                        throw std::invalid_argument("Render feature '" + descriptor.id.value() +
                                                    "' contains duplicate dependency '" + dependency.value() + "'.");
                    }
                    if (dependencyIterator->second == featureIndex) {
                        throw std::invalid_argument("Render feature '" + descriptor.id.value() +
                                                    "' cannot depend on itself.");
                    }
                    dependants[dependencyIterator->second].push_back(featureIndex);
                    ++indegree[featureIndex];
                }
            }

            std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                if (indegree[index] == 0) {
                    ready.push(index);
                }
            }

            graph.order.reserve(descriptors.size());
            while (!ready.empty()) {
                const std::size_t featureIndex = ready.top();
                ready.pop();
                graph.order.push_back(featureIndex);
                for (const std::size_t dependant : dependants[featureIndex]) {
                    --indegree[dependant];
                    if (indegree[dependant] == 0) {
                        ready.push(dependant);
                    }
                }
            }

            if (graph.order.size() != descriptors.size()) {
                throw std::invalid_argument("Render feature dependency graph contains a cycle.");
            }
            return graph;
        }

    } // namespace

    FeatureId::FeatureId(std::string_view value) : value_(value) {
        if (value_.empty()) {
            throw std::invalid_argument("Feature id must not be empty.");
        }
    }

    const std::string& FeatureId::value() const noexcept {
        return value_;
    }

    std::size_t FeatureIdHash::operator()(const FeatureId& id) const noexcept {
        return std::hash<std::string>{}(id.value());
    }

    FeatureDescriptor::FeatureDescriptor(FeatureId featureId) : id(std::move(featureId)) {
    }

    ResolvedRenderFeature::ResolvedRenderFeature(FeatureId featureId) : id(std::move(featureId)) {
    }

    bool ResolvedRenderFeature::enabled() const noexcept {
        return activation == FeatureActivation::Enabled;
    }

    std::span<const ResolvedRenderFeature> ResolvedRenderFeaturePlan::features() const noexcept {
        return features_;
    }

    std::span<const FeatureId> ResolvedRenderFeaturePlan::executionOrder() const noexcept {
        return executionOrder_;
    }

    const ResolvedRenderFeature* ResolvedRenderFeaturePlan::find(const FeatureId& id) const noexcept {
        for (const ResolvedRenderFeature& feature : features_) {
            if (feature.id == id) {
                return &feature;
            }
        }
        return nullptr;
    }

    void RenderFeaturePlan::addFeature(FeatureDescriptor descriptor) {
        descriptors_.push_back(std::move(descriptor));
    }

    void RenderFeaturePlan::clear() noexcept {
        descriptors_.clear();
    }

    std::span<const FeatureDescriptor> RenderFeaturePlan::descriptors() const noexcept {
        return descriptors_;
    }

    void RenderFeaturePlan::validate() const {
        (void)buildFeatureGraph(descriptors_);
    }

    std::vector<FeatureId> RenderFeaturePlan::topologicalOrder() const {
        const FeatureGraph graph = buildFeatureGraph(descriptors_);
        std::vector<FeatureId> result;
        result.reserve(graph.order.size());
        for (const std::size_t index : graph.order) {
            result.push_back(descriptors_[index].id);
        }
        return result;
    }

    ResolvedRenderFeaturePlan RenderFeaturePlan::resolve(const RenderDeviceCapabilities& deviceCapabilities) const {
        const FeatureGraph graph = buildFeatureGraph(descriptors_);
        ResolvedRenderFeaturePlan result;
        result.features_.reserve(descriptors_.size());
        result.executionOrder_.reserve(descriptors_.size());
        std::vector<bool> enabled(descriptors_.size(), false);

        for (const std::size_t featureIndex : graph.order) {
            const FeatureDescriptor& descriptor = descriptors_[featureIndex];
            ResolvedRenderFeature resolved(descriptor.id);
            resolved.historyDomains = descriptor.historyDomains;
            resolved.availableOptionalCapabilities =
                descriptor.optionalCapabilities.intersection(deviceCapabilities.supported);
            resolved.missingRequiredCapabilities =
                descriptor.requiredCapabilities.difference(deviceCapabilities.supported);

            if (!resolved.missingRequiredCapabilities.empty()) {
                if (descriptor.missingRequirementPolicy == MissingRequirementPolicy::RejectPlan) {
                    throw std::runtime_error("Render feature '" + descriptor.id.value() +
                                             "' is missing required capabilities: " +
                                             capabilityList(resolved.missingRequiredCapabilities) + ".");
                }
                resolved.activation = FeatureActivation::DisabledMissingCapabilities;
            } else {
                for (const FeatureId& dependency : descriptor.dependencies) {
                    const std::size_t dependencyIndex = graph.indexById.at(dependency.value());
                    if (!enabled[dependencyIndex]) {
                        resolved.activation = FeatureActivation::DisabledDependency;
                        resolved.unavailableDependency = dependency;
                        if (descriptor.missingRequirementPolicy == MissingRequirementPolicy::RejectPlan) {
                            throw std::runtime_error("Render feature '" + descriptor.id.value() +
                                                     "' depends on unavailable feature '" + dependency.value() + "'.");
                        }
                        break;
                    }
                }
            }

            enabled[featureIndex] = resolved.enabled();
            if (resolved.enabled()) {
                result.executionOrder_.push_back(resolved.id);
            }
            result.features_.push_back(std::move(resolved));
        }
        return result;
    }

} // namespace lumin::render::core
