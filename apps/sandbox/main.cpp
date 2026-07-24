#include "SandboxGame.hpp"

#include "lumin/core/Application.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

    struct LaunchOptions {
        lumin::core::ApplicationConfig application;
        lumin::sandbox::SandboxGameConfig game;
    };

    bool parseBoolean(std::string_view value) {
        if (value == "true" || value == "1" || value == "on") {
            return true;
        }
        if (value == "false" || value == "0" || value == "off") {
            return false;
        }
        throw std::invalid_argument("Invalid value for --renderdoc: " + std::string(value));
    }

    void setStartupScript(lumin::core::ApplicationConfig& config, const std::filesystem::path& source) {
        if (source.empty()) {
            throw std::invalid_argument("--script requires a path");
        }
        const std::filesystem::path absoluteSource = std::filesystem::absolute(source).lexically_normal();
        config.scriptRoot = absoluteSource.parent_path();
        config.startupScript = absoluteSource.filename();
    }

    LaunchOptions parseArguments(int argc, char** argv) {
        LaunchOptions options;
#if defined(LUMIN_SANDBOX_SCRIPT_DIR)
        options.application.scriptRoot = LUMIN_SANDBOX_SCRIPT_DIR;
#else
        options.application.scriptRoot = std::filesystem::path{"apps"} / "sandbox" / "scripts";
#endif
        options.application.startupScript = "sandbox.lua";

        constexpr std::string_view renderDocValuePrefix = "--renderdoc=";
        constexpr std::string_view renderDocPathPrefix = "--renderdoc-path=";
        constexpr std::string_view scriptPrefix = "--script=";

        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--renderdoc") {
                options.application.enableRenderDoc = true;
            } else if (argument.starts_with(renderDocValuePrefix)) {
                options.application.enableRenderDoc = parseBoolean(argument.substr(renderDocValuePrefix.size()));
            } else if (argument == "--renderdoc-path") {
                if (++index >= argc) {
                    throw std::invalid_argument("--renderdoc-path requires a path");
                }
                options.application.renderDocPath = argv[index];
            } else if (argument.starts_with(renderDocPathPrefix)) {
                options.application.renderDocPath = argument.substr(renderDocPathPrefix.size());
            } else if (argument == "--script") {
                if (++index >= argc) {
                    throw std::invalid_argument("--script requires a path");
                }
                setStartupScript(options.application, argv[index]);
            } else if (argument.starts_with(scriptPrefix)) {
                setStartupScript(options.application, argument.substr(scriptPrefix.size()));
            } else if (argument.starts_with('-')) {
                throw std::invalid_argument("Unknown option: " + std::string(argument));
            } else if (!options.game.objPath.has_value()) {
                options.game.objPath = argument;
            } else {
                throw std::invalid_argument("Only one OBJ path may be supplied");
            }
        }

        if (options.application.enableRenderDoc &&
            (!options.application.renderDocPath.has_value() || options.application.renderDocPath->empty())) {
            throw std::invalid_argument("--renderdoc=true requires --renderdoc-path <renderdoc library>");
        }
        return options;
    }

} // namespace

int main(int argc, char** argv) {
    try {
        LaunchOptions options = parseArguments(argc, argv);
        auto game = std::make_unique<lumin::sandbox::SandboxGame>(std::move(options.game));
        lumin::core::Application app(std::move(options.application), std::move(game));
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
