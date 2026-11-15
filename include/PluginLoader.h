#pragma once
#include <string>
#include <vector>

namespace PluginLoader {

struct PluginInfo {
    std::string name;
    std::string path;
    void* handle;
};

// Discovers and loads all mod plugins.
// Scans:
//   1. plugins/<PluginFolder>/*.dll (for plugins that have their own folder/configs)
//   2. plugins/*.dll (for standalone loose plugins)
// Calls PluginInit() on the plugin if exported.
void LoadPlugins();

// Returns the list of all currently loaded plugins
const std::vector<PluginInfo>& GetLoadedPlugins();

} // namespace PluginLoader
