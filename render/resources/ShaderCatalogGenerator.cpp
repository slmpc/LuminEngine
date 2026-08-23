#include "ShaderCatalog.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

    namespace fs = std::filesystem;
    using lumin::render::ShaderCatalog;
    using lumin::render::ShaderEntryDesc;
    using lumin::render::ShaderFeature;

    struct Arguments {
        fs::path sourceDirectory;
        fs::path outputDirectory;
        fs::path slangc;
        bool rayTracing = false;
        bool nrd = false;
        bool sharc = false;
    };

    bool parseBool(std::string_view value) {
        return value == "1" || value == "ON" || value == "on" || value == "true" || value == "TRUE";
    }

    Arguments parseArguments(int argc, char** argv) {
        std::unordered_map<std::string, std::string> values;
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc) {
                throw std::invalid_argument("Shader catalog generator arguments must use --key value pairs.");
            }
            values.emplace(argv[index], argv[index + 1]);
        }
        for (const std::string_view required :
             {"--source-dir", "--output-dir", "--slangc", "--ray-tracing", "--nrd", "--sharc"}) {
            if (!values.contains(std::string(required))) {
                throw std::invalid_argument("Missing shader generator argument: " + std::string(required));
            }
        }
        return Arguments{
            .sourceDirectory = fs::absolute(values.at("--source-dir")),
            .outputDirectory = fs::absolute(values.at("--output-dir")),
            .slangc = fs::absolute(values.at("--slangc")),
            .rayTracing = parseBool(values.at("--ray-tracing")),
            .nrd = parseBool(values.at("--nrd")),
            .sharc = parseBool(values.at("--sharc")),
        };
    }

    std::string jsonQuote(std::string_view value) {
        std::string result = "\"";
        for (const char character : value) {
            switch (character) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += character;
                break;
            }
        }
        result += '"';
        return result;
    }

    std::string pathText(const fs::path& path) {
        return path.generic_string();
    }

    std::string cmakeQuote(const fs::path& path) {
        return jsonQuote(pathText(path));
    }

    std::string cmakeQuote(const std::string& value) {
        return jsonQuote(value);
    }

    std::string cmakeQuote(std::string_view value) {
        return jsonQuote(value);
    }

    template <typename Values, typename Convert>
    void writeJsonArray(std::ostringstream& output, const Values& values, Convert convert) {
        output << '[';
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << convert(values[index]);
        }
        output << ']';
    }

    std::string makeManifest(const ShaderCatalog& catalog) {
        std::ostringstream output;
        output << "{\n  \"schemaVersion\": 1,\n  \"compiler\": {\n"
               << "    \"target\": " << jsonQuote(catalog.target) << ",\n"
               << "    \"profile\": " << jsonQuote(catalog.profile) << ",\n"
               << "    \"optimization\": " << jsonQuote(catalog.optimization) << ",\n"
               << "    \"matrixLayout\": " << jsonQuote(catalog.matrixLayout) << ",\n"
               << "    \"warningsAsErrors\": " << jsonQuote(catalog.warningsAsErrors) << ",\n"
               << "    \"includeDirectories\": ";
        writeJsonArray(output, catalog.includeDirectories, [](const std::string& value) {
            return jsonQuote(value);
        });
        output << "\n  },\n  \"abi\": {\n    \"structs\": [\n";
        for (std::size_t structIndex = 0; structIndex < catalog.abiStructs.size(); ++structIndex) {
            const auto& abi = catalog.abiStructs[structIndex];
            output << "      {\n        \"name\": " << jsonQuote(abi.name) << ",\n        \"size\": " << abi.size
                   << ",\n        \"fields\": [\n";
            for (std::size_t fieldIndex = 0; fieldIndex < abi.fields.size(); ++fieldIndex) {
                const auto& field = abi.fields[fieldIndex];
                output << "          {\"name\": " << jsonQuote(field.name) << ", \"offset\": " << field.offset
                       << ", \"size\": " << field.size << '}';
                output << (fieldIndex + 1 == abi.fields.size() ? "\n" : ",\n");
            }
            output << "        ]\n      }" << (structIndex + 1 == catalog.abiStructs.size() ? "\n" : ",\n");
        }
        output << "    ]\n  },\n  \"shaders\": [\n";
        for (std::size_t entryIndex = 0; entryIndex < catalog.entries.size(); ++entryIndex) {
            const ShaderEntryDesc& entry = catalog.entries[entryIndex];
            output << "    {\n"
                   << "      \"name\": " << jsonQuote(entry.name) << ",\n"
                   << "      \"source\": " << jsonQuote(entry.source) << ",\n"
                   << "      \"entry\": " << jsonQuote(entry.entryPoint) << ",\n"
                   << "      \"stage\": " << jsonQuote(lumin::render::toString(entry.stage)) << ",\n"
                   << "      \"output\": " << jsonQuote(entry.output) << ",\n"
                   << "      \"reflection\": " << jsonQuote(entry.reflection) << ",\n"
                   << "      \"depfile\": " << jsonQuote(entry.depfile) << ",\n"
                   << "      \"requires\": ";
            writeJsonArray(output, entry.requirements, [](ShaderFeature feature) {
                return jsonQuote(lumin::render::toString(feature));
            });
            output << ",\n      \"capabilities\": ";
            writeJsonArray(output, entry.capabilities, [](const std::string& value) {
                return jsonQuote(value);
            });
            output << ",\n      \"includeDirectories\": ";
            writeJsonArray(output, entry.includeDirectories, [](const std::string& value) {
                return jsonQuote(value);
            });
            output << ",\n      \"bindings\": [\n";
            for (std::size_t bindingIndex = 0; bindingIndex < entry.bindings.size(); ++bindingIndex) {
                const auto& binding = entry.bindings[bindingIndex];
                output << "        {\"name\": " << jsonQuote(binding.name)
                       << ", \"kind\": " << jsonQuote(lumin::render::toString(binding.kind));
                if (binding.descriptorSet.has_value()) {
                    output << ", \"set\": " << *binding.descriptorSet;
                }
                output << ", \"binding\": " << binding.binding << '}';
                output << (bindingIndex + 1 == entry.bindings.size() ? "\n" : ",\n");
            }
            output << "      ],\n      \"abiStructs\": ";
            writeJsonArray(output, entry.abiStructs, [](const std::string& value) {
                return jsonQuote(value);
            });
            output << "\n    }" << (entryIndex + 1 == catalog.entries.size() ? "\n" : ",\n");
        }
        output << "  ]\n}\n";
        return output.str();
    }

    bool featureEnabled(ShaderFeature feature, const Arguments& arguments) {
        switch (feature) {
        case ShaderFeature::RayTracing:
            return arguments.rayTracing;
        case ShaderFeature::Nrd:
            return arguments.nrd;
        case ShaderFeature::Sharc:
            return arguments.sharc;
        }
        return false;
    }

    bool entryEnabled(const ShaderEntryDesc& entry, const Arguments& arguments) {
        for (const ShaderFeature feature : entry.requirements) {
            if (!featureEnabled(feature, arguments)) {
                return false;
            }
        }
        return true;
    }

    fs::path checkedDirectory(const fs::path& sourceDirectory, std::string_view relative, std::string_view shaderName) {
        const fs::path directory = fs::weakly_canonical(sourceDirectory / relative);
        if (!fs::is_directory(directory)) {
            throw std::runtime_error(std::string(shaderName) +
                                     ": shader search directory does not exist: " + std::string(relative));
        }
        return directory;
    }

    std::string makeTargets(const ShaderCatalog& catalog, const Arguments& arguments) {
        const fs::path manifestPath = fs::absolute(arguments.outputDirectory / "shader-manifest.json");
        std::ostringstream output;
        output << "# Generated by the C++ ShaderCatalogBuilder; do not edit.\n"
               << "set(LUMIN_SHADER_MANIFEST " << cmakeQuote(manifestPath) << ")\n"
               << "set(LUMIN_SHADER_OUTPUTS)\nset(LUMIN_SHADER_REFLECTIONS)\n";
        for (const ShaderEntryDesc& entry : catalog.entries) {
            const fs::path source = fs::weakly_canonical(arguments.sourceDirectory / entry.source);
            if (!fs::is_regular_file(source)) {
                throw std::runtime_error(entry.name + ": shader source does not exist: " + entry.source);
            }
            if (!entryEnabled(entry, arguments)) {
                continue;
            }
            const fs::path binary = fs::absolute(arguments.outputDirectory / entry.output);
            const fs::path reflection = fs::absolute(arguments.outputDirectory / entry.reflection);
            const fs::path depfile = fs::absolute(arguments.outputDirectory / entry.depfile);
            output << "add_custom_command(\n"
                   << "    OUTPUT " << cmakeQuote(binary) << ' ' << cmakeQuote(reflection) << "\n"
                   << "    BYPRODUCTS " << cmakeQuote(depfile) << "\n"
                   << "    COMMAND ${CMAKE_COMMAND} -E make_directory " << cmakeQuote(binary.parent_path()) << ' '
                   << cmakeQuote(reflection.parent_path()) << ' ' << cmakeQuote(depfile.parent_path()) << "\n"
                   << "    COMMAND " << cmakeQuote(arguments.slangc) << ' ' << cmakeQuote(source) << " -target "
                   << cmakeQuote(catalog.target) << " -profile " << cmakeQuote(catalog.profile)
                   << " -warnings-as-errors " << cmakeQuote(catalog.warningsAsErrors) << " -O" << catalog.optimization;
            if (catalog.target == "spirv") {
                output << " -fvk-use-entrypoint-name";
            }
            if (!catalog.matrixLayout.empty()) {
                output << " -matrix-layout-" << catalog.matrixLayout;
            }
            if (!entry.capabilities.empty()) {
                std::string joined;
                for (const std::string& capability : entry.capabilities) {
                    if (!joined.empty()) {
                        joined += '+';
                    }
                    joined += capability;
                }
                output << " -capability " << cmakeQuote(joined);
            }
            std::vector<fs::path> includeDirectories;
            for (const std::string& directory : catalog.includeDirectories) {
                includeDirectories.push_back(checkedDirectory(arguments.sourceDirectory, directory, entry.name));
            }
            for (const std::string& directory : entry.includeDirectories) {
                const fs::path resolved = checkedDirectory(arguments.sourceDirectory, directory, entry.name);
                if (std::ranges::find(includeDirectories, resolved) == includeDirectories.end()) {
                    includeDirectories.push_back(resolved);
                }
            }
            for (const fs::path& directory : includeDirectories) {
                output << " -I " << cmakeQuote(directory);
            }
            for (const std::string& define : entry.defines) {
                output << " -D " << cmakeQuote(define);
            }
            output << " -entry " << cmakeQuote(entry.entryPoint) << " -stage "
                   << cmakeQuote(lumin::render::toString(entry.stage)) << " -reflection-json " << cmakeQuote(reflection)
                   << " -depfile " << cmakeQuote(depfile);
            for (const std::string& option : entry.options) {
                output << ' ' << cmakeQuote(option);
            }
            output << " -o " << cmakeQuote(binary) << "\n"
                   << "    DEPENDS " << cmakeQuote(source) << "\n"
                   << "    DEPFILE " << cmakeQuote(depfile) << "\n"
                   << "    COMMENT " << cmakeQuote("Compiling " + entry.name) << "\n"
                   << "    VERBATIM\n)\n"
                   << "list(APPEND LUMIN_SHADER_OUTPUTS " << cmakeQuote(binary) << ")\n"
                   << "list(APPEND LUMIN_SHADER_REFLECTIONS " << cmakeQuote(reflection) << ")\n";
        }
        return output.str();
    }

    void writeIfChanged(const fs::path& path, std::string_view text) {
        std::ifstream existing(path, std::ios::binary);
        if (existing) {
            std::ostringstream current;
            current << existing.rdbuf();
            if (current.str() == text) {
                return;
            }
        }
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw std::runtime_error("Failed to open generated shader file: " + pathText(path));
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            throw std::runtime_error("Failed to write generated shader file: " + pathText(path));
        }
    }

} // namespace

int main(int argc, char** argv) {
    try {
        const Arguments arguments = parseArguments(argc, argv);
        const ShaderCatalog& catalog = lumin::render::builtinShaderCatalog();
        writeIfChanged(arguments.outputDirectory / "shader-manifest.json", makeManifest(catalog));
        writeIfChanged(arguments.outputDirectory / "shader-targets.cmake", makeTargets(catalog, arguments));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ShaderCatalogGenerator: " << error.what() << '\n';
        return 1;
    }
}
