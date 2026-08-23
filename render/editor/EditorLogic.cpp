#include "render/editor/EditorLogic.hpp"

#include <algorithm>

namespace lumin::editor {

    const EditorActorSnapshot* EditorLogicSnapshot::findActor(scene::ActorHandle handle) const noexcept {
        const auto found = std::ranges::find(actors, handle, &EditorActorSnapshot::handle);
        return found == actors.end() ? nullptr : &*found;
    }

    const EditorModelSnapshot* EditorLogicSnapshot::findModel(scene::ModelHandle handle) const noexcept {
        const auto found = std::ranges::find(models, handle, &EditorModelSnapshot::handle);
        return found == models.end() ? nullptr : &*found;
    }

    const scripting::ScriptInfo* EditorLogicSnapshot::findScript(scripting::ScriptHandle handle) const noexcept {
        const auto found = std::ranges::find(scripts, handle, &scripting::ScriptInfo::handle);
        return found == scripts.end() ? nullptr : &*found;
    }

    std::vector<scripting::ScriptInfo> EditorLogicSnapshot::scriptsForActor(scene::ActorHandle actor) const {
        std::vector<scripting::ScriptInfo> result;
        std::ranges::copy_if(scripts, std::back_inserter(result), [actor](const scripting::ScriptInfo& info) {
            return info.actor == actor;
        });
        return result;
    }

} // namespace lumin::editor
