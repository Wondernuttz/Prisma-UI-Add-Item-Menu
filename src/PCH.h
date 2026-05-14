#pragma once

#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>
#include <REL/Relocation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace logger = SKSE::log;

using namespace std::literals;
