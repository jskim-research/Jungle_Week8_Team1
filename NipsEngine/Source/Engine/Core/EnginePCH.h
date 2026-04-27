#pragma once

// Keep the PCH limited to stable platform, STL, and low-level engine types.
// Frequently edited engine modules should stay out of this file.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "Core/CoreMinimal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <forward_list>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <Windows.h>
#include <windowsx.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
