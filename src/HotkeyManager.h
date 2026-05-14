#pragma once

namespace HotkeyManager
{
    // Register an InputEvent sink with BSInputDeviceManager. Idempotent.
    void Install();

    // Optional: explicit removal (for plugin unload). Currently a no-op because
    // SKSE plugins don't unload mid-session, but harmless if SKSE ever changes.
    void Uninstall();
}
