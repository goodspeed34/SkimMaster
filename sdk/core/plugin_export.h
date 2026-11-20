#pragma once

#ifdef _WIN32
#define SKIM_PLUGIN_EXPORT __declspec(dllexport)
#else
#define SKIM_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
