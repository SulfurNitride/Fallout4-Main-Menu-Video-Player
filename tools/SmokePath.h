#pragma once

#include <string>
#include <string_view>

#include <Windows.h>

[[nodiscard]] inline std::string WidePathToUtf8(const wchar_t* value)
{
    const std::wstring_view wide{ value };
    if (wide.empty()) {
        return {};
    }

    const int byteCount = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (byteCount <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(byteCount), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            wide.data(),
            static_cast<int>(wide.size()),
            utf8.data(),
            byteCount,
            nullptr,
            nullptr) != byteCount) {
        return {};
    }
    return utf8;
}
