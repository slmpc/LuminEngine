#include "application/LogicRuntime.hpp"

#include "render/world/RenderWorld.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace lumin::core {
    namespace {

        using Clock = std::chrono::steady_clock;

        struct PendingCommand {
            editor::EditorCommandId id = 0;
            editor::EditorLogicCommand command;
        };

        [[nodiscard]] std::chrono::nanoseconds tickPeriod(std::uint32_t tickRate) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>{1.0 / static_cast<double>(tickRate)});
        }

        [[nodiscard]] std::string exceptionMessage(const std::exception_ptr& failure) {
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& exception) {
                return exception.what();
            } catch (...) {
                return "Logic Runtime failed with a non-standard exception.";
            }
        }

    } // namespace

    struct LogicRuntime::Impl final {
        Impl(LogicRuntimeConfig configValue, std::unique_ptr<game::Game> gameValue)
            : config(std::move(configValue)), pendingGame(std::move(gameValue)) {
            if (pendingGame == nullptr) {
                throw std::invalid_argument("LogicRuntime requires a Game instance.");
            }
            worker = std::thread([this] {
                run();
            });
            std::unique_lock lock{mutex};
            changed.wait(lock, [this] {
                return runtimeStatus.state != LogicRuntimeState::Starting;
            });
            if (runtimeStatus.state == LogicRuntimeState::Failed) {
                const std::exception_ptr startupFailure = failure;
                lock.unlock();
                worker.join();
                std::rethrow_exception(startupFailure);
            }
        }

        ~Impl() {
            stop();
        }

        [[nodiscard]] std::shared_ptr<const editor::EditorLogicSnapshot>
        makeSnapshot(scene::Level& level, const scene::Camera& camera, scripting::ScriptRuntime& scripts,
                     project::ProjectSession& projectSession, render::world::RenderWorldCache& worldCache,
                     std::uint64_t tick, std::uint64_t lastCommand, std::string diagnostic) {
            auto result = std::make_shared<editor::EditorLogicSnapshot>();
            result->revision = nextSnapshotRevision++;
            result->logicTick = tick;
            result->lastAppliedCommand = lastCommand;
            result->renderWorld = worldCache.sync(level).snapshot;
            result->camera = camera;
            result->environment = level.environment();
            result->diagnostic = std::move(diagnostic);
            for (const scene::ActorHandle handle : level.actorHandles()) {
                const scene::Actor* actor = level.actor(handle);
                if (actor != nullptr) {
                    result->actors.push_back(
                        {handle, actor->name(), actor->transform(), actor->material(), actor->modelHandle()});
                }
            }
            for (const scene::ModelHandle handle : level.modelHandles()) {
                const scene::ModelInstance& model = level.model(handle);
                result->models.push_back(
                    {handle, model, level.actorForModel(handle), projectSession.assetForMesh(model.mesh)});
            }
            result->scripts = scripts.scripts();
            result->scriptDiagnostics = scripts.diagnostics();
            result->consoleHistory = scripts.consoleHistory();
            result->project.open = projectSession.hasProject();
            result->project.dirty = projectSession.dirty();
            result->project.projectFile = projectSession.projectFile();
            result->project.rootDirectory = projectSession.rootDirectory();
            result->project.manifest = projectSession.manifest();
            result->project.assets = projectSession.assets();
            result->project.entries = projectSession.projectEntries();
            result->project.diagnostics = projectSession.diagnostics();
            result->project.settings = projectSession.settings();
            return result;
        }

        void publish(std::shared_ptr<const editor::EditorLogicSnapshot> value) noexcept {
            latestSnapshot.store(std::move(value), std::memory_order_release);
        }

        void run() noexcept {
            try {
                scene::Level level;
                scene::Camera camera;
                scripting::ScriptRuntime scripts({.scriptRoot = config.scriptRoot,
                                                  .diagnosticCapacity = 256,
                                                  .consoleHistoryCapacity = 128,
                                                  .diagnosticSink = {}});
                project::ProjectSession projectSession(level, camera, scripts);
                render::world::RenderWorldCache worldCache;
                std::unique_ptr<game::Game> game = std::move(pendingGame);
                game::GameContext gameContext{level, camera, scripts};
                const auto startupScript = game::initializeGame(*game, gameContext, config.startupScript);
                static_cast<void>(startupScript);

                std::string startupDiagnostic;
                if (config.startupProject.has_value()) {
                    std::string error;
                    if (!projectSession.open(*config.startupProject, error)) {
                        projectSession.close();
                        startupDiagnostic = std::move(error);
                    }
                }

                std::uint64_t tick = 0;
                std::uint64_t lastAppliedCommand = 0;
                publish(makeSnapshot(level, camera, scripts, projectSession, worldCache, tick, lastAppliedCommand,
                                     startupDiagnostic));
                {
                    std::lock_guard lock{mutex};
                    runtimeStatus.state = LogicRuntimeState::Ready;
                    runtimeStatus.logicTickHz = projectSession.settings().logicTickHz;
                    runtimeStatus.diagnostic = startupDiagnostic;
                }
                changed.notify_all();

                std::uint32_t activeTickRate = projectSession.settings().logicTickHz;
                Clock::time_point nextTick = Clock::now() + tickPeriod(activeTickRate);
                while (true) {
                    std::deque<PendingCommand> localCommands;
                    std::optional<scene::Camera> pendingCamera;
                    {
                        std::unique_lock lock{mutex};
                        changed.wait_until(lock, nextTick, [this] {
                            return stopRequested || !commands.empty();
                        });
                        if (stopRequested) {
                            break;
                        }
                        localCommands.swap(commands);
                        if (publishedCameraGeneration != consumedCameraGeneration) {
                            pendingCamera = latestCamera;
                            consumedCameraGeneration = publishedCameraGeneration;
                        }
                    }

                    // Camera 由渲染主线程逐帧更新；逻辑线程仅在命令和 Tick 前同步镜像。
                    bool stateChanged = pendingCamera.has_value();
                    if (pendingCamera.has_value()) {
                        camera = std::move(*pendingCamera);
                    }
                    for (PendingCommand& pending : localCommands) {
                        editor::EditorCommandOutcome outcome;
                        try {
                            outcome = pending.command(level, camera, scripts, projectSession);
                        } catch (const std::exception& exception) {
                            outcome.succeeded = false;
                            outcome.message = exception.what();
                        } catch (...) {
                            outcome.succeeded = false;
                            outcome.message = "Editor command failed with a non-standard exception.";
                        }
                        lastAppliedCommand = pending.id;
                        {
                            std::lock_guard lock{mutex};
                            results.push_back({pending.id, std::move(outcome)});
                        }
                        stateChanged = true;
                    }

                    const Clock::time_point now = Clock::now();
                    if (now >= nextTick) {
                        LogicInputState input;
                        {
                            std::lock_guard lock{mutex};
                            input = latestInput;
                        }
                        game::advanceGameFrame(*game, gameContext, input.game,
                                               1.0f / static_cast<float>(activeTickRate));
                        ++tick;
                        stateChanged = true;
                        {
                            std::lock_guard lock{mutex};
                            ++runtimeStatus.completedTicks;
                            if (now > nextTick + tickPeriod(activeTickRate)) {
                                ++runtimeStatus.missedDeadlines;
                            }
                        }
                        // 不追算积压；逻辑时间在过载时允许变慢。
                        nextTick = Clock::now() + tickPeriod(activeTickRate);
                    }

                    const std::uint32_t requestedTickRate = projectSession.hasProject()
                                                                ? projectSession.settings().logicTickHz
                                                                : project::DefaultLogicTickHz;
                    if (requestedTickRate != activeTickRate) {
                        activeTickRate = requestedTickRate;
                        nextTick = Clock::now() + tickPeriod(activeTickRate);
                        std::lock_guard lock{mutex};
                        runtimeStatus.logicTickHz = activeTickRate;
                    }
                    if (stateChanged) {
                        publish(makeSnapshot(level, camera, scripts, projectSession, worldCache, tick,
                                             lastAppliedCommand, {}));
                    }
                }

                // 所有逻辑对象在创建它们的线程按依赖逆序销毁。
                game.reset();
                {
                    std::lock_guard lock{mutex};
                    runtimeStatus.state = LogicRuntimeState::Stopped;
                }
                changed.notify_all();
            } catch (...) {
                const std::exception_ptr currentFailure = std::current_exception();
                {
                    std::lock_guard lock{mutex};
                    failure = currentFailure;
                    runtimeStatus.state = LogicRuntimeState::Failed;
                    runtimeStatus.diagnostic = exceptionMessage(currentFailure);
                }
                changed.notify_all();
            }
        }

        [[nodiscard]] editor::EditorCommandId submit(editor::EditorLogicCommand command) {
            if (!command) {
                throw std::invalid_argument("LogicRuntime cannot submit an empty Editor command.");
            }
            std::lock_guard lock{mutex};
            if (runtimeStatus.state != LogicRuntimeState::Ready || stopRequested) {
                throw std::logic_error("LogicRuntime is not accepting Editor commands.");
            }
            const editor::EditorCommandId id = nextCommandId++;
            commands.push_back({id, std::move(command)});
            changed.notify_one();
            return id;
        }

        void stop() {
            {
                std::lock_guard lock{mutex};
                stopRequested = true;
            }
            changed.notify_all();
            if (worker.joinable()) {
                worker.join();
            }
        }

        LogicRuntimeConfig config;
        std::unique_ptr<game::Game> pendingGame;
        std::thread worker;
        mutable std::mutex mutex;
        std::condition_variable changed;
        std::deque<PendingCommand> commands;
        std::vector<editor::EditorCommandResult> results;
        LogicInputState latestInput;
        /** 渲染线程最近发布的 Camera 值及其 latest-wins 同步代数。 */
        scene::Camera latestCamera;
        std::uint64_t publishedCameraGeneration = 0;
        std::uint64_t consumedCameraGeneration = 0;
        bool stopRequested = false;
        editor::EditorCommandId nextCommandId = 1;
        std::uint64_t nextSnapshotRevision = 1;
        std::atomic<std::shared_ptr<const editor::EditorLogicSnapshot>> latestSnapshot;
        LogicRuntimeStatus runtimeStatus;
        std::exception_ptr failure;
    };

    LogicRuntime::LogicRuntime(LogicRuntimeConfig config, std::unique_ptr<game::Game> game)
        : impl_(std::make_unique<Impl>(std::move(config), std::move(game))) {
    }

    LogicRuntime::~LogicRuntime() = default;

    std::shared_ptr<const editor::EditorLogicSnapshot> LogicRuntime::snapshot() const noexcept {
        return impl_->latestSnapshot.load(std::memory_order_acquire);
    }

    editor::EditorCommandId LogicRuntime::submit(editor::EditorLogicCommand command) {
        return impl_->submit(std::move(command));
    }

    std::vector<editor::EditorCommandResult> LogicRuntime::drainResults() {
        std::lock_guard lock{impl_->mutex};
        std::vector<editor::EditorCommandResult> result;
        result.swap(impl_->results);
        return result;
    }

    void LogicRuntime::publishInput(LogicInputState input) {
        std::lock_guard lock{impl_->mutex};
        impl_->latestInput = std::move(input);
    }

    void LogicRuntime::publishCamera(scene::Camera camera) {
        std::lock_guard lock{impl_->mutex};
        impl_->latestCamera = std::move(camera);
        ++impl_->publishedCameraGeneration;
    }

    LogicRuntimeStatus LogicRuntime::status() const {
        std::lock_guard lock{impl_->mutex};
        return impl_->runtimeStatus;
    }

    void LogicRuntime::rethrowIfFailed() const {
        std::lock_guard lock{impl_->mutex};
        if (impl_->runtimeStatus.state == LogicRuntimeState::Failed && impl_->failure != nullptr) {
            std::rethrow_exception(impl_->failure);
        }
    }

    void LogicRuntime::stop() {
        impl_->stop();
    }

} // namespace lumin::core
