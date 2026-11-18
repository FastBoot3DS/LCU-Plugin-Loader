#include "PluginLoader.h"
#include "PluginAPI.h"
#include <windows.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern void Log(const char* fmt, ...);

namespace PluginLoader {

static std::vector<PluginInfo> g_loadedPlugins;
static SRWLOCK g_pluginLogLock = SRWLOCK_INIT;

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string repository;
    std::string entryDll;
};

struct PluginLoggerContext {
    std::string id;
    std::string name;
    std::string logPath;
};

static std::vector<std::unique_ptr<PluginLoggerContext>> g_pluginLoggers;

static bool EndsWithIgnoreCase(const std::string& str, const std::string& suffix);

static void AppendUtf8(std::string& output, unsigned int codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

class FlatJsonObjectParser {
public:
    explicit FlatJsonObjectParser(const std::string& input) : input_(input), position_(0) {}

    bool Parse(std::map<std::string, std::string>& values, std::string& error) {
        SkipWhitespace();
        if (!Consume('{')) return Fail(error, "expected '{'");
        SkipWhitespace();
        if (Consume('}')) return Finish(error);

        while (true) {
            std::string key;
            std::string value;
            if (!ParseString(key, error)) return false;
            SkipWhitespace();
            if (!Consume(':')) return Fail(error, "expected ':'");
            SkipWhitespace();
            if (!ParseString(value, error)) return false;
            if (!values.insert(std::make_pair(key, value)).second) {
                return Fail(error, "duplicate property '" + key + "'");
            }
            SkipWhitespace();
            if (Consume('}')) return Finish(error);
            if (!Consume(',')) return Fail(error, "expected ',' or '}'");
            SkipWhitespace();
        }
    }

private:
    bool ParseString(std::string& value, std::string& error) {
        if (!Consume('"')) return Fail(error, "expected a string");
        while (position_ < input_.size()) {
            const unsigned char current = static_cast<unsigned char>(input_[position_++]);
            if (current == '"') return true;
            if (current < 0x20) return Fail(error, "unescaped control character");
            if (current != '\\') {
                value.push_back(static_cast<char>(current));
                continue;
            }

            if (position_ >= input_.size()) return Fail(error, "incomplete escape");
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                unsigned int codePoint = 0;
                if (!ParseHexCodePoint(codePoint, error)) return false;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (position_ + 2 > input_.size() ||
                        input_[position_] != '\\' || input_[position_ + 1] != 'u') {
                        return Fail(error, "unpaired high surrogate");
                    }
                    position_ += 2;
                    unsigned int lowSurrogate = 0;
                    if (!ParseHexCodePoint(lowSurrogate, error)) return false;
                    if (lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF) {
                        return Fail(error, "invalid low surrogate");
                    }
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) +
                        (lowSurrogate - 0xDC00);
                } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                    return Fail(error, "unpaired low surrogate");
                }
                AppendUtf8(value, codePoint);
                break;
            }
            default:
                return Fail(error, "invalid escape sequence");
            }
        }
        return Fail(error, "unterminated string");
    }

    bool ParseHexCodePoint(unsigned int& codePoint, std::string& error) {
        if (position_ + 4 > input_.size()) return Fail(error, "incomplete unicode escape");
        codePoint = 0;
        for (int i = 0; i < 4; ++i) {
            const char digit = input_[position_++];
            codePoint <<= 4;
            if (digit >= '0' && digit <= '9') codePoint |= digit - '0';
            else if (digit >= 'a' && digit <= 'f') codePoint |= digit - 'a' + 10;
            else if (digit >= 'A' && digit <= 'F') codePoint |= digit - 'A' + 10;
            else return Fail(error, "invalid unicode escape");
        }
        return true;
    }

    bool Finish(std::string& error) {
        SkipWhitespace();
        return position_ == input_.size() || Fail(error, "unexpected trailing data");
    }

    void SkipWhitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) {
            ++position_;
        }
    }

    bool Consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool Fail(std::string& error, const std::string& message) {
        error = message;
        return false;
    }

    const std::string& input_;
    size_t position_;
};

