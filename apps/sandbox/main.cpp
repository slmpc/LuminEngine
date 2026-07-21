#include "lumin/core/Application.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <utility>

int main(int argc, char** argv) {
    lumin::core::ApplicationConfig config;

    if (argc > 1) {
        config.objPath = argv[1];
    }

    try {
        lumin::core::Application app(std::move(config));
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
