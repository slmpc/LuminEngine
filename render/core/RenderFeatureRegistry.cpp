#include "render/core/RenderFeatureRegistry.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lumin::render::core {
    namespace {

        using ContractSet = std::unordered_set<FrameDataContract, FrameDataContractHash>;
        using ProducerMap = std::unordered_map<FrameDataContract, std::size_t, FrameDataContractHash>;

        void validateContracts(const FeatureDescriptor& descriptor) {
            ContractSet required;
            ContractSet optional;
            ContractSet outputs;
            for (const FrameDataContract& contract : descriptor.requiredInputs) {
                if (!required.insert(contract).second) {
                    throw std::invalid_argument("Feature has a duplicate required frame-data input: " +
                                                descriptor.id.value());
                }
            }
            for (const FrameDataContract& contract : descriptor.optionalInputs) {
                if (!optional.insert(contract).second || required.contains(contract)) {
                    throw std::invalid_argument("Feature has an overlapping optional frame-data input: " +
                                                descriptor.id.value());
                }
            }
            for (const FrameDataContract& contract : descriptor.outputs) {
                if (!outputs.insert(contract).second || required.contains(contract) || optional.contains(contract)) {
                    throw std::invalid_argument("Feature has a duplicate or self-consuming frame-data output: " +
                                                descriptor.id.value());
                }
            }
        }

        [[nodiscard]] ProducerMap buildProducerMap(const std::vector<const FeatureDescriptor*>& descriptors,
                                                   const std::vector<std::uint8_t>& active,
                                                   const ContractSet& externalInputs) {
            ProducerMap producers;
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                if (!active[index]) {
                    continue;
                }
                for (const FrameDataContract& output : descriptors[index]->outputs) {
                    if (externalInputs.contains(output)) {
                        throw std::invalid_argument("A Feature output conflicts with a recipe external input: " +
                                                    output.name());
                    }
                    const auto [_, inserted] = producers.emplace(output, index);
                    if (!inserted) {
                        throw std::invalid_argument("Multiple enabled Features produce frame-data contract: " +
                                                    output.name());
                    }
                }
            }
            return producers;
        }

        [[nodiscard]] bool deactivateOrReject(const FeatureDescriptor& descriptor, std::uint8_t& active,
                                              ResolvedRecipeFeature& state, RecipeFeatureActivation activation,
                                              std::string message) {
            if (descriptor.missingRequirementPolicy == MissingRequirementPolicy::RejectPlan) {
                throw std::runtime_error(std::move(message));
            }
            active = 0;
            state.activation = activation;
            return true;
        }

    } // namespace

    void RenderFeatureRegistry::registerFeature(FeatureDescriptor descriptor, RenderFeatureFactory factory) {
        if (!factory) {
            throw std::invalid_argument("Render Feature registration requires a factory.");
        }
        validateContracts(descriptor);
        if (indices_.contains(descriptor.id)) {
            throw std::invalid_argument("Duplicate Render Feature registration: " + descriptor.id.value());
        }

        const std::size_t index = entries_.size();
        FeatureId id = descriptor.id;
        entries_.push_back(Entry{std::move(descriptor), std::move(factory)});
        indices_.emplace(std::move(id), index);
    }

    bool RenderFeatureRegistry::contains(const FeatureId& id) const noexcept {
        return indices_.contains(id);
    }

    const FeatureDescriptor* RenderFeatureRegistry::descriptor(const FeatureId& id) const noexcept {
        const auto found = indices_.find(id);
        return found == indices_.end() ? nullptr : &entries_[found->second].descriptor;
    }

    std::unique_ptr<IRenderFeature> RenderFeatureRegistry::create(const FeatureId& id,
                                                                  const FeatureCreateContext& context) const {
        const auto found = indices_.find(id);
        if (found == indices_.end()) {
            throw std::out_of_range("Render Feature is not registered: " + id.value());
        }
        std::unique_ptr<IRenderFeature> feature = entries_[found->second].factory(context);
        if (!feature) {
            throw std::runtime_error("Render Feature factory returned null: " + id.value());
        }
        if (feature->descriptor().id != id) {
            throw std::runtime_error("Render Feature factory returned an incompatible descriptor: " + id.value());
        }
        return feature;
    }

    std::size_t RenderFeatureRegistry::size() const noexcept {
        return entries_.size();
    }

    ResolvedRecipeFeature::ResolvedRecipeFeature(FeatureId featureId) : id(std::move(featureId)) {
    }

    bool ResolvedRecipeFeature::enabled() const noexcept {
        return activation == RecipeFeatureActivation::Enabled;
    }

    const std::string& ResolvedRenderPipeline::id() const noexcept {
        return id_;
    }

    std::span<const ResolvedRecipeFeature> ResolvedRenderPipeline::features() const noexcept {
        return features_;
    }

    std::span<const FeatureId> ResolvedRenderPipeline::executionOrder() const noexcept {
        return executionOrder_;
    }

    const ResolvedRecipeFeature* ResolvedRenderPipeline::find(const FeatureId& id) const noexcept {
        const auto found = std::ranges::find(features_, id, &ResolvedRecipeFeature::id);
        return found == features_.end() ? nullptr : &*found;
    }

    ResolvedRenderPipeline RenderPipelineRecipeResolver::resolve(const RenderFeatureRegistry& registry,
                                                                 const RenderPipelineRecipe& recipe,
                                                                 const RenderDeviceCapabilities& capabilities) {
        if (recipe.id.empty()) {
            throw std::invalid_argument("Render pipeline recipes require an id.");
        }

        ResolvedRenderPipeline result;
        result.id_ = recipe.id;
        std::vector<const FeatureDescriptor*> descriptors;
        descriptors.reserve(recipe.features.size());
        result.features_.reserve(recipe.features.size());

        std::unordered_map<FeatureId, std::size_t, FeatureIdHash> selected;
        for (const FeatureId& id : recipe.features) {
            const FeatureDescriptor* descriptor = registry.descriptor(id);
            if (descriptor == nullptr) {
                throw std::invalid_argument("Recipe references an unregistered Render Feature: " + id.value());
            }
            if (!selected.emplace(id, descriptors.size()).second) {
                throw std::invalid_argument("Recipe selects a Render Feature more than once: " + id.value());
            }
            descriptors.push_back(descriptor);
            result.features_.emplace_back(id);
        }

        ContractSet externalInputs;
        for (const FrameDataContract& input : recipe.externalInputs) {
            if (!externalInputs.insert(input).second) {
                throw std::invalid_argument("Recipe has a duplicate external frame-data input: " + input.name());
            }
        }

        std::vector<std::uint8_t> active(descriptors.size(), 1);
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            const CapabilitySet missing = descriptors[index]->requiredCapabilities.difference(capabilities.supported);
            if (!missing.empty()) {
                result.features_[index].missingCapabilities = missing;
                static_cast<void>(deactivateOrReject(*descriptors[index], active[index], result.features_[index],
                                                     RecipeFeatureActivation::DisabledMissingCapabilities,
                                                     "Required capabilities are unavailable for Render Feature: " +
                                                         descriptors[index]->id.value()));
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            const ProducerMap producers = buildProducerMap(descriptors, active, externalInputs);
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                if (!active[index]) {
                    continue;
                }
                const FeatureDescriptor& descriptor = *descriptors[index];
                for (const FeatureId& dependency : descriptor.dependencies) {
                    const auto found = selected.find(dependency);
                    if (found == selected.end()) {
                        throw std::invalid_argument("Recipe omits explicit dependency '" + dependency.value() +
                                                    "' required by Feature '" + descriptor.id.value() + "'.");
                    }
                    if (!active[found->second]) {
                        result.features_[index].unavailableDependency = dependency;
                        changed |= deactivateOrReject(descriptor, active[index], result.features_[index],
                                                      RecipeFeatureActivation::DisabledDependency,
                                                      "A required Render Feature dependency is unavailable: " +
                                                          descriptor.id.value());
                        break;
                    }
                }
                if (!active[index]) {
                    continue;
                }
                for (const FrameDataContract& input : descriptor.requiredInputs) {
                    if (!externalInputs.contains(input) && !producers.contains(input)) {
                        result.features_[index].unavailableInput = input;
                        changed |= deactivateOrReject(
                            descriptor, active[index], result.features_[index], RecipeFeatureActivation::DisabledInput,
                            "Required frame data is unavailable for Render Feature: " + descriptor.id.value() + " (" +
                                input.name() + ")");
                        break;
                    }
                }
            }
        }

        const ProducerMap producers = buildProducerMap(descriptors, active, externalInputs);
        std::unordered_map<HistoryDomain, FeatureId> historyOwners;
        std::vector<std::vector<std::size_t>> edges(descriptors.size());
        std::vector<std::size_t> indegree(descriptors.size(), 0);
        const auto addEdge = [&](std::size_t producer, std::size_t consumer) {
            if (producer == consumer) {
                throw std::invalid_argument("Render pipeline recipe contains a self dependency.");
            }
            auto& outgoing = edges[producer];
            if (std::ranges::find(outgoing, consumer) == outgoing.end()) {
                outgoing.push_back(consumer);
                ++indegree[consumer];
            }
        };

        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            if (!active[index]) {
                continue;
            }
            const FeatureDescriptor& descriptor = *descriptors[index];
            for (const HistoryDomain domain : descriptor.historyDomains) {
                const auto [found, inserted] = historyOwners.emplace(domain, descriptor.id);
                if (!inserted) {
                    throw std::invalid_argument("Multiple enabled Features own the same history domain: " +
                                                found->second.value() + " and " + descriptor.id.value());
                }
            }
            for (const FeatureId& dependency : descriptor.dependencies) {
                addEdge(selected.at(dependency), index);
            }
            for (const FeatureId& predecessor : descriptor.after) {
                const auto found = selected.find(predecessor);
                if (found == selected.end()) {
                    throw std::invalid_argument("Recipe omits ordering predecessor '" + predecessor.value() + "'.");
                }
                if (active[found->second]) {
                    addEdge(found->second, index);
                }
            }
            const auto connectInput = [&](const FrameDataContract& input) {
                const auto producer = producers.find(input);
                if (producer != producers.end()) {
                    addEdge(producer->second, index);
                }
            };
            for (const FrameDataContract& input : descriptor.requiredInputs) {
                connectInput(input);
            }
            for (const FrameDataContract& input : descriptor.optionalInputs) {
                connectInput(input);
            }
        }

        std::deque<std::size_t> ready;
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            if (active[index] && indegree[index] == 0) {
                ready.push_back(index);
            }
        }
        while (!ready.empty()) {
            const std::size_t index = ready.front();
            ready.pop_front();
            result.executionOrder_.push_back(descriptors[index]->id);
            for (const std::size_t consumer : edges[index]) {
                if (--indegree[consumer] == 0) {
                    const auto insertion = std::ranges::find_if(ready, [consumer](std::size_t queued) {
                        return consumer < queued;
                    });
                    ready.insert(insertion, consumer);
                }
            }
        }

        const std::size_t activeCount = static_cast<std::size_t>(std::ranges::count(active, std::uint8_t{1}));
        if (result.executionOrder_.size() != activeCount) {
            throw std::invalid_argument("Render pipeline recipe contains a Feature dependency cycle.");
        }
        return result;
    }

} // namespace lumin::render::core
