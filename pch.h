#pragma once

// Suppress benign nonstandard-extension warnings from third-party/SDK headers
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4200) // zero-sized array in struct/union
#pragma warning(disable:4201) // nameless struct/union
#endif

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace logs = SKSE::log;
using namespace std::literals;