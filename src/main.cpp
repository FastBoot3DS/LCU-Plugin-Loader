#include "PluginLoader.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

using DirectInput8Create_t =
    HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

INIT_ONCE g_dinputInitOnce = INIT_ONCE_STATIC_INIT;
DirectInput8Create_t g_realDirectInput8Create = nullptr;
SRWLOCK g_logLock = SRWLOCK_INIT;

BOOL CALLBACK LoadRealDirectInput(PINIT_ONCE, PVOID, PVOID*) {
    char systemPath[MAX_PATH];
    const UINT length = GetSystemDirectoryA(systemPath, MAX_PATH);
    constexpr char suffix[] = "\\dinput8.dll";

    if (length == 0 || length + sizeof(suffix) > MAX_PATH) {
        return TRUE;
    }

    std::memcpy(systemPath + length, suffix, sizeof(suffix));
    HMODULE module = LoadLibraryA(systemPath);
    if (module != nullptr) {
        g_realDirectInput8Create = reinterpret_cast<DirectInput8Create_t>(
            GetProcAddress(module, "DirectInput8Create"));
    }

    return TRUE;
}

DWORD WINAPI LoadPluginsThread(LPVOID) {
    PluginLoader::LoadPlugins();
    return 0;
}

} // namespace

void Log(const char* format, ...) {
    char message[2048];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (length < 0) {
        OutputDebugStringA("[PluginLoader] Log formatting failed.\n");
        return;
    }

    message[sizeof(message) - 1] = '\0';
    AcquireSRWLockExclusive(&g_logLock);
    FILE* file = std::fopen("PluginLoader.log", "a");
    if (file != nullptr) {
        std::fputs(message, file);
        std::fclose(file);
    } else {
        OutputDebugStringA("[PluginLoader] Could not open PluginLoader.log.\n");
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance,
    DWORD version,
    REFIID interfaceId,
    LPVOID* output,
    LPUNKNOWN outer)
{
    InitOnceExecuteOnce(&g_dinputInitOnce, LoadRealDirectInput, nullptr, nullptr);
    if (g_realDirectInput8Create == nullptr) {
        return E_FAIL;
    }
    return g_realDirectInput8Create(instance, version, interfaceId, output, outer);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, LoadPluginsThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        } else {
            OutputDebugStringA("[PluginLoader] Could not start plugin loader thread.\n");
        }
    }
    return TRUE;
}
