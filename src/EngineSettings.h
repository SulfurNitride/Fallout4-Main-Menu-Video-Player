#pragma once

#include "F4SEMinimal.h"

namespace EngineSettings
{
    bool Initialize(
        const F4SEMinimal::Interface* f4se,
        std::uint32_t runtimeVersion);
    bool Apply();
    void BeginMainMenu();
    void EndMainMenu();
    [[nodiscard]] bool IsBorderlessMode() noexcept;
}
