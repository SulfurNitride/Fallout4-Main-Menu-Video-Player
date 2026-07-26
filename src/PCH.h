#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

using namespace std::literals;
