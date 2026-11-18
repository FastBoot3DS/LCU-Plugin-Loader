#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PluginLogLevel {
    PLUGIN_LOG_DEBUG = 0,
    PLUGIN_LOG_INFO = 1,
    PLUGIN_LOG_WARNING = 2,
    PLUGIN_LOG_ERROR = 3
} PluginLogLevel;

typedef void (*PluginLogCallback)(
    void* loggerContext,
    PluginLogLevel level,
    const char* format,
    ...);

typedef struct PluginLoaderAPI {
    uint32_t version;
    const char* pluginId;
    const char* pluginName;
    void* loggerContext;
    PluginLogCallback log;
} PluginLoaderAPI;

#ifdef __cplusplus
}
#endif
