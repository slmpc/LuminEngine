#include "lumin/core/Application.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

    bool parseBoolean(std::string_view value) {
        if (value == "true" || value == "1" || value == "on") {
            return true;
        }
        if (value == "false" || value == "0" || value == "off") {
            return false;
        }
        throw std::invalid_argument("Invalid value for --renderdoc: " + std::string(value));
    }

    lumin::core::ApplicationConfig parseArguments(int argc, char** argv) {
        lumin::core::ApplicationConfig config;
        constexpr std::string_view renderDocValuePrefix = "--renderdoc=";
        constexpr std::string_view renderDocPathPrefix = "--renderdoc-path=";

        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--renderdoc") {
                config.enableRenderDoc = true;
            } else if (argument.starts_with(renderDocValuePrefix)) {
                config.enableRenderDoc = parseBoolean(argument.substr(renderDocValuePrefix.size()));
            } else if (argument == "--renderdoc-path") {
                if (++index >= argc) {
                    throw std::invalid_argument("--renderdoc-path requires a path");
                }
                config.renderDocPath = argv[index];
            } else if (argument.starts_with(renderDocPathPrefix)) {
                config.renderDocPath = argument.substr(renderDocPathPrefix.size());
            } else if (argument.starts_with('-')) {
                throw std::invalid_argument("Unknown option: " + std::string(argument));
            } else if (!config.objPath.has_value()) {
                config.objPath = argument;
            } else {
                throw std::invalid_argument("Only one OBJ path may be supplied");
            }
        }

        if (config.enableRenderDoc && (!config.renderDocPath.has_value() || config.renderDocPath->empty())) {
            throw std::invalid_argument("--renderdoc=true requires --renderdoc-path <renderdoc library>");
        }
        return config;
    }

} // namespace

int main(int argc, char** argv) {
    try {
        lumin::core::ApplicationConfig config = parseArguments(argc, argv);
        lumin::core::Application app(std::move(config));
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
