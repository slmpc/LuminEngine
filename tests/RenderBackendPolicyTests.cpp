#include "render/BackendLifetime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

    namespace fs = std::filesystem;

    struct Violation {
        fs::path path;
        std::string rule;
        std::string token;
    };

    std::string readFile(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot read " + path.generic_string());
        }
        std::string result{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());
        return result;
    }

    std::string stripCommentsAndLiterals(std::string_view source) {
        enum class State {
            Code,
            LineComment,
            BlockComment,
            String,
            Character,
            RawString
        };
        State state = State::Code;
        std::string result(source.size(), ' ');
        std::string rawEnd;
        for (std::size_t index = 0; index < source.size();) {
            const char current = source[index];
            const char next = index + 1 < source.size() ? source[index + 1] : '\0';
            if (current == '\n') {
                result[index] = '\n';
            }
            if (state == State::Code) {
                if (current == '/' && next == '/') {
                    state = State::LineComment;
                    index += 2;
                } else if (current == '/' && next == '*') {
                    state = State::BlockComment;
                    index += 2;
                } else if (current == '"') {
                    state = State::String;
                    ++index;
                } else if (current == '\'') {
                    state = State::Character;
                    ++index;
                } else if (current == 'R' && next == '"') {
                    const std::size_t open = source.find('(', index + 2);
                    if (open == std::string_view::npos || open - index > 18) {
                        result[index++] = current;
                    } else {
                        rawEnd = ")" + std::string(source.substr(index + 2, open - index - 2)) + "\"";
                        state = State::RawString;
                        index = open + 1;
                    }
                } else {
                    result[index++] = current;
                }
            } else if (state == State::LineComment) {
                if (current == '\n') {
                    state = State::Code;
                }
                ++index;
            } else if (state == State::BlockComment) {
                if (current == '*' && next == '/') {
                    state = State::Code;
                    index += 2;
                } else {
                    ++index;
                }
            } else if (state == State::RawString) {
                if (source.substr(index, rawEnd.size()) == rawEnd) {
                    index += rawEnd.size();
                    state = State::Code;
                } else {
                    ++index;
                }
            } else {
                const char delimiter = state == State::String ? '"' : '\'';
                if (current == '\\' && index + 1 < source.size()) {
                    index += 2;
                } else {
                    ++index;
                    if (current == delimiter) {
                        state = State::Code;
                    }
                }
            }
        }
        return result;
    }

    std::vector<fs::path> rendererSources(const fs::path& root) {
        std::vector<fs::path> files;
        for (const fs::path& directory : {root / "render"}) {
            for (const fs::directory_entry& entry : fs::recursive_directory_iterator(directory)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                const std::string extension = entry.path().extension().string();
                if (extension == ".h" || extension == ".hpp" || extension == ".cpp") {
                    files.push_back(fs::weakly_canonical(entry.path()));
                }
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    void addIdentifierViolations(const fs::path& root, const fs::path& path, const std::string& code,
                                 std::vector<Violation>& violations) {
        const fs::path relative = fs::relative(path, root);
        const bool contextAllowlist = relative == fs::path("render/platform/vulkan/VulkanContext.hpp") ||
                                      relative == fs::path("render/platform/vulkan/VulkanContext.cpp") ||
                                      relative == fs::path("render/platform/Window.hpp") ||
                                      relative == fs::path("render/platform/Window.cpp");
        static const std::regex identifierPattern(R"(\b[A-Za-z_][A-Za-z0-9_]*\b)");
        for (std::sregex_iterator iterator(code.begin(), code.end(), identifierPattern), end; iterator != end;
             ++iterator) {
            const std::string identifier = iterator->str();
            const bool nativeVulkan = identifier.starts_with("Vk") || identifier.starts_with("VK_") ||
                                      (identifier.size() > 2 && identifier.starts_with("vk") &&
                                       std::isupper(static_cast<unsigned char>(identifier[2])) != 0);
            if (!contextAllowlist && nativeVulkan) {
                violations.push_back({relative, "native Vulkan is restricted to VulkanContext", identifier});
            }
            if (identifier == "VulkanBuffer" || identifier == "VulkanImage" || identifier == "VulkanResourceManager") {
                violations.push_back({relative, "temporary Vulkan resource aliases are forbidden", identifier});
            }
            if (identifier.starts_with("ImGui_ImplVulkan")) {
                violations.push_back({relative, "Dear ImGui Vulkan backend is forbidden", identifier});
            }
        }
    }

    void inspectContents(const fs::path& root, const fs::path& path, const std::string& source,
                         std::vector<Violation>& violations) {
        const fs::path relative = fs::relative(path, root);
        const std::string code = stripCommentsAndLiterals(source);
        addIdentifierViolations(root, path, code, violations);

        const bool frameGraphImplementation = relative == fs::path("render/resources/FrameGraph.cpp");
        if (!frameGraphImplementation) {
            std::string callsOnly = code;
            if (relative == fs::path("render/resources/FrameGraph.hpp")) {
                static const std::regex pureVirtualBarrierDeclaration(
                    R"(virtual\s+void\s+(?:beginTracking[A-Za-z0-9_]*State|set(?:Texture|Buffer|Permanent)[A-Za-z0-9_]*State|commitBarriers)\s*\([^;{}]*\)\s*(?:=\s*0)?\s*;)");
                callsOnly = std::regex_replace(callsOnly, pureVirtualBarrierDeclaration, "");
            }
            static const std::regex explicitBarrierCall(
                R"(\b(beginTracking[A-Za-z0-9_]*State|set(?:Texture|Buffer|Permanent)[A-Za-z0-9_]*State|commitBarriers)\s*\()");
            for (std::sregex_iterator iterator(callsOnly.begin(), callsOnly.end(), explicitBarrierCall), end;
                 iterator != end; ++iterator) {
                violations.push_back({relative, "FrameGraph is the sole explicit barrier owner", (*iterator)[1]});
            }
        }

        static const std::regex createCommandListPattern(R"(\bcreateCommandList\s*\()");
        static const std::regex enableAutomaticBarriersPattern(R"(setEnableAutomaticBarriers\s*\(\s*true\s*\))");
        static const std::regex disableAutomaticBarriersPattern(R"(setEnableAutomaticBarriers\s*\(\s*false\s*\))");
        const std::size_t commandLists = std::distance(
            std::sregex_iterator(code.begin(), code.end(), createCommandListPattern), std::sregex_iterator());
        const std::size_t disables = std::distance(
            std::sregex_iterator(code.begin(), code.end(), disableAutomaticBarriersPattern), std::sregex_iterator());
        const std::size_t enables = std::distance(
            std::sregex_iterator(code.begin(), code.end(), enableAutomaticBarriersPattern), std::sregex_iterator());
        const bool initializationUpload = relative == fs::path("render/resources/VulkanResources.cpp") ||
                                          relative == fs::path("render/presentation/UiRenderer.cpp");
        if (enables != (initializationUpload ? 1U : 0U)) {
            violations.push_back({relative,
                                  "automatic barriers are allowed exactly once on dedicated initialization uploads",
                                  std::to_string(enables) + " enable"});
        }
        if (commandLists != enables + disables) {
            violations.push_back({relative, "every renderer command list must select an explicit barrier policy",
                                  std::to_string(commandLists) + " create / " + std::to_string(enables) + " enable / " +
                                      std::to_string(disables) + " disable"});
        }
    }

    void inspectFile(const fs::path& root, const fs::path& path, std::vector<Violation>& violations) {
        inspectContents(root, path, readFile(path), violations);
    }

    std::size_t requireAfter(const std::string& text, std::string_view token, std::size_t position,
                             std::string_view contract) {
        const std::size_t found = text.find(token, position);
        if (found == std::string::npos) {
            throw std::runtime_error(std::string(contract) + ": missing or out-of-order token " + std::string(token));
        }
        return found + token.size();
    }

    void verifySubmissionAndLifetime(const fs::path& root) {
        const std::string context =
            stripCommentsAndLiterals(readFile(root / "render/platform/vulkan/VulkanContext.cpp"));
        std::size_t position = context.find("void VulkanContext::submitFrameCommands");
        position = requireAfter(context, "queueWaitForSemaphore", position, "same-submit ordering");
        position = requireAfter(context, "queueSignalSemaphore", position, "same-submit ordering");
        requireAfter(context, "executeCommandLists", position, "same-submit ordering");

        position = context.find("void VulkanContext::destroy()");
        requireAfter(context, "detail::destroyBackendLifetime", position, "production lifetime seam");
        if (context.find("catch (...) {\n            destroy();\n            throw;") == std::string::npos ||
            context.find("VulkanContext::~VulkanContext() {\n        destroy();") == std::string::npos) {
            throw std::runtime_error("normal and partial construction paths must share idempotent destroy()");
        }

        const std::string application = stripCommentsAndLiterals(readFile(root / "application/Application.cpp"));
        const std::string rendererRuntime = stripCommentsAndLiterals(readFile(root / "render/runtime/Renderer.cpp"));
        if (application.find("std::unique_ptr<render::Renderer> renderer") == std::string::npos ||
            application.find("make_unique<render::VulkanSurfaceBootstrap>") == std::string::npos ||
            application.find("make_unique<render::VulkanContext>") != std::string::npos ||
            rendererRuntime.find("make_unique<VulkanContext>(std::move(*bootstrap))") == std::string::npos ||
            rendererRuntime.find("session.reset();\n                    context.reset();") == std::string::npos) {
            throw std::runtime_error(
                "Main thread must transfer a surface bootstrap before the Renderer thread creates and destroys VulkanContext");
        }

        const auto recordCleanup = [](lumin::render::detail::BackendLifetimeAvailability availability) {
            bool destroyed = false;
            std::vector<std::string> events;
            const auto invoke = [&] {
                lumin::render::detail::destroyBackendLifetime(
                    destroyed, availability,
                    [&] {
                        events.emplace_back("children");
                    },
                    [&] {
                        events.emplace_back("swapchain-wrappers");
                    },
                    [&] {
                        events.emplace_back("nvrhi-device");
                    },
                    [&] {
                        events.emplace_back("vk-device");
                    });
            };
            invoke();
            invoke();
            return events;
        };
        using lumin::render::detail::BackendLifetimeAvailability;
        if (recordCleanup({true, true, true, true}) !=
                std::vector<std::string>{"children", "swapchain-wrappers", "nvrhi-device", "vk-device"} ||
            recordCleanup({true, false, false, false}) != std::vector<std::string>{"children"} ||
            recordCleanup({true, true, false, true}) !=
                std::vector<std::string>{"children", "swapchain-wrappers", "vk-device"}) {
            throw std::runtime_error("lifetime recorder rejected normal, partial, or repeated cleanup");
        }
    }

    void verifyScannerAdversarialCases(const fs::path& root) {
        const std::string decoys =
            "// vkCreateForbidden(); commitBarriers();\nconst char* text = \"VkRenderPass\";\n"
            "const char* raw = R\"tag(setTextureState(); setEnableAutomaticBarriers(true))tag\";\n";
        const std::string strippedDecoys = stripCommentsAndLiterals(decoys);
        if (strippedDecoys.find("vkCreateForbidden") != std::string::npos ||
            strippedDecoys.find("commitBarriers") != std::string::npos ||
            strippedDecoys.find("setTextureState") != std::string::npos) {
            throw std::runtime_error("scanner did not ignore comment/string false positives");
        }
        const std::vector<std::pair<fs::path, std::string>> maliciousCases = {
            {root / "render/AdversarialProbe.cpp", "void vkCreateMaliciousHelper();"},
            {root / "render/resources/FrameGraph.hpp", "inline void probe(){ commitBarriers(); }"},
            {root / "render/AdversarialProbe.cpp", "void probe(){ setEnableAutomaticBarriers(true); }"},
        };
        for (const auto& [path, malicious] : maliciousCases) {
            std::vector<Violation> violations;
            inspectContents(root, path, malicious, violations);
            if (violations.empty()) {
                throw std::runtime_error("scanner did not catch an adversarial backend policy probe");
            }
        }
    }

} // namespace

int main() {
    try {
        const fs::path root = fs::weakly_canonical(fs::path(LUMIN_TEST_SOURCE_DIR));
        const std::vector<fs::path> sources = rendererSources(root);
        if (sources.empty()) {
            throw std::runtime_error("renderer source enumeration returned no files");
        }
        verifyScannerAdversarialCases(root);
        std::vector<Violation> violations;
        for (const fs::path& source : sources) {
            inspectFile(root, source, violations);
        }
        for (const Violation& violation : violations) {
            std::cerr << violation.path.generic_string() << ": " << violation.rule << " [" << violation.token << "]\n";
        }
        if (!violations.empty()) {
            std::cerr << "FAIL: " << violations.size() << " renderer backend policy violation(s) across "
                      << sources.size() << " enumerated source files.\n";
            return 1;
        }
        verifySubmissionAndLifetime(root);
        std::cout << "PASS: enumerated " << sources.size()
                  << " renderer source files; VulkanContext is the only native Vulkan boundary.\n";
        std::cout << "PASS: FrameGraph owns runtime barriers; only dedicated initialization uploads enable automatic "
                     "barriers.\n";
        std::cout << "PASS: lifetime recorder validated normal, partial, and repeated cleanup ordering.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
