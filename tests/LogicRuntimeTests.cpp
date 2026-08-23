#include "application/LogicRuntime.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

    using namespace std::chrono_literals;

    void require(bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error(std::string{message});
        }
    }

    template <typename Predicate> void waitUntil(Predicate&& predicate, std::string_view failureMessage) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error(std::string{failureMessage});
            }
            std::this_thread::sleep_for(2ms);
        }
    }

    struct TemporaryDirectory {
        TemporaryDirectory() {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("lumin-logic-runtime-tests-" + std::to_string(suffix));
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }

        std::filesystem::path path;
    };

    class CountingGame final : public lumin::game::Game {
    public:
        explicit CountingGame(std::shared_ptr<std::atomic_uint64_t> ticks) : ticks_(std::move(ticks)) {
        }

        void initialize(lumin::game::GameContext&) override {
        }

        void tick(lumin::game::GameContext&, float) override {
            ticks_->fetch_add(1, std::memory_order_relaxed);
        }

    private:
        std::shared_ptr<std::atomic_uint64_t> ticks_;
    };

    void waitForCommand(lumin::core::LogicRuntime& runtime, lumin::editor::EditorCommandId command) {
        waitUntil(
            [&runtime, command] {
                const auto snapshot = runtime.snapshot();
                return snapshot != nullptr && snapshot->lastAppliedCommand >= command;
            },
            "Logic Runtime did not publish the completed Editor command.");
        runtime.rethrowIfFailed();
    }

    void testProjectTickRateControlsLogicRuntime() {
        TemporaryDirectory temporary;
        auto gameTicks = std::make_shared<std::atomic_uint64_t>(0);
        lumin::core::LogicRuntime runtime({.scriptRoot = temporary.path}, std::make_unique<CountingGame>(gameTicks));

        const std::uint64_t initialTicks = runtime.status().completedTicks;
        waitUntil(
            [&runtime, initialTicks] {
                return runtime.status().completedTicks >= initialTicks + 2;
            },
            "Logic Runtime must advance fixed-rate ticks without render frames.");
        require(gameTicks->load(std::memory_order_relaxed) >= 2,
                "Logic Runtime must invoke the injected Game on its owning thread.");

        const auto createProject = runtime.submit([root = temporary.path](lumin::scene::Level&, lumin::scene::Camera&,
                                                                          lumin::scripting::ScriptRuntime&,
                                                                          lumin::project::ProjectSession& project) {
            std::string error;
            lumin::editor::EditorCommandOutcome outcome;
            outcome.succeeded = project.create(root, "TickRate", error);
            outcome.message = std::move(error);
            return outcome;
        });
        waitForCommand(runtime, createProject);

        const auto setTickRate = [&runtime](std::uint32_t tickRate) {
            return runtime.submit([tickRate](lumin::scene::Level&, lumin::scene::Camera&,
                                             lumin::scripting::ScriptRuntime&,
                                             lumin::project::ProjectSession& project) {
                auto settings = project.settings();
                settings.logicTickHz = tickRate;
                project.setSettings(std::move(settings));
                return lumin::editor::EditorCommandOutcome{};
            });
        };

        waitForCommand(runtime, setTickRate(144));
        waitUntil(
            [&runtime] {
                return runtime.status().logicTickHz == 144;
            },
            "Project Settings must update the live logic frequency to 144 Hz.");

        waitForCommand(runtime, setTickRate(1));
        waitUntil(
            [&runtime] {
                return runtime.status().logicTickHz == lumin::project::MinimumLogicTickHz;
            },
            "Logic frequencies below the supported range must normalize to 15 Hz.");

        waitForCommand(runtime, setTickRate(1'000));
        waitUntil(
            [&runtime] {
                return runtime.status().logicTickHz == lumin::project::MaximumLogicTickHz;
            },
            "Logic frequencies above the supported range must normalize to 240 Hz.");

        const auto closeProject =
            runtime.submit([](lumin::scene::Level&, lumin::scene::Camera&, lumin::scripting::ScriptRuntime&,
                              lumin::project::ProjectSession& project) {
                project.close();
                return lumin::editor::EditorCommandOutcome{};
            });
        waitForCommand(runtime, closeProject);
        waitUntil(
            [&runtime] {
                return runtime.status().logicTickHz == lumin::project::DefaultLogicTickHz;
            },
            "Closing the project must restore the default 60 Hz logic frequency.");

        runtime.stop();
        require(runtime.status().state == lumin::core::LogicRuntimeState::Stopped,
                "Stopping Logic Runtime must join the logic thread deterministically.");
    }

    void testRenderCameraIsMirroredBeforeEditorCommands() {
        TemporaryDirectory temporary;
        auto gameTicks = std::make_shared<std::atomic_uint64_t>(0);
        lumin::core::LogicRuntime runtime({.scriptRoot = temporary.path}, std::make_unique<CountingGame>(gameTicks));

        lumin::scene::Camera camera;
        camera.setPosition({7.0f, 8.0f, 9.0f});
        camera.setOrientation(35.0f, -12.0f);
        camera.setMoveSpeed(11.0f);
        camera.markCut();
        runtime.publishCamera(camera);
        const auto command = runtime.submit([](lumin::scene::Level&, lumin::scene::Camera&,
                                               lumin::scripting::ScriptRuntime&, lumin::project::ProjectSession&) {
            return lumin::editor::EditorCommandOutcome{};
        });
        waitForCommand(runtime, command);

        const auto snapshot = runtime.snapshot();
        require(snapshot != nullptr && snapshot->camera.position() == camera.position() &&
                    snapshot->camera.yawDegrees() == camera.yawDegrees() &&
                    snapshot->camera.pitchDegrees() == camera.pitchDegrees() &&
                    snapshot->camera.moveSpeed() == camera.moveSpeed() &&
                    snapshot->camera.cutEpoch() == camera.cutEpoch(),
                "Logic Runtime must consume the latest render Camera mirror before an Editor command.");
        runtime.stop();
    }

} // namespace

int main() {
    try {
        testProjectTickRateControlsLogicRuntime();
        testRenderCameraIsMirroredBeforeEditorCommands();
        return 0;
    } catch (const std::exception& exception) {
        return exception.what()[0] == '\0' ? 2 : 1;
    }
}
