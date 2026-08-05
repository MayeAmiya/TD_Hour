#include "core/container/hash_containers.h"
#include "DrawFunc.h"
#include "../../../core/constants/Strings.h"
#include <mutex>

namespace gui::draw {

struct DrawFuncPair {
    DrawFunc imageFunc;
    DrawFunc colorFunc;
};

static container::HashMap<container::String, DrawFuncPair>& getRegistry() {
    static container::HashMap<container::String, DrawFuncPair> s_registry;
    return s_registry;
}

DrawFunc getDrawFunc(const container::String& windowType, bool hasImageStatus) {
    auto& reg = getRegistry();
    auto it = reg.find(windowType);
    if (it == reg.end()) {
        // Fallback: try lowercase
        container::String lower = windowType;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        it = reg.find(lower);
    }
    if (it == reg.end()) return nullptr;
    return hasImageStatus ? it->second.imageFunc : it->second.colorFunc;
}

void registerDrawFunc(const container::String& windowType, DrawFunc imageFunc, DrawFunc colorFunc) {
    getRegistry()[windowType] = { std::move(imageFunc), std::move(colorFunc) };
}

} // namespace gui::draw
