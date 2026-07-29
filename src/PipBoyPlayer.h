#pragma once

#include "F4SEMinimal.h"

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace PipBoyPlayer
{
    bool InitializeScaleform(const F4SEMinimal::Interface* f4se);
    void Shutdown();

    [[nodiscard]] bool Active() noexcept;
    [[nodiscard]] bool CommandDetectionReady() noexcept;
    void PollScaleformCommand();
    void Activate();
    void Deactivate();
    void TickPointer();
    bool HandleWindowMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

    // Called after Scaleform unbinds the Pip-Boy render target.
    bool UploadFrame(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* target);
}
