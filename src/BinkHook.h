#pragma once

namespace BinkHook
{
    bool Install();
    bool HandleWindowMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
}
