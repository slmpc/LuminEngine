#include "lumin/scripting/ScriptRuntime.hpp"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace lumin::scripting {

    namespace {

        constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
        constexpr std::uint64_t FnvPrime = 1099511628211ULL;

        std::uint64_t contentHash(std::string_view content) noexcept {
            std::uint64_t hash = FnvOffset;
            for (const char value : content) {
                hash ^= static_cast<unsigned char>(value);
                hash *= FnvPrime;
            }
            return hash;
        }

        std::string cleanLuaError(std::string message) {
            const std::size_t lineEnd = message.find_first_of("\r\n");
            if (lineEnd != std::string::npos) {
                message.resize(lineEnd);
            }
            const std::size_t detail = message.rfind(": ");
            if (detail != std::string::npos) {
                message.erase(0, detail + 2);
            }
            return message;
        }

        std::string luaValueToString(const sol::object& value) {
            switch (value.get_type()) {
            case sol::type::lua_nil:
                return "nil";
            case sol::type::boolean:
                return value.as<bool>() ? "true" : "false";
            case sol::type::number: {
                std::ostringstream stream;
                stream << std::setprecision(15) << value.as<double>();
                return stream.str();
            }
            case sol::type::string:
                return value.as<std::string>();
            case sol::type::table:
                return "<table>";
            case sol::type::function:
                return "<function>";
            case sol::type::userdata:
            case sol::type::lightuserdata:
                return "<userdata>";
            case sol::type::thread:
                return "<thread>";
            case sol::type::poly:
            case sol::type::none:
                return "<none>";
            }
            return "<value>";
        }

        int quietExceptionHandler(lua_State* state, sol::optional<const std::exception&>, sol::string_view message) {
            return sol::stack::push(state, message);
        }

    } // namespace

    struct ScriptRuntime::Impl : std::enable_shared_from_this<ScriptRuntime::Impl> {
        struct FileData {
            std::string content;
            std::uint64_t hash = 0;
        };

        struct LoadedScript {
            sol::environment environment;
            std::optional<sol::protected_function> onSpawn;
            std::optional<sol::protected_function> onTick;
            std::optional<sol::protected_function> onDestroy;
            std::uint64_t hash = 0;
        };

        struct FileOutcome {
            std::optional<FileData> data;
            ScriptResult result;
        };

        struct LoadOutcome {
            std::optional<LoadedScript> script;
            ScriptResult result;
        };

        struct InvocationContext {
            ScriptPhase phase = ScriptPhase::Load;
            ScriptHandle script;
            std::filesystem::path source;
            std::uint64_t token = 0;
        };

        struct ContextGuard {
            Impl& runtime;

            ContextGuard(Impl& runtimeValue, InvocationContext context) : runtime(runtimeValue) {
                runtime.contexts.push_back(std::move(context));
                ++runtime.busyDepth;
            }

            ~ContextGuard() {
                runtime.contexts.pop_back();
                --runtime.busyDepth;
            }

            ContextGuard(const ContextGuard&) = delete;
            ContextGuard& operator=(const ContextGuard&) = delete;
        };

        struct ActorProxy;
        struct LevelProxy;
        class ScriptActor;

        explicit Impl(ScriptRuntimeOptions optionsValue);

        ScriptSpawnResult spawnExternal(scene::Level& level, const std::filesystem::path& source);
        ScriptSpawnResult spawnInternal(scene::Level& level, const std::filesystem::path& source,
                                        const std::filesystem::path& baseDirectory);
        ScriptResult reloadExternal(ScriptHandle handle);
        ScriptResult reloadOne(ScriptHandle handle);
        std::vector<ScriptReloadResult> reloadChangedExternal();
        ScriptResult executeExternal(std::string_view source, std::string_view chunkName);
        std::optional<ScriptInfo> scriptInfo(ScriptHandle handle) const;
        std::vector<ScriptInfo> scriptInfos() const;
        std::vector<ScriptDiagnostic> diagnosticSnapshot(std::uint64_t afterSequence) const;
        std::vector<ConsoleHistoryEntry> historySnapshot(std::uint64_t afterSequence) const;
        void clearDiagnosticEntries();
        void clearHistoryEntries();

        void initializeLua();
        sol::environment makeEnvironment();
        std::filesystem::path normalizeRoot(const std::filesystem::path& configuredRoot) const;
        std::optional<std::filesystem::path> resolveSource(const std::filesystem::path& source,
                                                           const std::filesystem::path& baseDirectory,
                                                           ScriptPhase phase, ScriptHandle handle,
                                                           ScriptResult& result);
        FileOutcome readFile(const std::filesystem::path& source, ScriptPhase phase, ScriptHandle handle);
        LoadOutcome loadScript(const std::filesystem::path& source, ScriptPhase phase, ScriptHandle handle);
        LoadOutcome loadScript(const std::filesystem::path& source, FileData file, ScriptPhase phase,
                               ScriptHandle handle);
        ScriptResult success(std::vector<std::string> values = {}) const;
        ScriptResult failure(ScriptError error, bool emit = true);
        ScriptResult busyFailure(ScriptPhase phase, ScriptHandle handle = {},
                                 const std::filesystem::path& source = {}) const;
        ScriptResult wrongThreadFailure(ScriptPhase phase, ScriptHandle handle = {},
                                        const std::filesystem::path& source = {}) const;
        ScriptError makeError(ScriptErrorCode code, ScriptPhase phase, ScriptHandle handle,
                              const std::filesystem::path& source, std::string message) const;
        ScriptError luaError(ScriptPhase phase, ScriptHandle handle, const std::filesystem::path& source,
                             const sol::error& error) const;
        void emitDiagnostic(ScriptSeverity severity, ScriptPhase phase, ScriptHandle handle,
                            const std::filesystem::path& source, std::string message);
        void emitLog(ScriptSeverity severity, std::string message);
        void recordHistory(std::string command, const ScriptResult& result);
        std::uint64_t allocateSequence(std::uint64_t& next, std::string_view name) const;
        ScriptHandle allocateHandle();
        std::uint64_t allocateInvocation();
        void registerActor(ScriptHandle handle, ScriptActor* actor);
        void unregisterActor(ScriptHandle handle, const ScriptActor* actor) noexcept;
        [[nodiscard]] bool onOwnerThread() const noexcept;
        void requireOwnerThread() const;
        [[nodiscard]] bool busy() const noexcept;
        [[nodiscard]] bool invocationActive(std::uint64_t token) const noexcept;
        [[nodiscard]] const InvocationContext* currentContext() const noexcept;

        ScriptRuntimeOptions options;
        std::thread::id ownerThread;
        std::filesystem::path root;
        sol::state lua;
        sol::environment consoleEnvironment;
        std::map<std::uint64_t, ScriptActor*> actors;
        std::deque<ScriptDiagnostic> diagnosticEntries;
        std::deque<ConsoleHistoryEntry> historyEntries;
        std::vector<InvocationContext> contexts;
        std::uint64_t nextScriptHandle = 1;
        std::uint64_t nextInvocation = 1;
        std::uint64_t nextDiagnosticSequence = 1;
        std::uint64_t nextHistorySequence = 1;
        std::size_t busyDepth = 0;
    };

    struct ScriptRuntime::Impl::ActorProxy {
        std::weak_ptr<Impl> runtime;
        std::uint64_t token = 0;
        scene::Actor* actor = nullptr;
        scene::Level* level = nullptr;

        [[nodiscard]] scene::Actor& requireActor() const;
        [[nodiscard]] scene::Level& requireLevel() const;
        [[nodiscard]] scene::ActorHandle handle() const;
        [[nodiscard]] bool isAlive() const;
        [[nodiscard]] bool isPendingDestroy() const;
        bool destroy() const;
        [[nodiscard]] std::tuple<float, float, float> position() const;
        [[nodiscard]] std::tuple<float, float, float> rotation() const;
        [[nodiscard]] std::tuple<float, float, float> scale() const;
        void setPosition(float x, float y, float z) const;
        void setRotation(float x, float y, float z) const;
        void setScale(float x, float y, float z) const;
        void translate(float x, float y, float z) const;
    };

    struct ScriptRuntime::Impl::LevelProxy {
        std::weak_ptr<Impl> runtime;
        std::uint64_t token = 0;
        scene::Level* level = nullptr;
        std::filesystem::path sourceDirectory;

        [[nodiscard]] std::shared_ptr<Impl> requireRuntime() const;
        [[nodiscard]] scene::Level& requireLevel() const;
        [[nodiscard]] std::tuple<std::optional<scene::ActorHandle>, std::optional<std::string>>
        spawnScript(const std::string& source) const;
        bool destroyActor(scene::ActorHandle handle) const;
        [[nodiscard]] bool isActorAlive(scene::ActorHandle handle) const;
        [[nodiscard]] std::size_t actorCount() const;
    };

    class ScriptRuntime::Impl::ScriptActor final : public scene::Actor {
    public:
        ScriptActor(std::shared_ptr<Impl> runtime, ScriptHandle scriptHandle, std::filesystem::path source,
                    LoadedScript script)
            : runtime_(std::move(runtime)), scriptHandle_(scriptHandle), source_(std::move(source)),
              script_(std::move(script)) {
        }

        ~ScriptActor() override {
            runtime_->unregisterActor(scriptHandle_, this);
        }

        void bindActorHandle(scene::ActorHandle actorHandle) noexcept {
            actorHandle_ = actorHandle;
        }

        [[nodiscard]] ScriptInfo info() const {
            return ScriptInfo{
                .handle = scriptHandle_,
                .actor = actorHandle_,
                .source = source_,
                .state = state_,
                .revision = revision_,
                .lastError = lastError_,
            };
        }

        [[nodiscard]] std::uint64_t contentHash() const noexcept {
            return script_.hash;
        }

        [[nodiscard]] bool reloadable() const noexcept {
            return state_ != ScriptState::PendingDestroy;
        }

        void recordReloadFailure(const ScriptError& error) {
            lastError_ = error;
        }

        void replace(LoadedScript script) {
            script_ = std::move(script);
            ++revision_;
            lastError_.reset();
            if (state_ != ScriptState::PendingSpawn) {
                state_ = ScriptState::Running;
            }
        }

        void onSpawn(scene::Level& level) override {
            actorHandle_ = handle();
            state_ = ScriptState::Running;
            if (const auto error = invoke(script_.onSpawn, ScriptPhase::Spawn, level, 0.0f); error.has_value()) {
                state_ = ScriptState::Faulted;
                lastError_ = *error;
            }
        }

        void tick(scene::Level& level, float deltaSeconds) override {
            if (state_ != ScriptState::Running) {
                return;
            }
            if (const auto error = invoke(script_.onTick, ScriptPhase::Tick, level, deltaSeconds); error.has_value()) {
                state_ = ScriptState::Faulted;
                lastError_ = *error;
            }
        }

        void onDestroy(scene::Level& level) override {
            state_ = ScriptState::PendingDestroy;
            if (const auto error = invoke(script_.onDestroy, ScriptPhase::Destroy, level, 0.0f); error.has_value()) {
                lastError_ = *error;
            }
        }

    private:
        std::optional<ScriptError> invoke(const std::optional<sol::protected_function>& callback, ScriptPhase phase,
                                          scene::Level& level, float deltaSeconds) {
            if (!callback.has_value()) {
                return std::nullopt;
            }
            const std::uint64_t token = runtime_->allocateInvocation();
            ContextGuard context{*runtime_, InvocationContext{phase, scriptHandle_, source_, token}};
            ActorProxy actorProxy{runtime_, token, this, &level};
            LevelProxy levelProxy{runtime_, token, &level, source_.parent_path()};
            sol::protected_function_result result;
            if (phase == ScriptPhase::Tick) {
                result = (*callback)(std::move(actorProxy), std::move(levelProxy), deltaSeconds);
            } else {
                result = (*callback)(std::move(actorProxy), std::move(levelProxy));
            }
            if (result.valid()) {
                return std::nullopt;
            }
            const sol::error luaFailure = result;
            ScriptError error = runtime_->luaError(phase, scriptHandle_, source_, luaFailure);
            runtime_->emitDiagnostic(ScriptSeverity::Error, error.phase, error.script, error.source, error.message);
            return error;
        }

        std::shared_ptr<Impl> runtime_;
        ScriptHandle scriptHandle_;
        scene::ActorHandle actorHandle_;
        std::filesystem::path source_;
        LoadedScript script_;
        ScriptState state_ = ScriptState::PendingSpawn;
        std::uint64_t revision_ = 1;
        std::optional<ScriptError> lastError_;
    };

    ScriptRuntime::Impl::Impl(ScriptRuntimeOptions optionsValue)
        : options(std::move(optionsValue)), ownerThread(std::this_thread::get_id()),
          root(normalizeRoot(options.scriptRoot)) {
        initializeLua();
    }

    void ScriptRuntime::Impl::initializeLua() {
        lua.set_exception_handler(&quietExceptionHandler);
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::utf8);

        lua.new_usertype<scene::ActorHandle>("ActorHandle", sol::no_constructor, "index",
                                             sol::property([](const scene::ActorHandle& handle) {
                                                 return handle.index;
                                             }),
                                             "generation", sol::property([](const scene::ActorHandle& handle) {
                                                 return handle.generation;
                                             }),
                                             "is_valid", &scene::ActorHandle::isValid);
        lua.new_usertype<ActorProxy>("Actor", sol::no_constructor, "handle", &ActorProxy::handle, "is_alive",
                                     &ActorProxy::isAlive, "is_pending_destroy", &ActorProxy::isPendingDestroy,
                                     "destroy", &ActorProxy::destroy, "get_position", &ActorProxy::position,
                                     "get_rotation", &ActorProxy::rotation, "get_scale", &ActorProxy::scale,
                                     "set_position", &ActorProxy::setPosition, "set_rotation", &ActorProxy::setRotation,
                                     "set_scale", &ActorProxy::setScale, "translate", &ActorProxy::translate);
        lua.new_usertype<LevelProxy>("Level", sol::no_constructor, "spawn_script", &LevelProxy::spawnScript,
                                     "destroy_actor", &LevelProxy::destroyActor, "is_actor_alive",
                                     &LevelProxy::isActorAlive, "actor_count", &LevelProxy::actorCount);

        consoleEnvironment = makeEnvironment();
    }

    sol::environment ScriptRuntime::Impl::makeEnvironment() {
        sol::environment environment{lua, sol::create};
        const sol::table globals = lua.globals();
        constexpr std::array<std::string_view, 14> BaseNames{
            "_VERSION", "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget", "rawlen",
            "select",   "tonumber", "tostring", "type",
        };
        for (const std::string_view name : BaseNames) {
            environment[std::string{name}] = globals[std::string{name}];
        }
        constexpr std::array<std::string_view, 4> LibraryNames{"math", "string", "table", "utf8"};
        for (const std::string_view name : LibraryNames) {
            const sol::table source = globals[std::string{name}];
            sol::table copy = lua.create_table();
            for (const auto& [key, value] : source) {
                copy[key] = value;
            }
            environment[std::string{name}] = std::move(copy);
        }

        sol::table log = lua.create_table();
        log.set_function("info", [this](std::string message) {
            emitLog(ScriptSeverity::Info, std::move(message));
        });
        log.set_function("warn", [this](std::string message) {
            emitLog(ScriptSeverity::Warning, std::move(message));
        });
        log.set_function("error", [this](std::string message) {
            emitLog(ScriptSeverity::Error, std::move(message));
        });
        environment["log"] = std::move(log);
        environment.set_function("print", [this](sol::variadic_args arguments) {
            std::string message;
            for (const auto argument : arguments) {
                if (!message.empty()) {
                    message.push_back('\t');
                }
                message += luaValueToString(sol::object{arguments.lua_state(), argument.stack_index()});
            }
            emitLog(ScriptSeverity::Info, std::move(message));
        });
        environment["_G"] = environment;
        return environment;
    }

    std::filesystem::path ScriptRuntime::Impl::normalizeRoot(const std::filesystem::path& configuredRoot) const {
        std::error_code error;
        std::filesystem::path path = configuredRoot.empty() ? std::filesystem::current_path(error) : configuredRoot;
        if (error) {
            throw std::runtime_error("Failed to resolve the scripting root directory.");
        }
        path = std::filesystem::absolute(path, error);
        if (error) {
            throw std::runtime_error("Failed to make the scripting root directory absolute.");
        }
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
        return error ? path.lexically_normal() : canonical;
    }

    std::optional<std::filesystem::path> ScriptRuntime::Impl::resolveSource(const std::filesystem::path& source,
                                                                            const std::filesystem::path& baseDirectory,
                                                                            ScriptPhase phase, ScriptHandle handle,
                                                                            ScriptResult& result) {
        if (source.empty()) {
            result = failure(
                makeError(ScriptErrorCode::FileNotFound, phase, handle, source, "Script source path cannot be empty."));
            return std::nullopt;
        }

        std::error_code error;
        std::filesystem::path candidate = source.is_absolute() ? source : baseDirectory / source;
        candidate = std::filesystem::absolute(candidate, error);
        if (error) {
            result = failure(makeError(ScriptErrorCode::Io, phase, handle, source,
                                       "Failed to make the script source path absolute."));
            return std::nullopt;
        }
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, error);
        if (!error) {
            candidate = canonical;
        } else {
            candidate = candidate.lexically_normal();
            error.clear();
        }

        const std::filesystem::path relative = candidate.lexically_relative(root);
        const bool outside = relative.empty() || relative.is_absolute() ||
                             (!relative.empty() && *relative.begin() == std::filesystem::path{".."});
        if (outside) {
            result = failure(makeError(ScriptErrorCode::PathOutsideRoot, phase, handle, candidate,
                                       "Script source path escapes the configured script root."));
            return std::nullopt;
        }
        return candidate;
    }

    ScriptRuntime::Impl::FileOutcome ScriptRuntime::Impl::readFile(const std::filesystem::path& source,
                                                                   ScriptPhase phase, ScriptHandle handle) {
        std::error_code filesystemError;
        const bool exists = std::filesystem::exists(source, filesystemError);
        if (filesystemError) {
            return FileOutcome{
                .data = std::nullopt,
                .result = failure(
                    makeError(ScriptErrorCode::Io, phase, handle, source, "Failed to inspect the script source file.")),
            };
        }
        if (!exists) {
            return FileOutcome{
                .data = std::nullopt,
                .result = failure(makeError(ScriptErrorCode::FileNotFound, phase, handle, source,
                                            "Script source file does not exist.")),
            };
        }

        std::ifstream stream(source, std::ios::binary);
        if (!stream.is_open()) {
            return FileOutcome{
                .data = std::nullopt,
                .result = failure(
                    makeError(ScriptErrorCode::Io, phase, handle, source, "Failed to open the script source file.")),
            };
        }
        std::string content{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        if (stream.bad()) {
            return FileOutcome{
                .data = std::nullopt,
                .result = failure(makeError(ScriptErrorCode::Io, phase, handle, source,
                                            "Failed while reading the script source file.")),
            };
        }
        return FileOutcome{
            .data = FileData{.content = std::move(content), .hash = 0},
            .result = success(),
        };
    }

    ScriptRuntime::Impl::LoadOutcome ScriptRuntime::Impl::loadScript(const std::filesystem::path& source,
                                                                     ScriptPhase phase, ScriptHandle handle) {
        FileOutcome file = readFile(source, phase, handle);
        if (!file.data.has_value()) {
            return LoadOutcome{.script = std::nullopt, .result = std::move(file.result)};
        }
        return loadScript(source, std::move(*file.data), phase, handle);
    }

    ScriptRuntime::Impl::LoadOutcome ScriptRuntime::Impl::loadScript(const std::filesystem::path& source, FileData file,
                                                                     ScriptPhase phase, ScriptHandle handle) {
        file.hash = contentHash(file.content);
        ContextGuard context{*this, InvocationContext{phase, handle, source, 0}};
        sol::environment environment = makeEnvironment();
        const std::string chunkName = "@" + source.generic_string();
        sol::load_result loadedChunk = lua.load_buffer(file.content.data(), file.content.size(), chunkName);
        if (!loadedChunk.valid()) {
            const sol::error luaFailure = loadedChunk;
            ScriptError error =
                makeError(ScriptErrorCode::Syntax, phase, handle, source, cleanLuaError(luaFailure.what()));
            return LoadOutcome{.script = std::nullopt, .result = failure(std::move(error))};
        }

        sol::protected_function chunk = loadedChunk;
        sol::set_environment(environment, chunk);
        sol::protected_function_result chunkResult = chunk();
        if (!chunkResult.valid()) {
            const sol::error luaFailure = chunkResult;
            return LoadOutcome{
                .script = std::nullopt,
                .result = failure(luaError(phase, handle, source, luaFailure)),
            };
        }
        if (chunkResult.return_count() != 1 || chunkResult.get_type() != sol::type::table) {
            return LoadOutcome{
                .script = std::nullopt,
                .result = failure(makeError(ScriptErrorCode::Contract, phase, handle, source,
                                            "Script chunk must return exactly one callback table.")),
            };
        }

        sol::table callbacks = chunkResult.get<sol::table>();
        LoadedScript candidate{
            .environment = std::move(environment),
            .onSpawn = std::nullopt,
            .onTick = std::nullopt,
            .onDestroy = std::nullopt,
            .hash = file.hash,
        };
        const auto assignCallback = [&](std::string_view name,
                                        std::optional<sol::protected_function>& destination) -> ScriptResult {
            const sol::object callback = callbacks.raw_get<sol::object>(std::string{name});
            if (!callback.valid() || callback.get_type() == sol::type::none ||
                callback.get_type() == sol::type::lua_nil) {
                return success();
            }
            if (callback.get_type() != sol::type::function) {
                return failure(makeError(ScriptErrorCode::Contract, phase, handle, source,
                                         "Script callback '" + std::string{name} + "' must be a function or nil."));
            }
            destination = callback.as<sol::protected_function>();
            return success();
        };

        ScriptResult callbackResult = assignCallback("on_spawn", candidate.onSpawn);
        if (!callbackResult.succeeded) {
            return LoadOutcome{.script = std::nullopt, .result = std::move(callbackResult)};
        }
        callbackResult = assignCallback("on_tick", candidate.onTick);
        if (!callbackResult.succeeded) {
            return LoadOutcome{.script = std::nullopt, .result = std::move(callbackResult)};
        }
        callbackResult = assignCallback("on_destroy", candidate.onDestroy);
        if (!callbackResult.succeeded) {
            return LoadOutcome{.script = std::nullopt, .result = std::move(callbackResult)};
        }
        return LoadOutcome{.script = std::move(candidate), .result = success()};
    }

    ScriptSpawnResult ScriptRuntime::Impl::spawnExternal(scene::Level& level, const std::filesystem::path& source) {
        if (!onOwnerThread()) {
            return ScriptSpawnResult{
                .result = wrongThreadFailure(ScriptPhase::Load, {}, source),
                .script = {},
                .actor = {},
            };
        }
        if (busy()) {
            return ScriptSpawnResult{
                .result = busyFailure(ScriptPhase::Load, {}, source),
                .script = {},
                .actor = {},
            };
        }
        ContextGuard context{*this, InvocationContext{ScriptPhase::Load, {}, source, 0}};
        return spawnInternal(level, source, root);
    }

    ScriptSpawnResult ScriptRuntime::Impl::spawnInternal(scene::Level& level, const std::filesystem::path& source,
                                                         const std::filesystem::path& baseDirectory) {
        ScriptResult resolution;
        const auto resolved = resolveSource(source, baseDirectory, ScriptPhase::Load, {}, resolution);
        if (!resolved.has_value()) {
            return ScriptSpawnResult{.result = std::move(resolution), .script = {}, .actor = {}};
        }
        LoadOutcome loaded = loadScript(*resolved, ScriptPhase::Load, {});
        if (!loaded.script.has_value()) {
            return ScriptSpawnResult{.result = std::move(loaded.result), .script = {}, .actor = {}};
        }

        const ScriptHandle scriptHandle = allocateHandle();
        auto actor =
            std::make_unique<ScriptActor>(shared_from_this(), scriptHandle, *resolved, std::move(*loaded.script));
        ScriptActor* actorPointer = actor.get();
        registerActor(scriptHandle, actorPointer);
        const scene::ActorHandle actorHandle = level.spawnActor(std::move(actor));
        const auto registered = actors.find(scriptHandle.value);
        if (registered != actors.end() && registered->second == actorPointer) {
            actorPointer->bindActorHandle(actorHandle);
        }
        return ScriptSpawnResult{
            .result = success(),
            .script = scriptHandle,
            .actor = actorHandle,
        };
    }

    ScriptResult ScriptRuntime::Impl::reloadExternal(ScriptHandle handle) {
        if (!onOwnerThread()) {
            return wrongThreadFailure(ScriptPhase::Reload, handle);
        }
        if (busy()) {
            return busyFailure(ScriptPhase::Reload, handle);
        }
        ContextGuard context{*this, InvocationContext{ScriptPhase::Reload, handle, {}, 0}};
        return reloadOne(handle);
    }

    ScriptResult ScriptRuntime::Impl::reloadOne(ScriptHandle handle) {
        const auto actorIterator = actors.find(handle.value);
        if (!handle.isValid() || actorIterator == actors.end() || !actorIterator->second->reloadable()) {
            return failure(makeError(ScriptErrorCode::InvalidHandle, ScriptPhase::Reload, handle, {},
                                     "ScriptHandle is stale or pending destruction."));
        }
        ScriptActor& actor = *actorIterator->second;
        const ScriptInfo before = actor.info();
        LoadOutcome loaded = loadScript(before.source, ScriptPhase::Reload, handle);
        if (!loaded.script.has_value()) {
            if (loaded.result.error.has_value()) {
                actor.recordReloadFailure(*loaded.result.error);
            }
            return std::move(loaded.result);
        }
        actor.replace(std::move(*loaded.script));
        return success();
    }

    std::vector<ScriptReloadResult> ScriptRuntime::Impl::reloadChangedExternal() {
        if (!onOwnerThread()) {
            return {{.script = {}, .result = wrongThreadFailure(ScriptPhase::Reload)}};
        }
        if (busy()) {
            return {{.script = {}, .result = busyFailure(ScriptPhase::Reload)}};
        }
        ContextGuard context{*this, InvocationContext{ScriptPhase::Reload, {}, {}, 0}};
        std::vector<ScriptReloadResult> results;
        for (const auto& [value, actorPointer] : actors) {
            const ScriptHandle handle{value};
            if (!actorPointer->reloadable()) {
                continue;
            }
            const ScriptInfo info = actorPointer->info();
            FileOutcome file = readFile(info.source, ScriptPhase::Reload, handle);
            if (!file.data.has_value()) {
                if (file.result.error.has_value()) {
                    actorPointer->recordReloadFailure(*file.result.error);
                }
                results.push_back(ScriptReloadResult{handle, std::move(file.result)});
                continue;
            }
            file.data->hash = contentHash(file.data->content);
            if (file.data->hash == actorPointer->contentHash()) {
                continue;
            }
            LoadOutcome loaded = loadScript(info.source, std::move(*file.data), ScriptPhase::Reload, handle);
            if (!loaded.script.has_value()) {
                if (loaded.result.error.has_value()) {
                    actorPointer->recordReloadFailure(*loaded.result.error);
                }
                results.push_back(ScriptReloadResult{handle, std::move(loaded.result)});
                continue;
            }
            actorPointer->replace(std::move(*loaded.script));
            results.push_back(ScriptReloadResult{handle, success()});
        }
        return results;
    }

    ScriptResult ScriptRuntime::Impl::executeExternal(std::string_view source, std::string_view chunkName) {
        if (!onOwnerThread()) {
            return wrongThreadFailure(ScriptPhase::Console, {}, std::filesystem::path{chunkName});
        }
        if (busy()) {
            ScriptResult result = busyFailure(ScriptPhase::Console, {}, std::filesystem::path{chunkName});
            recordHistory(std::string{source}, result);
            return result;
        }

        const std::filesystem::path diagnosticSource{chunkName};
        ContextGuard context{*this, InvocationContext{ScriptPhase::Console, {}, diagnosticSource, 0}};
        sol::load_result loadedChunk = lua.load_buffer(source.data(), source.size(), std::string{chunkName});
        if (!loadedChunk.valid()) {
            const sol::error luaFailure = loadedChunk;
            ScriptResult result = failure(makeError(ScriptErrorCode::Syntax, ScriptPhase::Console, {}, diagnosticSource,
                                                    cleanLuaError(luaFailure.what())));
            recordHistory(std::string{source}, result);
            return result;
        }
        sol::protected_function chunk = loadedChunk;
        sol::set_environment(consoleEnvironment, chunk);
        sol::protected_function_result execution = chunk();
        if (!execution.valid()) {
            const sol::error luaFailure = execution;
            ScriptResult result = failure(luaError(ScriptPhase::Console, {}, diagnosticSource, luaFailure));
            recordHistory(std::string{source}, result);
            return result;
        }

        std::vector<std::string> values;
        values.reserve(static_cast<std::size_t>(execution.return_count()));
        for (int index = 0; index < execution.return_count(); ++index) {
            values.push_back(luaValueToString(execution.get<sol::object>(index)));
        }
        ScriptResult result = success(std::move(values));
        recordHistory(std::string{source}, result);
        return result;
    }

    std::optional<ScriptInfo> ScriptRuntime::Impl::scriptInfo(ScriptHandle handle) const {
        requireOwnerThread();
        const auto iterator = actors.find(handle.value);
        if (!handle.isValid() || iterator == actors.end()) {
            return std::nullopt;
        }
        return iterator->second->info();
    }

    std::vector<ScriptInfo> ScriptRuntime::Impl::scriptInfos() const {
        requireOwnerThread();
        std::vector<ScriptInfo> result;
        result.reserve(actors.size());
        for (const auto& [value, actor] : actors) {
            static_cast<void>(value);
            result.push_back(actor->info());
        }
        return result;
    }

    std::vector<ScriptDiagnostic> ScriptRuntime::Impl::diagnosticSnapshot(std::uint64_t afterSequence) const {
        requireOwnerThread();
        std::vector<ScriptDiagnostic> result;
        for (const ScriptDiagnostic& diagnostic : diagnosticEntries) {
            if (diagnostic.sequence > afterSequence) {
                result.push_back(diagnostic);
            }
        }
        return result;
    }

    std::vector<ConsoleHistoryEntry> ScriptRuntime::Impl::historySnapshot(std::uint64_t afterSequence) const {
        requireOwnerThread();
        std::vector<ConsoleHistoryEntry> result;
        for (const ConsoleHistoryEntry& entry : historyEntries) {
            if (entry.sequence > afterSequence) {
                result.push_back(entry);
            }
        }
        return result;
    }

    void ScriptRuntime::Impl::clearDiagnosticEntries() {
        requireOwnerThread();
        diagnosticEntries.clear();
    }

    void ScriptRuntime::Impl::clearHistoryEntries() {
        requireOwnerThread();
        historyEntries.clear();
    }

    ScriptResult ScriptRuntime::Impl::success(std::vector<std::string> values) const {
        return ScriptResult{.succeeded = true, .error = std::nullopt, .values = std::move(values)};
    }

    ScriptResult ScriptRuntime::Impl::failure(ScriptError error, bool emit) {
        if (emit) {
            emitDiagnostic(ScriptSeverity::Error, error.phase, error.script, error.source, error.message);
        }
        return ScriptResult{.succeeded = false, .error = std::move(error), .values = {}};
    }

    ScriptResult ScriptRuntime::Impl::busyFailure(ScriptPhase phase, ScriptHandle handle,
                                                  const std::filesystem::path& source) const {
        return ScriptResult{
            .succeeded = false,
            .error = makeError(ScriptErrorCode::Busy, phase, handle, source,
                               "Script runtime is already executing an operation."),
            .values = {},
        };
    }

    ScriptResult ScriptRuntime::Impl::wrongThreadFailure(ScriptPhase phase, ScriptHandle handle,
                                                         const std::filesystem::path& source) const {
        return ScriptResult{
            .succeeded = false,
            .error = makeError(ScriptErrorCode::WrongThread, phase, handle, source,
                               "Script runtime can only be used from its owning thread."),
            .values = {},
        };
    }

    ScriptError ScriptRuntime::Impl::makeError(ScriptErrorCode code, ScriptPhase phase, ScriptHandle handle,
                                               const std::filesystem::path& source, std::string message) const {
        return ScriptError{code, phase, handle, source, std::move(message)};
    }

    ScriptError ScriptRuntime::Impl::luaError(ScriptPhase phase, ScriptHandle handle,
                                              const std::filesystem::path& source, const sol::error& error) const {
        return makeError(ScriptErrorCode::Runtime, phase, handle, source, cleanLuaError(error.what()));
    }

    void ScriptRuntime::Impl::emitDiagnostic(ScriptSeverity severity, ScriptPhase phase, ScriptHandle handle,
                                             const std::filesystem::path& source, std::string message) {
        ScriptDiagnostic diagnostic{
            .sequence = allocateSequence(nextDiagnosticSequence, "diagnostic"),
            .timestamp = std::chrono::system_clock::now(),
            .severity = severity,
            .phase = phase,
            .script = handle,
            .source = source,
            .message = std::move(message),
        };
        if (options.diagnosticCapacity != 0) {
            if (diagnosticEntries.size() >= options.diagnosticCapacity) {
                diagnosticEntries.pop_front();
            }
            diagnosticEntries.push_back(diagnostic);
        }
        if (options.diagnosticSink) {
            try {
                options.diagnosticSink(diagnostic);
            } catch (...) {
            }
        }
    }

    void ScriptRuntime::Impl::emitLog(ScriptSeverity severity, std::string message) {
        const InvocationContext* context = currentContext();
        if (context == nullptr) {
            emitDiagnostic(severity, ScriptPhase::Console, {}, {}, std::move(message));
            return;
        }
        emitDiagnostic(severity, context->phase, context->script, context->source, std::move(message));
    }

    void ScriptRuntime::Impl::recordHistory(std::string command, const ScriptResult& result) {
        ConsoleHistoryEntry entry{
            .sequence = allocateSequence(nextHistorySequence, "console history"),
            .timestamp = std::chrono::system_clock::now(),
            .command = std::move(command),
            .result = result,
        };
        if (options.consoleHistoryCapacity == 0) {
            return;
        }
        if (historyEntries.size() >= options.consoleHistoryCapacity) {
            historyEntries.pop_front();
        }
        historyEntries.push_back(std::move(entry));
    }

    std::uint64_t ScriptRuntime::Impl::allocateSequence(std::uint64_t& next, std::string_view name) const {
        if (next == 0) {
            throw std::overflow_error(std::string{name} + " sequence exhausted.");
        }
        return next++;
    }

    ScriptHandle ScriptRuntime::Impl::allocateHandle() {
        return ScriptHandle{allocateSequence(nextScriptHandle, "script handle")};
    }

    std::uint64_t ScriptRuntime::Impl::allocateInvocation() {
        return allocateSequence(nextInvocation, "script invocation");
    }

    void ScriptRuntime::Impl::registerActor(ScriptHandle handle, ScriptActor* actor) {
        if (!actors.emplace(handle.value, actor).second) {
            throw std::logic_error("ScriptHandle collision detected.");
        }
    }

    void ScriptRuntime::Impl::unregisterActor(ScriptHandle handle, const ScriptActor* actor) noexcept {
        const auto iterator = actors.find(handle.value);
        if (iterator != actors.end() && iterator->second == actor) {
            actors.erase(iterator);
        }
    }

    bool ScriptRuntime::Impl::onOwnerThread() const noexcept {
        return std::this_thread::get_id() == ownerThread;
    }

    void ScriptRuntime::Impl::requireOwnerThread() const {
        if (!onOwnerThread()) {
            throw std::logic_error("ScriptRuntime observer called from a non-owning thread.");
        }
    }

    bool ScriptRuntime::Impl::busy() const noexcept {
        return busyDepth != 0;
    }

    bool ScriptRuntime::Impl::invocationActive(std::uint64_t token) const noexcept {
        const InvocationContext* context = currentContext();
        return token != 0 && context != nullptr && context->token == token;
    }

    const ScriptRuntime::Impl::InvocationContext* ScriptRuntime::Impl::currentContext() const noexcept {
        return contexts.empty() ? nullptr : &contexts.back();
    }

    scene::Actor& ScriptRuntime::Impl::ActorProxy::requireActor() const {
        const auto owner = runtime.lock();
        if (owner == nullptr || !owner->invocationActive(token) || actor == nullptr) {
            throw sol::error("Actor proxy expired.");
        }
        return *actor;
    }

    scene::Level& ScriptRuntime::Impl::ActorProxy::requireLevel() const {
        const auto owner = runtime.lock();
        if (owner == nullptr || !owner->invocationActive(token) || level == nullptr) {
            throw sol::error("Actor proxy expired.");
        }
        return *level;
    }

    scene::ActorHandle ScriptRuntime::Impl::ActorProxy::handle() const {
        return requireActor().handle();
    }

    bool ScriptRuntime::Impl::ActorProxy::isAlive() const {
        return requireLevel().isActorAlive(requireActor().handle());
    }

    bool ScriptRuntime::Impl::ActorProxy::isPendingDestroy() const {
        return requireActor().isPendingDestroy();
    }

    bool ScriptRuntime::Impl::ActorProxy::destroy() const {
        return requireLevel().destroyActor(requireActor().handle());
    }

    std::tuple<float, float, float> ScriptRuntime::Impl::ActorProxy::position() const {
        const glm::vec3 value = requireActor().transform().position;
        return {value.x, value.y, value.z};
    }

    std::tuple<float, float, float> ScriptRuntime::Impl::ActorProxy::rotation() const {
        const glm::vec3 value = requireActor().transform().rotationDegrees;
        return {value.x, value.y, value.z};
    }

    std::tuple<float, float, float> ScriptRuntime::Impl::ActorProxy::scale() const {
        const glm::vec3 value = requireActor().transform().scale;
        return {value.x, value.y, value.z};
    }

    void ScriptRuntime::Impl::ActorProxy::setPosition(float x, float y, float z) const {
        scene::Actor& value = requireActor();
        scene::Transform transform = value.transform();
        transform.position = {x, y, z};
        value.setTransform(transform);
    }

    void ScriptRuntime::Impl::ActorProxy::setRotation(float x, float y, float z) const {
        scene::Actor& value = requireActor();
        scene::Transform transform = value.transform();
        transform.rotationDegrees = {x, y, z};
        value.setTransform(transform);
    }

    void ScriptRuntime::Impl::ActorProxy::setScale(float x, float y, float z) const {
        scene::Actor& value = requireActor();
        scene::Transform transform = value.transform();
        transform.scale = {x, y, z};
        value.setTransform(transform);
    }

    void ScriptRuntime::Impl::ActorProxy::translate(float x, float y, float z) const {
        requireActor().translate({x, y, z});
    }

    std::shared_ptr<ScriptRuntime::Impl> ScriptRuntime::Impl::LevelProxy::requireRuntime() const {
        const auto owner = runtime.lock();
        if (owner == nullptr || !owner->invocationActive(token)) {
            throw sol::error("Level proxy expired.");
        }
        return owner;
    }

    scene::Level& ScriptRuntime::Impl::LevelProxy::requireLevel() const {
        static_cast<void>(requireRuntime());
        if (level == nullptr) {
            throw sol::error("Level proxy expired.");
        }
        return *level;
    }

    std::tuple<std::optional<scene::ActorHandle>, std::optional<std::string>>
    ScriptRuntime::Impl::LevelProxy::spawnScript(const std::string& source) const {
        const auto owner = requireRuntime();
        ScriptSpawnResult result = owner->spawnInternal(requireLevel(), std::filesystem::path{source}, sourceDirectory);
        if (!result.result.succeeded) {
            return {std::nullopt, result.result.error.has_value()
                                      ? std::optional<std::string>{result.result.error->message}
                                      : std::optional<std::string>{"Script spawn failed."}};
        }
        return {result.actor, std::nullopt};
    }

    bool ScriptRuntime::Impl::LevelProxy::destroyActor(scene::ActorHandle handle) const {
        return requireLevel().destroyActor(handle);
    }

    bool ScriptRuntime::Impl::LevelProxy::isActorAlive(scene::ActorHandle handle) const {
        return requireLevel().isActorAlive(handle);
    }

    std::size_t ScriptRuntime::Impl::LevelProxy::actorCount() const {
        return requireLevel().actorCount();
    }

    ScriptRuntime::ScriptRuntime(ScriptRuntimeOptions options) : impl_(std::make_shared<Impl>(std::move(options))) {
    }

    ScriptRuntime::~ScriptRuntime() = default;

    ScriptSpawnResult ScriptRuntime::spawn(scene::Level& level, const std::filesystem::path& source) {
        return impl_->spawnExternal(level, source);
    }

    ScriptResult ScriptRuntime::reload(ScriptHandle handle) {
        return impl_->reloadExternal(handle);
    }

    std::vector<ScriptReloadResult> ScriptRuntime::reloadChanged() {
        return impl_->reloadChangedExternal();
    }

    ScriptResult ScriptRuntime::execute(std::string_view source, std::string_view chunkName) {
        return impl_->executeExternal(source, chunkName);
    }

    std::optional<ScriptInfo> ScriptRuntime::script(ScriptHandle handle) const {
        return impl_->scriptInfo(handle);
    }

    std::vector<ScriptInfo> ScriptRuntime::scripts() const {
        return impl_->scriptInfos();
    }

    std::vector<ScriptDiagnostic> ScriptRuntime::diagnostics(std::uint64_t afterSequence) const {
        return impl_->diagnosticSnapshot(afterSequence);
    }

    void ScriptRuntime::clearDiagnostics() {
        impl_->clearDiagnosticEntries();
    }

    std::vector<ConsoleHistoryEntry> ScriptRuntime::consoleHistory(std::uint64_t afterSequence) const {
        return impl_->historySnapshot(afterSequence);
    }

    void ScriptRuntime::clearConsoleHistory() {
        impl_->clearHistoryEntries();
    }

} // namespace lumin::scripting
