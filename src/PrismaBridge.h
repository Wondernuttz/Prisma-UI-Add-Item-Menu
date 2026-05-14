#pragma once

#include <cstdint>

namespace PrismaBridge
{
    // Acquire the PrismaUI API and create our view. Idempotent. Safe to call
    // multiple times (e.g. from kPostLoad and again after kDataLoaded).
    bool Initialize();

    // Bring the menu up — Show + Focus. Pause behavior follows Settings.pauseGameWhileOpen.
    void OpenMenu();

    // Tear down the focus + hide. Game resumes if it was paused.
    void CloseMenu();

    // True if the menu is currently shown AND focused.
    bool IsMenuOpen();

    // Shutdown — destroy the view, release pointers. Called on plugin unload.
    void Shutdown();
}
