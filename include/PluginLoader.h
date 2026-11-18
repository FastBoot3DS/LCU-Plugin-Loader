#pragma once
#include <string>
#include <vector>

namespace PluginLoader {

struct PluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string repository;
    std::string path;
    void* handle;
};

// Discovers and loads all mod plugins.
// Scans:
//   1. plugins/<PluginFolder>/ (manifest entry DLL, or all DLLs for legacy plugins)
//   2. plugins/*.dll (for standalone loose plugins)
// Validates optional plugin.json manifests and calls PluginInitV2() or the
// legacy PluginInit()/ModInit() hook when exported.
void LoadPlugins();

// Returns the list of all currently loaded plugins
const std::vector<PluginInfo>& GetLoadedPlugins();

} // namespace PluginLoader
