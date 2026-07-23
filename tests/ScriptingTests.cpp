#include "lumin/scripting/ScriptRuntime.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

    void require(bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error(std::string{message});
        }
    }

    class TemporaryScripts {
    public:
        TemporaryScripts() {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path() / ("lumin-scripting-tests-" + std::to_string(suffix));
            std::filesystem::create_directories(root_);
        }

        ~TemporaryScripts() {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }

        TemporaryScripts(const TemporaryScripts&) = delete;
        TemporaryScripts& operator=(const TemporaryScripts&) = delete;

        [[nodiscard]] const std::filesystem::path& root() const noexcept {
            return root_;
        }

        void write(std::string_view name, std::string_view source) const {
            std::ofstream stream(root_ / std::filesystem::path{name}, std::ios::binary | std::ios::trunc);
            require(stream.is_open(), "Test script file must open for writing.");
            stream.write(source.data(), static_cast<std::streamsize>(source.size()));
            require(stream.good(), "Test script file must be written completely.");
        }

    private:
        std::filesystem::path root_;
    };

    lumin::scripting::ScriptRuntime makeRuntime(const TemporaryScripts& scripts) {
        return lumin::scripting::ScriptRuntime{
            lumin::scripting::ScriptRuntimeOptions{.scriptRoot = scripts.root()},
        };
    }

    std::size_t countMessage(const std::vector<lumin::scripting::ScriptDiagnostic>& diagnostics,
                             std::string_view message) {
        std::size_t count = 0;
        for (const auto& diagnostic : diagnostics) {
            count += diagnostic.message == message ? 1U : 0U;
        }
        return count;
    }

    std::string describeDiagnostics(const std::vector<lumin::scripting::ScriptDiagnostic>& diagnostics) {
        std::string result;
        for (const auto& diagnostic : diagnostics) {
            if (!result.empty()) {
                result += " | ";
            }
            result += diagnostic.message;
        }
        return result;
    }

    void requireError(const lumin::scripting::ScriptResult& result, lumin::scripting::ScriptErrorCode code,
                      std::string_view message) {
        require(!result.succeeded && result.error.has_value() && result.error->code == code, message);
    }

    void testValidLifecycleOrder() {
        TemporaryScripts scripts;
        scripts.write("child.lua", R"lua(
return {
    on_spawn = function(actor, level)
        log.info("child.spawn")
    end,
    on_tick = function(actor, level, delta_seconds)
        log.info("child.tick")
        actor:destroy()
    end,
    on_destroy = function(actor, level)
        log.info("child.destroy")
    end
}
)lua");
        scripts.write("root.lua", R"lua(
return {
    on_spawn = function(actor, level)
        log.info("root.spawn")
    end,
    on_tick = function(actor, level, delta_seconds)
        log.info("root.tick")
        local child, message = level:spawn_script("child.lua")
        if child == nil then
            error(message)
        end
        actor:destroy()
    end,
    on_destroy = function(actor, level)
        log.info("root.destroy")
    end
}
)lua");

        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto root = runtime.spawn(level, "root.lua");
        require(static_cast<bool>(root), "Valid root script must spawn.");
        require(countMessage(runtime.diagnostics(), "root.spawn") == 1, "on_spawn must run exactly once.");

        level.tick(1.0f / 60.0f);
        const auto firstFrame = runtime.diagnostics();
        require(countMessage(firstFrame, "root.tick") == 1, "Root must tick on its first frame.");
        require(countMessage(firstFrame, "root.destroy") == 1,
                "Destroyed root must receive on_destroy once: " + describeDiagnostics(firstFrame));
        require(countMessage(firstFrame, "child.spawn") == 1, "Deferred child must activate after the callback.");
        require(countMessage(firstFrame, "child.tick") == 0, "Deferred child must not tick on its spawn frame.");

        level.tick(1.0f / 60.0f);
        const auto secondFrame = runtime.diagnostics();
        require(countMessage(secondFrame, "child.tick") == 1, "Deferred child must first tick on the next frame.");
        require(countMessage(secondFrame, "child.destroy") == 1, "Child destroy callback must run exactly once.");
        require(level.actorCount() == 0, "Lifecycle scripts must leave no live actors.");
    }

    void testSyntaxAndContractErrors() {
        TemporaryScripts scripts;
        scripts.write("syntax.lua", "return { on_tick = function( }");
        scripts.write("contract.lua", "return { on_tick = 42 }");

        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto syntax = runtime.spawn(level, "syntax.lua");
        const auto contract = runtime.spawn(level, "contract.lua");

        requireError(syntax.result, lumin::scripting::ScriptErrorCode::Syntax,
                     "Malformed Lua must return a Syntax error.");
        requireError(contract.result, lumin::scripting::ScriptErrorCode::Contract,
                     "Non-function callbacks must return a Contract error.");
        require(level.actorCount() == 0, "Invalid scripts must not allocate actors.");
    }

    void testRuntimeErrorIsolation() {
        TemporaryScripts scripts;
        scripts.write("bad.lua", R"lua(
return {
    on_tick = function(actor, level, delta_seconds)
        error("expected tick failure")
    end
}
)lua");
        scripts.write("good.lua", R"lua(
return {
    on_tick = function(actor, level, delta_seconds)
        actor:translate(1.0, 0.0, 0.0)
    end
}
)lua");

        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto bad = runtime.spawn(level, "bad.lua");
        const auto good = runtime.spawn(level, "good.lua");
        require(static_cast<bool>(bad) && static_cast<bool>(good), "Both runtime-error fixtures must spawn.");

        level.tick(1.0f / 60.0f);
        const auto badInfo = runtime.script(bad.script);
        const auto* goodActor = level.actor(good.actor);
        require(badInfo.has_value() && badInfo->state == lumin::scripting::ScriptState::Faulted &&
                    badInfo->lastError.has_value() &&
                    badInfo->lastError->code == lumin::scripting::ScriptErrorCode::Runtime,
                "A failing tick must fault only its script instance.");
        require(goodActor != nullptr && goodActor->transform().position.x == 1.0f,
                "A healthy script must still tick after another script fails.");

        const std::size_t errorsAfterFirstTick = countMessage(runtime.diagnostics(), "expected tick failure");
        level.tick(1.0f / 60.0f);
        goodActor = level.actor(good.actor);
        require(goodActor != nullptr && goodActor->transform().position.x == 2.0f,
                "Healthy scripts must keep ticking while a peer remains faulted.");
        require(countMessage(runtime.diagnostics(), "expected tick failure") == errorsAfterFirstTick,
                "A faulted script must not emit the same tick failure repeatedly.");
    }

    void testTransactionalReloadKeepsOldScript() {
        TemporaryScripts scripts;
        scripts.write("reload.lua", R"lua(
return {
    on_spawn = function(actor, level)
        log.info("reload.spawn")
    end,
    on_tick = function(actor, level, delta_seconds)
        actor:translate(1.0, 0.0, 0.0)
    end,
    on_destroy = function(actor, level)
        log.info("reload.destroy")
    end
}
)lua");

        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto instance = runtime.spawn(level, "reload.lua");
        require(static_cast<bool>(instance), "Reload fixture must spawn.");
        level.tick(0.0f);
        const auto before = runtime.script(instance.script);
        require(before.has_value() && before->revision == 1, "Initial script revision must be one.");

        scripts.write("reload.lua", "return { on_tick = function( }");
        const auto failedReload = runtime.reload(instance.script);
        requireError(failedReload, lumin::scripting::ScriptErrorCode::Syntax,
                     "Invalid replacement must fail transactionally.");
        const auto afterFailure = runtime.script(instance.script);
        require(afterFailure.has_value() && afterFailure->revision == before->revision &&
                    afterFailure->state == lumin::scripting::ScriptState::Running,
                "Failed reload must preserve the old running revision.");
        level.tick(0.0f);
        const auto* actorAfterFailure = level.actor(instance.actor);
        require(actorAfterFailure != nullptr && actorAfterFailure->transform().position.x == 2.0f,
                "Failed reload must keep executing the old callback.");

        scripts.write("reload.lua", R"lua(
return {
    on_spawn = function(actor, level)
        log.info("reload.spawn.unexpected")
    end,
    on_tick = function(actor, level, delta_seconds)
        actor:translate(10.0, 0.0, 0.0)
    end,
    on_destroy = function(actor, level)
        log.info("reload.destroy")
    end
}
)lua");
        const auto successfulReload = runtime.reload(instance.script);
        require(successfulReload.succeeded, "Valid replacement must reload.");
        const auto afterSuccess = runtime.script(instance.script);
        require(afterSuccess.has_value() && afterSuccess->revision == before->revision + 1,
                "Successful reload must advance the script revision.");
        level.tick(0.0f);
        const auto* actorAfterSuccess = level.actor(instance.actor);
        require(actorAfterSuccess != nullptr && actorAfterSuccess->transform().position.x == 12.0f,
                "Successful reload must atomically switch to the new callback.");
        require(countMessage(runtime.diagnostics(), "reload.spawn") == 1 &&
                    countMessage(runtime.diagnostics(), "reload.spawn.unexpected") == 0,
                "Reload must not replay lifecycle callbacks.");
        require(level.destroyActor(instance.actor), "Reload fixture must be destroyable.");
        require(countMessage(runtime.diagnostics(), "reload.destroy") == 1,
                "The active replacement must receive the one real destroy callback.");
    }

    void testStaleProxyAndHandleRejection() {
        TemporaryScripts scripts;
        scripts.write("stale.lua", R"lua(
local saved_actor = nil
return {
    on_spawn = function(actor, level)
        saved_actor = actor
    end,
    on_tick = function(actor, level, delta_seconds)
        saved_actor:translate(1.0, 0.0, 0.0)
    end
}
)lua");

        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto instance = runtime.spawn(level, "stale.lua");
        require(static_cast<bool>(instance), "Stale proxy fixture must spawn.");
        level.tick(0.0f);
        const auto info = runtime.script(instance.script);
        require(info.has_value() && info->state == lumin::scripting::ScriptState::Faulted &&
                    info->lastError.has_value() && info->lastError->code == lumin::scripting::ScriptErrorCode::Runtime,
                "A proxy retained across callbacks must be rejected as expired.");

        require(level.destroyActor(instance.actor), "Faulted script actor must still be destroyable.");
        requireError(runtime.reload(instance.script), lumin::scripting::ScriptErrorCode::InvalidHandle,
                     "Destroyed ScriptHandle must be rejected.");
    }

    void testReloadChangedUsesContent() {
        TemporaryScripts scripts;
        scripts.write("changed.lua", R"lua(
return {
    on_tick = function(actor, level, delta_seconds)
        actor:translate(1.0, 0.0, 0.0)
    end
}
)lua");

        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto instance = runtime.spawn(level, "changed.lua");
        require(static_cast<bool>(instance), "Changed-file fixture must spawn.");

        scripts.write("changed.lua", R"lua(
return {
    on_tick = function(actor, level, delta_seconds)
        actor:translate(3.0, 0.0, 0.0)
    end
}
)lua");
        const auto changed = runtime.reloadChanged();
        require(changed.size() == 1 && changed.front().script == instance.script && changed.front().result.succeeded,
                "Content change polling must reload the changed instance once.");
        require(runtime.reloadChanged().empty(), "Unchanged content must not reload repeatedly.");
        level.tick(0.0f);
        const auto* actor = level.actor(instance.actor);
        require(actor != nullptr && actor->transform().position.x == 3.0f,
                "Changed-file reload must install the new callback.");
    }

    void testDestroyErrorCleanup() {
        TemporaryScripts scripts;
        scripts.write("destroy-error.lua", R"lua(
return {
    on_destroy = function(actor, level)
        error("expected destroy failure")
    end
}
)lua");

        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto instance = runtime.spawn(level, "destroy-error.lua");
        require(static_cast<bool>(instance), "Destroy-error fixture must spawn.");
        require(level.destroyActor(instance.actor), "Destroy-error actor removal must still be accepted.");
        require(!level.isActorAlive(instance.actor) && !runtime.script(instance.script).has_value(),
                "Destroy callback errors must not retain Actor or ScriptHandle state.");
        const auto diagnostics = runtime.diagnostics();
        require(countMessage(diagnostics, "expected destroy failure") == 1,
                "Destroy callback failure must be recorded exactly once.");
    }

    void testBusyReentrancy() {
        TemporaryScripts scripts;
        scripts.write("busy.lua", R"lua(
return {
    on_tick = function(actor, level, delta_seconds)
        log.info("busy.trigger")
    end
}
)lua");

        lumin::scripting::ScriptRuntime* runtimePointer = nullptr;
        lumin::scripting::ScriptHandle scriptHandle;
        std::optional<lumin::scripting::ScriptResult> nestedExecute;
        std::optional<lumin::scripting::ScriptResult> nestedReload;
        lumin::scripting::ScriptRuntimeOptions options;
        options.scriptRoot = scripts.root();
        options.diagnosticSink = [&](const lumin::scripting::ScriptDiagnostic& diagnostic) {
            if (diagnostic.message != "busy.trigger" || runtimePointer == nullptr) {
                return;
            }
            nestedExecute = runtimePointer->execute("return 1", "<nested>");
            nestedReload = runtimePointer->reload(scriptHandle);
        };

        lumin::scene::Level level;
        lumin::scripting::ScriptRuntime runtime{std::move(options)};
        runtimePointer = &runtime;
        const auto instance = runtime.spawn(level, "busy.lua");
        require(static_cast<bool>(instance), "Busy fixture must spawn.");
        scriptHandle = instance.script;
        level.tick(0.0f);

        require(nestedExecute.has_value(), "Diagnostic sink must attempt nested execute.");
        require(nestedReload.has_value(), "Diagnostic sink must attempt nested reload.");
        requireError(*nestedExecute, lumin::scripting::ScriptErrorCode::Busy,
                     "Nested console execution must return Busy.");
        requireError(*nestedReload, lumin::scripting::ScriptErrorCode::Busy, "Nested reload must return Busy.");
    }

    void testDiagnosticEvictionAndSequence() {
        TemporaryScripts scripts;
        lumin::scripting::ScriptRuntime runtime{
            lumin::scripting::ScriptRuntimeOptions{.scriptRoot = scripts.root(), .diagnosticCapacity = 3},
        };

        require(runtime.execute("print('one')").succeeded, "First console print must execute.");
        require(runtime.execute("print('two')").succeeded, "Second console print must execute.");
        require(runtime.execute("print('three')").succeeded, "Third console print must execute.");
        require(runtime.execute("print('four')").succeeded, "Fourth console print must execute.");
        const auto retained = runtime.diagnostics();
        require(retained.size() == 3 && retained[0].sequence == 2 && retained[1].sequence == 3 &&
                    retained[2].sequence == 4,
                "Diagnostic capacity must evict oldest entries without renumbering.");

        runtime.clearDiagnostics();
        require(runtime.diagnostics().empty(), "Diagnostic clear must remove retained entries.");
        require(runtime.execute("print('five')").succeeded, "Console print after clear must execute.");
        const auto afterClear = runtime.diagnostics();
        require(afterClear.size() == 1 && afterClear.front().sequence == 5,
                "Diagnostic clear must not reset the monotonic sequence.");
    }

    void testPersistentConsoleHistory() {
        TemporaryScripts scripts;
        auto runtime = makeRuntime(scripts);
        const auto first = runtime.execute("counter = (counter or 0) + 1; return counter");
        const auto second = runtime.execute("counter = (counter or 0) + 1; return counter");
        require(first.succeeded && first.values == std::vector<std::string>{"1"},
                "Console must return the first persistent value.");
        require(second.succeeded && second.values == std::vector<std::string>{"2"},
                "Console environment must persist between commands.");

        const auto history = runtime.consoleHistory();
        require(history.size() == 2 && history[0].sequence == 1 && history[1].sequence == 2,
                "Console history must retain commands in sequence order.");
        runtime.clearConsoleHistory();
        require(runtime.consoleHistory().empty(), "Console history clear must remove retained commands.");
        require(runtime.execute("return counter").succeeded, "Console must remain usable after history clear.");
        const auto afterClear = runtime.consoleHistory();
        require(afterClear.size() == 1 && afterClear.front().sequence == 3,
                "Console history clear must not reset its monotonic sequence.");
    }

    void testEnvironmentIsolation() {
        TemporaryScripts scripts;
        scripts.write("mutator.lua", R"lua(
math.lumin_marker = true
string.lumin_marker = true
table.lumin_marker = true
utf8.lumin_marker = true
script_global = true
log.info = function(message)
    error("mutated logger escaped its environment")
end
return {}
)lua");
        scripts.write("observer.lua", R"lua(
assert(math.lumin_marker == nil, "math table leaked between scripts")
assert(string.lumin_marker == nil, "string table leaked between scripts")
assert(table.lumin_marker == nil, "table table leaked between scripts")
assert(utf8.lumin_marker == nil, "utf8 table leaked between scripts")
assert(script_global == nil, "global leaked between scripts")
return {
    on_spawn = function(actor, level)
        log.info("observer.spawn")
    end
}
)lua");

        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto mutator = runtime.spawn(level, "mutator.lua");
        const auto observer = runtime.spawn(level, "observer.lua");
        require(static_cast<bool>(mutator) && static_cast<bool>(observer),
                "Each script must receive an isolated environment and library tables.");
        require(countMessage(runtime.diagnostics(), "observer.spawn") == 1,
                "A script must receive its own unmodified log table.");

        const auto consoleBefore = runtime.execute("return math.lumin_marker, script_global");
        require(consoleBefore.succeeded && consoleBefore.values == std::vector<std::string>{"nil", "nil"},
                "Script globals and library mutations must not leak into the console.");
        require(runtime.execute("math.console_marker = true; console_global = true").succeeded,
                "Console isolation fixture must execute.");

        scripts.write("console-observer.lua", R"lua(
assert(math.console_marker == nil, "console library mutation leaked into a script")
assert(console_global == nil, "console global leaked into a script")
return {}
)lua");
        require(static_cast<bool>(runtime.spawn(level, "console-observer.lua")),
                "Console state must remain isolated from script environments.");
    }

    void testUnsafeMetatableApisHidden() {
        TemporaryScripts scripts;
        scripts.write("metatable-apis.lua", R"lua(
assert(getmetatable == nil, "getmetatable must not be exported to scripts")
assert(rawset == nil, "rawset must not be exported to scripts")
assert(setmetatable == nil, "setmetatable must not be exported to scripts")
return {}
)lua");

        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto console = runtime.execute("return getmetatable, rawset, setmetatable", "<metatable-console>");
        require(console.succeeded && console.values == std::vector<std::string>{"nil", "nil", "nil"},
                "Unsafe metatable APIs must not be exported to the console environment.");
        const auto script = runtime.spawn(level, "metatable-apis.lua");
        require(script.result.succeeded, "Unsafe metatable APIs must not be exported to loaded scripts.");
    }

    void testImmediateSelfDestroyDuringSpawn() {
        TemporaryScripts scripts;
        scripts.write("self-destroy.lua", R"lua(
return {
    on_spawn = function(actor, level)
        log.info("self.spawn")
        actor:destroy()
    end,
    on_destroy = function(actor, level)
        log.info("self.destroy")
    end
}
)lua");

        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto instance = runtime.spawn(level, "self-destroy.lua");
        require(instance.result.succeeded, "Self-destruction during on_spawn must not fail the spawn operation.");
        require(!level.isActorAlive(instance.actor) && level.actorCount() == 0,
                "An actor destroyed during on_spawn must be removed before spawn returns.");
        require(!runtime.script(instance.script).has_value(),
                "Immediate self-destruction must unregister the ScriptHandle without retaining a dangling actor.");
        const auto diagnostics = runtime.diagnostics();
        require(countMessage(diagnostics, "self.spawn") == 1 && countMessage(diagnostics, "self.destroy") == 1,
                "Immediate self-destruction must run each lifecycle callback exactly once.");
    }

    void testPathEscapeRejection() {
        TemporaryScripts scripts;
        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);

        const auto relative = runtime.spawn(level, std::filesystem::path{".."} / "outside.lua");
        requireError(relative.result, lumin::scripting::ScriptErrorCode::PathOutsideRoot,
                     "A relative script path outside scriptRoot must be rejected.");
        const auto absolute = runtime.spawn(level, scripts.root().parent_path() / "outside.lua");
        requireError(absolute.result, lumin::scripting::ScriptErrorCode::PathOutsideRoot,
                     "An absolute script path outside scriptRoot must be rejected.");
        require(level.actorCount() == 0, "Rejected script paths must not allocate actors.");
    }

    void testWrongThreadOperations() {
        TemporaryScripts scripts;
        scripts.write("thread.lua", "return {}");
        lumin::scene::Level level;
        auto runtime = makeRuntime(scripts);
        const auto instance = runtime.spawn(level, "thread.lua");
        require(static_cast<bool>(instance), "Wrong-thread fixture must spawn on the owning thread.");

        std::optional<lumin::scripting::ScriptSpawnResult> spawnResult;
        std::optional<lumin::scripting::ScriptResult> reloadResult;
        std::optional<lumin::scripting::ScriptResult> executeResult;
        std::vector<lumin::scripting::ScriptReloadResult> changedResults;
        std::thread worker{[&] {
            spawnResult = runtime.spawn(level, "thread.lua");
            reloadResult = runtime.reload(instance.script);
            changedResults = runtime.reloadChanged();
            executeResult = runtime.execute("return 1");
        }};
        worker.join();

        require(spawnResult.has_value(), "Worker thread must attempt spawn.");
        requireError(spawnResult->result, lumin::scripting::ScriptErrorCode::WrongThread,
                     "Spawn from a non-owning thread must return WrongThread.");
        require(reloadResult.has_value(), "Worker thread must attempt reload.");
        requireError(*reloadResult, lumin::scripting::ScriptErrorCode::WrongThread,
                     "Reload from a non-owning thread must return WrongThread.");
        require(changedResults.size() == 1, "Wrong-thread reloadChanged must return one structured failure.");
        requireError(changedResults.front().result, lumin::scripting::ScriptErrorCode::WrongThread,
                     "reloadChanged from a non-owning thread must return WrongThread.");
        require(executeResult.has_value(), "Worker thread must attempt console execution.");
        requireError(*executeResult, lumin::scripting::ScriptErrorCode::WrongThread,
                     "Console execution from a non-owning thread must return WrongThread.");
    }

} // namespace

int main() {
    using Test = std::pair<std::string_view, std::function<void()>>;
    const std::vector<Test> tests{
        {"valid lifecycle order", testValidLifecycleOrder},
        {"syntax and contract errors", testSyntaxAndContractErrors},
        {"runtime error isolation", testRuntimeErrorIsolation},
        {"transactional reload keeps old script", testTransactionalReloadKeepsOldScript},
        {"stale proxy and handle rejection", testStaleProxyAndHandleRejection},
        {"reloadChanged uses content", testReloadChangedUsesContent},
        {"destroy error cleanup", testDestroyErrorCleanup},
        {"Busy reentrancy", testBusyReentrancy},
        {"diagnostic eviction and sequence", testDiagnosticEvictionAndSequence},
        {"persistent console history", testPersistentConsoleHistory},
        {"environment isolation", testEnvironmentIsolation},
        {"unsafe metatable APIs hidden", testUnsafeMetatableApisHidden},
        {"immediate self-destroy during spawn", testImmediateSelfDestroyDuringSpawn},
        {"path escape rejection", testPathEscapeRejection},
        {"wrong-thread operations", testWrongThreadOperations},
    };

    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
