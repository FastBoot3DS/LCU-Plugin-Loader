# LCU Plugin Loader: DirectInput8 Proxy & Mod Loader

A lightweight DirectInput8 (`dinput8.dll`) proxy DLL and plugin loader for **LEGO City Undercover** (PC / DirectX 11).

This tool acts as the root entry point for the game process, automatically discovering and loading mod plugins from a `plugins/` folder without requiring separate system DLL proxy names (e.g. `dxgi.dll`, `d3d11.dll`) or fighting with tools like ReShade or Special K.

## PLEASE READ

Due to the way these plugins work please only install plugins from sources you really do trust because at these plugins are just like any other DLL file and is able to execute code.

I strongly recommend only installing a plugin you have built your self from a public open source repo from someone trust worthy or take the time to understand what a plugin is doing and general common sense for noticing malicious intent behind code.  

---

## Table of Contents
- [Features](#features)
- [Requirements](#requirements)
- [Building](#building)
  - [Option A: Via VS Code (Recommended)](#option-a-via-vs-code-recommended)
  - [Option B: Via Command Line](#option-b-via-command-line)
- [Installation & Setup](#installation--setup)
- [Plugin Organization Guide](#plugin-organization-guide)
  - [Folder Structure](#folder-structure)
  - [Disabling Plugins](#disabling-plugins)
- [Plugin Developer API](#plugin-developer-api)
  - [Lifecycle Hooks](#lifecycle-hooks)
  - [Shared Logging](#shared-logging)
- [Repository Structure](#repository-structure)

---

## Features

* **Zero DLL Conflicts**: Loads unlimited mod plugins dynamically into the game without needing additional proxy wrappers.
* **Folder-Per-Plugin Architecture**: Mods can live in their own dedicated folders alongside config files (`.ini`, `.json`), assets, and dependencies.
* **Altered Search Path**: Uses `LOAD_WITH_ALTERED_SEARCH_PATH` so plugins inside subdirectories can resolve their own private dependency DLLs automatically.
* **Non-Blocking Async Init**: Plugin discovery and loading runs on a background worker thread during startup to avoid hanging the game's initial window creation.
* **Thread-Safe Unified Logging**: Exports `PluginLog` from `dinput8.dll` with SRWLock protection so all plugins can output to `PluginLoader.log`.
* **Instant Toggle**: Disable any mod or folder instantly by appending `.disabled` or prefixing with `_`.

---

## Requirements

* **Python 3.8+** (for executing `build.py`)
* **x86_64 MinGW-w64 GCC Toolchain** (`g++` targeting Windows 64-bit, e.g. via [MSYS2 UCRT64 / MINGW64](https://www.msys2.org/))
  * Must be present in your `PATH`, or specified via the `CXX` environment variable.

---

## Building

### Option A: Via VS Code (Recommended)

1. Open the project folder in Visual Studio Code.
2. Press `Ctrl + Shift + B` (or open the Command Palette with `Ctrl + Shift + P` and select **Tasks: Run Build Task**).
3. Select **LCU Plugin Loader: Build all**.
4. The output DLL will be generated in `bin/dinput8.dll`.

### Option B: Via Command Line

Run `build.py` directly using Python:

```powershell
python build.py
```

#### Custom Compiler:
If `g++` is not in your system `PATH` or you wish to use a specific MinGW compiler:

```powershell
# In PowerShell:
$env:CXX = "C:\msys64\ucrt64\bin\g++.exe"
python build.py

# In Command Prompt (cmd.exe):
set CXX=C:\msys64\ucrt64\bin\g++.exe
python build.py
```

The script verifies:
1. Source files exist (`src/PluginLoader.cpp`, `src/main.cpp`).
2. The compiler targets `x86_64-w64-mingw32` or Windows.
3. Compiles with `-O2 -std=c++17 -static -static-libgcc -static-libstdc++`.
4. Outputs the finished binary to `bin/dinput8.dll`.

---

## Installation & Setup

1. Copy the compiled `bin/dinput8.dll` into your game installation directory alongside `LEGOLCUR_DX11.exe`:
   ```text
   steamapps/common/LEGO City Undercover/
   ├── LEGOLCUR_DX11.exe
   ├── dinput8.dll
   └── plugins/
   ```
2. Place any mod plugins into the `plugins/` folder (the loader will also create this directory automatically on first launch if missing).
3. Launch the game normally via Steam or `LEGOLCUR_DX11.exe`.
4. Check `PluginLoader.log` in the game directory to verify that plugins loaded successfully.

---

## Plugin Organization Guide

### Folder Structure

The loader supports both dedicated directories and standalone loose DLLs:

```text
LEGO City Undercover/
├── LEGOLCUR_DX11.exe
├── dinput8.dll                      <-- Plugin Loader
├── PluginLoader.log                 <-- Output log
└── plugins/
    ├── TowTruckTweaks/              <-- Dedicated folder
    │   ├── TowTruckTweaks.dll       <-- Mod binary
    │   └── config.ini               <-- Mod settings
    ├── MultiplayerMod/              <-- Dedicated folder
    │   ├── MultiplayerMod.dll
    │   └── network_settings.json
    └── StandaloneTool.dll           <-- Standalone loose plugin
```

### Disabling Plugins

To temporarily disable a mod without deleting it:
* **Disable a folder**: Append `.disabled` (e.g. `plugins/MultiplayerMod.disabled/`) or prefix with `_` (e.g. `plugins/_MultiplayerMod/`).
* **Disable a DLL**: Append `.disabled` (e.g. `plugins/TowTruckTweaks/TowTruckTweaks.dll.disabled`) or prefix with `_` (e.g. `_StandaloneTool.dll`).

The loader will automatically ignore these files and folders during discovery.

---

## Plugin Developer API

### Lifecycle Hooks

When a plugin is loaded, Windows calls its standard `DllMain(DLL_PROCESS_ATTACH)` routine.

Additionally, the loader checks for an exported `PluginInit` (or `ModInit`) function:

```cpp
#include <windows.h>

extern "C" __declspec(dllexport) void PluginInit() {
    // Optional initialization routine called immediately after LoadLibrary
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        // Standard DllMain entry point
    }
    return TRUE;
}
```

### Shared Logging

`dinput8.dll` exports `PluginLog(const char* format, ...)`. If you want your plugin to write to the common `PluginLoader.log`:

```cpp
#include <windows.h>

typedef void (*PluginLog_t)(const char* format, ...);

void LogToGame(const char* msg) {
    HMODULE hLoader = GetModuleHandleA("dinput8.dll");
    if (hLoader) {
        PluginLog_t pfnLog = reinterpret_cast<PluginLog_t>(GetProcAddress(hLoader, "PluginLog"));
        if (pfnLog) {
            pfnLog("[MyMod] %s\n", msg);
        }
    }
}
```

---

## Repository Structure

```text
LCU-PluginLoader/
├── .vscode/
│   └── tasks.json                   <-- VS Code default build task ("Ctrl+Shift+B")
├── bin/
│   └── dinput8.dll                  <-- Built proxy DLL binary
├── include/
│   └── PluginLoader.h               <-- Plugin loader interface
├── src/
│   ├── main.cpp                     <-- DirectInput8 proxy exports, DllMain & logging
│   └── PluginLoader.cpp             <-- Plugin scanner & loader implementation
├── build.py                         <-- Automated Python build script
└── README.md                        <-- This documentation
```
