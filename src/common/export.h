#pragma once

// The library is built with -fvisibility=hidden; only the two loader
// negotiation entry points are exported (exports.map lists them too).
#if defined(_WIN32)
#define VRSFOV_EXPORT __declspec(dllexport)
#else
#define VRSFOV_EXPORT __attribute__((visibility("default")))
#endif
