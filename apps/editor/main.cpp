#include "application/Application.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

    class EditorHostGame final : public lumin::game::Game {
    public:
        void initialize(lumin::game::GameContext&) override {
        }

        void tick(lumin::game::GameContext&, float) override {
        }
    };

    bool parseBoolean(std::string_view value) {
        if (value == "true" || value == "1" || value == "on") {
            return true;
        }
        if (value == "false" || value == "0" || value == "off") {
            return false;
        }
        throw std::invalid_argument("Invalid value for --renderdoc: " + std::string{value});
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
            } else {
                throw std::invalid_argument("Unknown option: " + std::string{argument});
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
        lumin::core::Application app(parseArguments(argc, argv), std::make_unique<EditorHostGame>());
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
