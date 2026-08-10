#include "render/FrameGraph.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

    using lumin::render::FrameGraph;
    using lumin::render::FrameGraphAccelerationStructureDesc;
    using lumin::render::FrameGraphBarrierRecorder;
    using lumin::render::FrameGraphBufferDesc;
    using lumin::render::FrameGraphContext;
    using lumin::render::FrameGraphPassHandle;
    using lumin::render::FrameGraphPassType;
    using lumin::render::FrameGraphTextureDesc;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    class RecordingBarriers final : public FrameGraphBarrierRecorder {
    public:
        void beginTrackingTextureState(nvrhi::ITexture*, nvrhi::TextureSubresourceSet,
                                       nvrhi::ResourceStates state) override {
            require(state != nvrhi::ResourceStates::Unknown,
                    "FrameGraph must never begin tracking a texture from Unknown.");
            events.emplace_back("begin-texture");
        }

        void beginTrackingBufferState(nvrhi::IBuffer*, nvrhi::ResourceStates) override {
            events.emplace_back("begin-buffer");
        }

        void setTextureState(nvrhi::ITexture*, nvrhi::TextureSubresourceSet,
                             nvrhi::ResourceStates state) override {
            require(state != nvrhi::ResourceStates::Unknown,
                    "FrameGraph must never request an Unknown destination state.");
            require(state != nvrhi::ResourceStates::Common,
                    "FrameGraph must never request Common as a Vulkan image barrier destination.");
            if (state == nvrhi::ResourceStates::RenderTarget) {
                events.emplace_back("texture-render-target");
            } else if (state == nvrhi::ResourceStates::CopySource) {
                events.emplace_back("texture-copy-source");
            } else {
                events.emplace_back("texture-state");
            }
        }

        void setBufferState(nvrhi::IBuffer*, nvrhi::ResourceStates) override {
            events.emplace_back("buffer-state");
        }

        void setAccelerationStructureState(nvrhi::rt::IAccelStruct*, nvrhi::ResourceStates state) override {
            if (state == nvrhi::ResourceStates::AccelStructRead) {
                events.emplace_back("as-read");
            } else if (state == nvrhi::ResourceStates::AccelStructWrite) {
                events.emplace_back("as-write");
            } else {
                events.emplace_back("as-common");
            }
        }

        void commitBarriers() override {
            events.emplace_back("commit");
        }

        std::vector<std::string> events;
    };

    void testFrameGraphOwnsManualTextureTransitions() {
        FrameGraph graph;
        FrameGraphTextureDesc texture;
        texture.texture = reinterpret_cast<nvrhi::ITexture*>(static_cast<std::uintptr_t>(1));
        texture.initialState = nvrhi::ResourceStates::ShaderResource;
        texture.finalState = nvrhi::ResourceStates::Present;
        const auto target = graph.importTexture("target", texture);

        std::vector<std::string> passEvents;
        graph.addPass(
            "write", FrameGraphPassType::Graphics,
            [target](lumin::render::FrameGraphBuilder& builder) {
                builder.writeTexture(target, nvrhi::ResourceStates::RenderTarget);
            },
            [&passEvents](const FrameGraphContext&) {
                passEvents.emplace_back("pass");
            });

        RecordingBarriers barriers;
        FrameGraphContext context;
        context.barriers = &barriers;
        graph.execute(context);

        require(passEvents == std::vector<std::string>{"pass"}, "FrameGraph pass callback must execute.");
        require(barriers.events == std::vector<std::string>{"begin-texture", "texture-render-target", "commit",
                                                            "texture-state", "commit"},
                "FrameGraph must own NvRHI tracking, transition and commit ordering.");
    }

    void testTransientDefaultsUseConcreteFirstState() {
        const FrameGraphTextureDesc texture;
        require(texture.initialState == nvrhi::ResourceStates::Common,
                "Transient texture imports must use Common as NvRHI's supported Undefined first-use state.");
        require(texture.finalState == nvrhi::ResourceStates::Unknown,
                "Unknown remains only the sentinel that suppresses a final transition.");
    }

    void testReadOnlyPassesDoNotRequestTransitions() {
        FrameGraph graph;
        FrameGraphTextureDesc texture;
        texture.texture = reinterpret_cast<nvrhi::ITexture*>(static_cast<std::uintptr_t>(2));
        texture.initialState = nvrhi::ResourceStates::ShaderResource;
        const auto input = graph.importTexture("input", texture);

        for (const char* name : {"read-a", "read-b"}) {
            graph.addPass(name, FrameGraphPassType::Graphics,
                          [input](lumin::render::FrameGraphBuilder& builder) {
                              builder.readTexture(input, nvrhi::ResourceStates::ShaderResource);
                          },
                          [](const FrameGraphContext&) {});
        }

        RecordingBarriers barriers;
        graph.execute(FrameGraphContext{.barriers = &barriers});
        require(barriers.events == std::vector<std::string>{"begin-texture", "commit", "commit"},
                "Read-only passes in one state must not request transitions.");
    }

    void testSameStateWritesForceMemoryDependency() {
        FrameGraph graph;
        FrameGraphTextureDesc texture;
        texture.texture = reinterpret_cast<nvrhi::ITexture*>(static_cast<std::uintptr_t>(3));
        texture.initialState = nvrhi::ResourceStates::RenderTarget;
        const auto target = graph.importTexture("target", texture);

        for (const char* name : {"write-a", "write-b"}) {
            graph.addPass(name, FrameGraphPassType::Graphics,
                          [target](lumin::render::FrameGraphBuilder& builder) {
                              builder.writeTexture(target, nvrhi::ResourceStates::RenderTarget);
                          },
                          [](const FrameGraphContext&) {});
        }

        RecordingBarriers barriers;
        graph.execute(FrameGraphContext{.barriers = &barriers});
        require(barriers.events == std::vector<std::string>{"begin-texture", "commit", "texture-copy-source",
                                                            "texture-render-target", "commit"},
                "Same-state writes must force an NvRHI memory dependency before the second pass.");
    }

    void testAccelerationStructuresUseFrameGraphBarriers() {
        FrameGraph graph;
        FrameGraphAccelerationStructureDesc desc;
        desc.accelerationStructure =
            reinterpret_cast<nvrhi::rt::IAccelStruct*>(static_cast<std::uintptr_t>(4));
        desc.initialState = nvrhi::ResourceStates::Common;
        desc.finalState = nvrhi::ResourceStates::AccelStructRead;
        const auto tlas = graph.importAccelerationStructure("scene-tlas", desc);

        graph.addPass(
            "build-tlas", FrameGraphPassType::Compute,
            [tlas](lumin::render::FrameGraphBuilder& builder) {
                builder.writeAccelerationStructure(tlas);
            },
            [](const FrameGraphContext&) {});
        graph.addPass(
            "trace", FrameGraphPassType::RayTracing,
            [tlas](lumin::render::FrameGraphBuilder& builder) {
                builder.readAccelerationStructure(tlas);
            },
            [](const FrameGraphContext&) {});

        RecordingBarriers barriers;
        graph.execute(FrameGraphContext{.barriers = &barriers});
        const std::vector<std::string> expected{"as-common", "as-write", "commit", "as-read", "commit"};
        if (barriers.events != expected) {
            std::cerr << "Acceleration-structure barrier events:";
            for (const std::string& event : barriers.events) {
                std::cerr << ' ' << event;
            }
            std::cerr << '\n';
        }
        require(barriers.events == expected,
                "FrameGraph must transition TLAS build writes to ray tracing reads.");
        require(graph.resourceInfo(tlas).kind == lumin::render::FrameGraphResourceKind::AccelerationStructure,
                "Acceleration structures must remain distinct graph resources.");
    }

    void testExplicitPassDependenciesParticipateInTopologicalSorting() {
        FrameGraph graph;
        const auto synchronizationBuffer = graph.createBuffer("synchronization", FrameGraphBufferDesc{});
        graph.addPass(
            "prepare-dependency", FrameGraphPassType::Compute,
            [synchronizationBuffer](lumin::render::FrameGraphBuilder& builder) {
                builder.write(synchronizationBuffer);
            },
            [](const FrameGraphContext&) {
            });
        const FrameGraphPassHandle dependency = graph.addPass(
            "dependency", FrameGraphPassType::Compute,
            [synchronizationBuffer](lumin::render::FrameGraphBuilder& builder) {
                builder.read(synchronizationBuffer);
            },
            [](const FrameGraphContext&) {
            });
        graph.addPass(
            "dependent", FrameGraphPassType::RayTracing,
            [dependency](lumin::render::FrameGraphBuilder& builder) {
                builder.dependsOn(dependency);
            },
            [](const FrameGraphContext&) {
            });

        graph.compile();
        const std::span<const std::uint32_t> order = graph.executionOrder();
        require(order.size() == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2,
                "Explicit pass dependencies must participate in FrameGraph topological sorting.");
    }

    void testDuplicatePassDependenciesAreDeduplicated() {
        FrameGraph graph;
        const FrameGraphPassHandle dependency =
            graph.addPass("dependency", FrameGraphPassType::Compute, {}, [](const FrameGraphContext&) {
            });
        graph.addPass(
            "dependent", FrameGraphPassType::RayTracing,
            [dependency](lumin::render::FrameGraphBuilder& builder) {
                builder.dependsOn(dependency);
                builder.dependsOn(dependency);
            },
            [](const FrameGraphContext&) {
            });

        graph.compile();
        const std::span<const std::uint32_t> order = graph.executionOrder();
        require(order.size() == 2 && order[0] == 0 && order[1] == 1,
                "Duplicate explicit pass dependencies must collapse to one topology edge.");
    }

    void testInvalidAndNonEarlierPassDependenciesAreRejected() {
        bool invalidRejected = false;
        try {
            FrameGraph graph;
            graph.addPass(
                "invalid", FrameGraphPassType::Compute,
                [](lumin::render::FrameGraphBuilder& builder) {
                    builder.dependsOn({});
                },
                [](const FrameGraphContext&) {
                });
        } catch (const std::out_of_range&) {
            invalidRejected = true;
        }
        require(invalidRejected, "An invalid explicit pass dependency must be rejected during setup.");

        bool selfRejected = false;
        try {
            FrameGraph graph;
            graph.addPass(
                "self", FrameGraphPassType::Compute,
                [](lumin::render::FrameGraphBuilder& builder) {
                    builder.dependsOn(FrameGraphPassHandle{0});
                },
                [](const FrameGraphContext&) {
                });
        } catch (const std::invalid_argument&) {
            selfRejected = true;
        }
        require(selfRejected, "A self-referential explicit pass dependency must be rejected during setup.");

        bool forwardRejected = false;
        try {
            FrameGraph graph;
            graph.addPass(
                "forward", FrameGraphPassType::Compute,
                [](lumin::render::FrameGraphBuilder& builder) {
                    builder.dependsOn(FrameGraphPassHandle{1});
                },
                [](const FrameGraphContext&) {
                });
        } catch (const std::invalid_argument&) {
            forwardRejected = true;
        }
        require(forwardRejected, "A forward explicit pass dependency must be rejected during setup.");
    }

}

int main() {
    try {
        testFrameGraphOwnsManualTextureTransitions();
        testTransientDefaultsUseConcreteFirstState();
        testReadOnlyPassesDoNotRequestTransitions();
        testSameStateWritesForceMemoryDependency();
        testAccelerationStructuresUseFrameGraphBarriers();
        testExplicitPassDependenciesParticipateInTopologicalSorting();
        testDuplicatePassDependenciesAreDeduplicated();
        testInvalidAndNonEarlierPassDependenciesAreRejected();
        std::cout << "FrameGraphNvRhiManualBarriers PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FrameGraphNvRhiManualBarriers FAIL: " << error.what() << '\n';
        return 1;
    }
}
