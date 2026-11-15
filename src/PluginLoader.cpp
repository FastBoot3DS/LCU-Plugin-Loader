#include "PluginLoader.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern void Log(const char* fmt, ...);

namespace PluginLoader {

static std::vector<PluginInfo> g_loadedPlugins;

static bool EndsWithIgnoreCase(const std::string& str, const std::string& suffix) {
    if (str.length() < suffix.length()) return false;
    size_t offset = str.length() - suffix.length();
    for (size_t i = 0; i < suffix.length(); ++i) {
        if (std::tolower(str[offset + i]) != std::tolower(suffix[i])) {
            return false;
        }
    }
    return true;
}

static void LoadSinglePlugin(const std::string& fullPath, const std::string& displayName) {
    Log("[PluginLoader] Loading plugin: %s (%s)...\n", displayName.c_str(), fullPath.c_str());

    HMODULE hMod = LoadLibraryExA(fullPath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hMod) {
        DWORD err = GetLastError();
        Log("[PluginLoader] [ERROR] Failed to load %s (Win32 Error: %lu)\n", displayName.c_str(), err);
        return;
    }

    PluginInfo info;
    info.name = displayName;
    info.path = fullPath;
    info.handle = hMod;
    g_loadedPlugins.push_back(info);

    Log("[PluginLoader] [SUCCESS] Loaded '%s' at 0x%p\n", displayName.c_str(), hMod);

    // Optional entry-point hook: PluginInit()
    typedef void (*PluginInit_t)();
    PluginInit_t pfnInit = reinterpret_cast<PluginInit_t>(GetProcAddress(hMod, "PluginInit"));
    if (!pfnInit) {
        pfnInit = reinterpret_cast<PluginInit_t>(GetProcAddress(hMod, "ModInit"));
    }

    if (pfnInit) {
        Log("[PluginLoader] Calling PluginInit() for %s...\n", displayName.c_str());
        pfnInit();
    }
}

void LoadPlugins() {
    char exePath[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
        Log("[PluginLoader] [ERROR] Failed to retrieve module file path\n");
        return;
    }

    std::string gameDir = exePath;
    size_t slash = gameDir.find_last_of("\\/");
    if (slash != std::string::npos) {
        gameDir = gameDir.substr(0, slash);
    }

    std::string pluginsDir = gameDir + "\\plugins";

    // Create the plugins directory if it does not exist
    if (!CreateDirectoryA(pluginsDir.c_str(), NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            Log("[PluginLoader] [WARN] Could not create plugins directory '%s' (Win32 Error: %lu)\n",
                pluginsDir.c_str(), err);
        }
    }

    Log("[PluginLoader] ====================================================\n");
    Log("[PluginLoader] Scanning for plugins in: %s\n", pluginsDir.c_str());

    // 1. Scan subdirectories: plugins/<PluginFolder>/*.dll
    // Allows each plugin to have its own dedicated directory for configs, assets, etc.
    std::string subDirPattern = pluginsDir + "\\*";
    WIN32_FIND_DATAA fdSub;
    HANDLE hSub = FindFirstFileA(subDirPattern.c_str(), &fdSub);
    if (hSub != INVALID_HANDLE_VALUE) {
        do {
            if ((fdSub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                std::strcmp(fdSub.cFileName, ".") != 0 &&
                std::strcmp(fdSub.cFileName, "..") != 0) {

                std::string folderName = fdSub.cFileName;

                // Skip directories starting with '.' or '_' (hidden/disabled)
                if (folderName[0] == '.' || folderName[0] == '_' || EndsWithIgnoreCase(folderName, ".disabled")) {
                    continue;
                }

                std::string pluginFolder = pluginsDir + "\\" + folderName;
                std::string dllPattern = pluginFolder + "\\*.dll";

                WIN32_FIND_DATAA fdDll;
                HANDLE hDll = FindFirstFileA(dllPattern.c_str(), &fdDll);
                if (hDll != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(fdDll.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                            std::string dllName = fdDll.cFileName;
                            if (!EndsWithIgnoreCase(dllName, ".disabled") && dllName[0] != '_') {
                                std::string fullDllPath = pluginFolder + "\\" + dllName;
                                std::string displayName = folderName + "/" + dllName;
                                LoadSinglePlugin(fullDllPath, displayName);
                            }
                        }
                    } while (FindNextFileA(hDll, &fdDll));
                    FindClose(hDll);
                }
            }
        } while (FindNextFileA(hSub, &fdSub));
        FindClose(hSub);
    }

    // 2. Scan root of plugins: plugins/*.dll (for loose single-file plugins)
    std::string loosePattern = pluginsDir + "\\*.dll";
    WIN32_FIND_DATAA fdLoose;
    HANDLE hLoose = FindFirstFileA(loosePattern.c_str(), &fdLoose);
    if (hLoose != INVALID_HANDLE_VALUE) {
        do {
            if (!(fdLoose.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::string dllName = fdLoose.cFileName;
                if (!EndsWithIgnoreCase(dllName, ".disabled") && dllName[0] != '_') {
                    std::string fullDllPath = pluginsDir + "\\" + dllName;
                    LoadSinglePlugin(fullDllPath, dllName);
                }
            }
        } while (FindNextFileA(hLoose, &fdLoose));
        FindClose(hLoose);
    }

    Log("[PluginLoader] Plugin scan complete. Loaded %zu mod plugin(s).\n", g_loadedPlugins.size());
    Log("[PluginLoader] ====================================================\n");
}

const std::vector<PluginInfo>& GetLoadedPlugins() {
    return g_loadedPlugins;
}

} // namespace PluginLoader