static bool IsValidPluginId(const std::string& id) {
    if (id.empty() ||
        !((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9'))) {
        return false;
    }
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

static bool HasHttpUrlScheme(const std::string& url) {
    return (url.compare(0, 7, "http://") == 0 && url.size() > 7) ||
        (url.compare(0, 8, "https://") == 0 && url.size() > 8);
}

static bool ReadPluginManifest(
    const std::string& manifestPath,
    PluginManifest& manifest,
    std::string& error)
{
    std::ifstream file(manifestPath.c_str(), std::ios::binary);
    if (!file) {
        error = "could not open plugin.json";
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff length = file.tellg();
    if (length < 0 || length > 64 * 1024) {
        error = "plugin.json is larger than 64 KiB or could not be read";
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::string contents(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    if (contents.size() >= 3 &&
        static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB &&
        static_cast<unsigned char>(contents[2]) == 0xBF) {
        contents.erase(0, 3);
    }

    std::map<std::string, std::string> values;
    FlatJsonObjectParser parser(contents);
    if (!parser.Parse(values, error)) return false;

    const char* requiredProperties[] = {
        "schemaVersion", "id", "name", "version", "author", "entryDll"
    };
    for (size_t i = 0; i < sizeof(requiredProperties) / sizeof(requiredProperties[0]); ++i) {
        if (values.find(requiredProperties[i]) == values.end() ||
            values[requiredProperties[i]].empty()) {
            error = std::string("missing or empty property '") + requiredProperties[i] + "'";
            return false;
        }
    }
    if (values["schemaVersion"] != "1") {
        error = "unsupported schemaVersion (expected \"1\")";
        return false;
    }
    if (!IsValidPluginId(values["id"])) {
        error = "id must contain lowercase letters, digits, '-' or '_' and start with a letter or digit";
        return false;
    }
    if (values["entryDll"].find_first_of("\\/:") != std::string::npos ||
        values["entryDll"] == "." || values["entryDll"] == ".." ||
        values["entryDll"][0] == '_' ||
        !EndsWithIgnoreCase(values["entryDll"], ".dll")) {
        error = "entryDll must be an enabled DLL filename in the plugin folder";
        return false;
    }
    const char* urlProperties[] = { "repository" };
    for (size_t i = 0; i < sizeof(urlProperties) / sizeof(urlProperties[0]); ++i) {
        const std::map<std::string, std::string>::const_iterator url = values.find(urlProperties[i]);
        if (url != values.end() && !url->second.empty() && !HasHttpUrlScheme(url->second)) {
            error = std::string("'") + urlProperties[i] + "' must use http:// or https://";
            return false;
        }
    }

    manifest.id = values["id"];
    manifest.name = values["name"];
    manifest.version = values["version"];
    manifest.author = values["author"];
    manifest.description = values["description"];
    manifest.repository = values["repository"];
    manifest.entryDll = values["entryDll"];
    return true;
}

static bool EnsurePluginLogDirectory(const std::string& path) {
    if (CreateDirectoryA(path.c_str(), NULL)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return false;
}

static void PluginLogToFile(
    void* loggerContext,
    PluginLogLevel level,
    const char* format,
    ...)
{
    if (loggerContext == NULL || format == NULL) return;
    const PluginLoggerContext* context =
        static_cast<const PluginLoggerContext*>(loggerContext);

    char message[2048];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0) {
        OutputDebugStringA("[PluginLoader] Plugin log formatting failed.\n");
        return;
    }
    message[sizeof(message) - 1] = '\0';

    const char* levelName = "INFO";
    switch (level) {
    case PLUGIN_LOG_DEBUG: levelName = "DEBUG"; break;
    case PLUGIN_LOG_INFO: levelName = "INFO"; break;
    case PLUGIN_LOG_WARNING: levelName = "WARNING"; break;
    case PLUGIN_LOG_ERROR: levelName = "ERROR"; break;
    default: levelName = "UNKNOWN"; break;
    }

    AcquireSRWLockExclusive(&g_pluginLogLock);
    FILE* file = std::fopen(context->logPath.c_str(), "a");
    if (file != NULL) {
        std::fprintf(file, "[%s] ", levelName);
        std::fputs(message, file);
        const size_t messageLength = std::strlen(message);
        if (messageLength == 0 || message[messageLength - 1] != '\n') {
            std::fputc('\n', file);
        }
        std::fclose(file);
    } else {
        OutputDebugStringA("[PluginLoader] Could not open a plugin log file.\n");
    }
    ReleaseSRWLockExclusive(&g_pluginLogLock);
}

static bool EndsWithIgnoreCase(const std::string& str, const std::string& suffix) {
    if (str.length() < suffix.length()) return false;
    size_t offset = str.length() - suffix.length();
    for (size_t i = 0; i < suffix.length(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[offset + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

static void LoadSinglePlugin(
    const std::string& fullPath,
    const std::string& displayName,
    const PluginManifest* manifest = NULL,
    const std::string& pluginFolder = std::string())
{
    Log("[PluginLoader] Loading plugin: %s (%s)...\n", displayName.c_str(), fullPath.c_str());

    std::unique_ptr<PluginLoggerContext> loggerContext;
    PluginLoaderAPI pluginApi = {};
    if (manifest != NULL) {
        const std::string logsDir = pluginFolder + "\\logs";
        if (!EnsurePluginLogDirectory(logsDir)) {
            Log("[PluginLoader] [ERROR] Could not create plugin log directory '%s' (Win32 Error: %lu)\n",
                logsDir.c_str(), GetLastError());
            return;
        }
        loggerContext.reset(new PluginLoggerContext());
        loggerContext->id = manifest->id;
        loggerContext->name = manifest->name;
        loggerContext->logPath = logsDir + "\\" + manifest->id + ".log";
        pluginApi.version = 1;
        pluginApi.pluginId = loggerContext->id.c_str();
        pluginApi.pluginName = loggerContext->name.c_str();
        pluginApi.loggerContext = loggerContext.get();
        pluginApi.log = PluginLogToFile;
    }

    HMODULE hMod = LoadLibraryExA(fullPath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hMod) {
        DWORD err = GetLastError();
        Log("[PluginLoader] [ERROR] Failed to load %s (Win32 Error: %lu)\n", displayName.c_str(), err);
        return;
    }

    PluginInfo info;
    info.id = manifest != NULL ? manifest->id : std::string();
    info.name = manifest != NULL ? manifest->name : displayName;
    info.version = manifest != NULL ? manifest->version : std::string();
    info.author = manifest != NULL ? manifest->author : std::string();
    info.description = manifest != NULL ? manifest->description : std::string();
    info.repository = manifest != NULL ? manifest->repository : std::string();
    info.path = fullPath;
    info.handle = hMod;
    g_loadedPlugins.push_back(info);
    if (loggerContext) {
        g_pluginLoggers.push_back(std::move(loggerContext));
    }

    Log("[PluginLoader] [SUCCESS] Loaded '%s' at 0x%p\n", info.name.c_str(), hMod);

    // Manifest plugins can opt into the versioned API without breaking legacy entry points.
    typedef void (*PluginInitV2_t)(const PluginLoaderAPI*);
    PluginInitV2_t pfnInitV2 = NULL;
    if (manifest != NULL) {
        pfnInitV2 = reinterpret_cast<PluginInitV2_t>(GetProcAddress(hMod, "PluginInitV2"));
    }
    typedef void (*PluginInit_t)();
    PluginInit_t pfnInit = NULL;
    if (pfnInitV2 == NULL) {
        pfnInit = reinterpret_cast<PluginInit_t>(GetProcAddress(hMod, "PluginInit"));
    }
    if (pfnInit == NULL && pfnInitV2 == NULL) {
        pfnInit = reinterpret_cast<PluginInit_t>(GetProcAddress(hMod, "ModInit"));
    }

    if (pfnInitV2 != NULL) {
        Log("[PluginLoader] Calling PluginInitV2() for %s...\n", info.name.c_str());
        pfnInitV2(&pluginApi);
    } else if (pfnInit != NULL) {
        Log("[PluginLoader] Calling plugin initialization for %s...\n", info.name.c_str());
        pfnInit();
    }
}

static bool IsPluginIdLoaded(const std::string& id) {
    for (size_t i = 0; i < g_loadedPlugins.size(); ++i) {
        if (g_loadedPlugins[i].id == id) return true;
    }
    return false;
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
                const std::string manifestPath = pluginFolder + "\\plugin.json";
                const DWORD manifestAttributes = GetFileAttributesA(manifestPath.c_str());
                if (manifestAttributes != INVALID_FILE_ATTRIBUTES) {
                    if ((manifestAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                        Log("[PluginLoader] [ERROR] '%s' is not a plugin.json file; skipping folder\n",
                            manifestPath.c_str());
                        continue;
                    }
                    PluginManifest manifest;
                    std::string error;
                    if (!ReadPluginManifest(manifestPath, manifest, error)) {
                        Log("[PluginLoader] [ERROR] Invalid manifest '%s': %s; skipping folder\n",
                            manifestPath.c_str(), error.c_str());
                        continue;
                    }
                    if (IsPluginIdLoaded(manifest.id)) {
                        Log("[PluginLoader] [ERROR] Duplicate plugin id '%s'; skipping folder\n",
                            manifest.id.c_str());
                        continue;
                    }
                    const std::string fullDllPath = pluginFolder + "\\" + manifest.entryDll;
                    const DWORD dllAttributes = GetFileAttributesA(fullDllPath.c_str());
                    if (dllAttributes == INVALID_FILE_ATTRIBUTES ||
                        (dllAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                        Log("[PluginLoader] [ERROR] Manifest entry DLL '%s' does not exist; skipping plugin\n",
                            fullDllPath.c_str());
                        continue;
                    }
                    LoadSinglePlugin(fullDllPath, manifest.name, &manifest, pluginFolder);
                    continue;
                }
                const DWORD manifestError = GetLastError();
                if (manifestError != ERROR_FILE_NOT_FOUND && manifestError != ERROR_PATH_NOT_FOUND) {
                    Log("[PluginLoader] [ERROR] Could not inspect manifest '%s' (Win32 Error: %lu); skipping folder\n",
                        manifestPath.c_str(), manifestError);
                    continue;
                }

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
