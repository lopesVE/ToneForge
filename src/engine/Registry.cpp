#include "engine/Module.h"

static std::vector<ModuleType>& registry() {
    static std::vector<ModuleType> types;
    return types;
}

Registrar::Registrar(const ModuleInfo& info, std::function<ModulePtr()> create) {
    registry().push_back({&info, std::move(create)});
}

const std::vector<ModuleType>& moduleTypes() { return registry(); }

ModulePtr createModule(const std::string& typeId) {
    for (auto& t : registry())
        if (t.info->typeId == typeId) return t.create();
    return nullptr;
}

const ModuleInfo* findModuleInfo(const std::string& typeId) {
    for (auto& t : registry())
        if (t.info->typeId == typeId) return t.info;
    return nullptr;
}
