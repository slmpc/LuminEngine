#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lumin/scene/Level.hpp"

namespace lumin::scripting {

    struct ScriptHandle {
        std::uint64_t value = 0;

        [[nodiscard]] bool isValid() const noexcept {
            return value != 0;
        }

        explicit operator bool() const noexcept {
            return isValid();
        }

        friend bool operator==(ScriptHandle, ScriptHandle) = default;
    };

    inline constexpr ScriptHandle InvalidScriptHandle{};

    enum class ScriptSeverity : std::uint8_t {
        Info,
        Warning,
        Error,
    };

    enum class ScriptPhase : std::uint8_t {
        Load,
        Spawn,
        Tick,
        Destroy,
        Reload,
        Console,
    };

    enum class ScriptErrorCode : std::uint8_t {
        None,
        FileNotFound,
        Io,
        Syntax,
        Contract,
        Runtime,
        Busy,
        InvalidHandle,
        PathOutsideRoot,
        WrongThread,
    };

    enum class ScriptState : std::uint8_t {
        PendingSpawn,
        Running,
        Faulted,
        PendingDestroy,
    };

    struct ScriptError {
        ScriptErrorCode code = ScriptErrorCode::None;
        ScriptPhase phase = ScriptPhase::Load;
        ScriptHandle script;
        std::filesystem::path source;
        std::string message;
    };

    struct ScriptResult {
        bool succeeded = false;
        std::optional<ScriptError> error;
        std::vector<std::string> values;

        explicit operator bool() const noexcept {
            return succeeded;
        }
    };

    struct ScriptSpawnResult {
        ScriptResult result;
        ScriptHandle script;
        scene::ActorHandle actor;

        explicit operator bool() const noexcept {
            return result.succeeded && script.isValid() && actor.isValid();
        }
    };

    struct ScriptReloadResult {
        ScriptHandle script;
        ScriptResult result;
    };

    struct ScriptInfo {
        ScriptHandle handle;
        scene::ActorHandle actor;
        std::filesystem::path source;
        ScriptState state = ScriptState::PendingSpawn;
        std::uint64_t revision = 0;
        std::optional<ScriptError> lastError;
    };

    struct ScriptDiagnostic {
        std::uint64_t sequence = 0;
        std::chrono::system_clock::time_point timestamp;
        ScriptSeverity severity = ScriptSeverity::Info;
        ScriptPhase phase = ScriptPhase::Load;
        ScriptHandle script;
        std::filesystem::path source;
        std::string message;
    };

    struct ConsoleHistoryEntry {
        std::uint64_t sequence = 0;
        std::chrono::system_clock::time_point timestamp;
        std::string command;
        ScriptResult result;
    };

    struct ScriptRuntimeOptions {
        std::filesystem::path scriptRoot;
        std::size_t diagnosticCapacity = 256;
        std::size_t consoleHistoryCapacity = 128;
        std::function<void(const ScriptDiagnostic&)> diagnosticSink;
    };

    class ScriptRuntime {
    public:
        explicit ScriptRuntime(ScriptRuntimeOptions options = {});
        ~ScriptRuntime();

        ScriptRuntime(const ScriptRuntime&) = delete;
        ScriptRuntime& operator=(const ScriptRuntime&) = delete;
        ScriptRuntime(ScriptRuntime&&) = delete;
        ScriptRuntime& operator=(ScriptRuntime&&) = delete;

        [[nodiscard]] ScriptSpawnResult spawn(scene::Level& level, const std::filesystem::path& source);
        [[nodiscard]] ScriptResult reload(ScriptHandle handle);
        [[nodiscard]] std::vector<ScriptReloadResult> reloadChanged();
        [[nodiscard]] ScriptResult execute(std::string_view source, std::string_view chunkName = "<console>");

        [[nodiscard]] std::optional<ScriptInfo> script(ScriptHandle handle) const;
        [[nodiscard]] std::vector<ScriptInfo> scripts() const;
        [[nodiscard]] std::vector<ScriptDiagnostic> diagnostics(std::uint64_t afterSequence = 0) const;
        void clearDiagnostics();
        [[nodiscard]] std::vector<ConsoleHistoryEntry> consoleHistory(std::uint64_t afterSequence = 0) const;
        void clearConsoleHistory();

    private:
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };

} // namespace lumin::scripting
