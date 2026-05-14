#include "PCH.h"
#include "HotkeyManager.h"
#include "PrismaBridge.h"
#include "Settings.h"

namespace HotkeyManager
{
    namespace
    {
        class KeyEventSink final : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            static KeyEventSink* GetSingleton()
            {
                static KeyEventSink s;
                return &s;
            }

            RE::BSEventNotifyControl ProcessEvent(
                RE::InputEvent* const* a_event,
                [[maybe_unused]] RE::BSTEventSource<RE::InputEvent*>* a_source) override
            {
                if (!a_event || !*a_event) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                Settings::ReloadIfChanged();
                const auto sets = Settings::Get();
                if (sets.toggleScancode == 0) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                for (auto* ev = *a_event; ev; ev = ev->next) {
                    if (ev->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
                        continue;
                    }

                    auto* btn = ev->AsButtonEvent();
                    if (!btn) {
                        continue;
                    }

                    if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                        continue;
                    }

                    if (!btn->IsDown()) {
                        continue;
                    }

                    if (btn->GetIDCode() != sets.toggleScancode) {
                        continue;
                    }

                    if (PrismaBridge::IsMenuOpen()) {
                        logger::info("HotkeyManager: toggle hotkey pressed; closing AddItem menu");
                        PrismaBridge::CloseMenu();
                        continue;
                    }

                    if (auto* ui = RE::UI::GetSingleton()) {
                        if (ui->GameIsPaused()) {
                            continue;
                        }
                        if (ui->IsMenuOpen(RE::Console::MENU_NAME)) {
                            continue;
                        }
                    }

                    logger::info("HotkeyManager: toggle hotkey pressed; opening AddItem menu");
                    PrismaBridge::OpenMenu();
                }

                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Install()
    {
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(KeyEventSink::GetSingleton());
            logger::info("HotkeyManager: input sink installed");
        } else {
            logger::error("HotkeyManager: BSInputDeviceManager singleton is null");
        }
    }

    void Uninstall()
    {
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->RemoveEventSink(KeyEventSink::GetSingleton());
        }
    }
}
