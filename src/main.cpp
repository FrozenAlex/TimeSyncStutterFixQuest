#include "main.hpp"

#include "scotland2/shared/modloader.h"
#include "logging.hpp"
#include "GlobalNamespace/AudioTimeSyncController.hpp"
#include "UnityEngine/Time.hpp"
// Include the modloader header, which allows us to tell the modloader which mod
// this is, and the version etc.
#include "scotland2/shared/modloader.h"

// beatsaber-hook is a modding framework that lets us call functions and fetch
// field values from in the game It also allows creating objects, configuration,
// and importantly, hooking methods to modify their values
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "beatsaber-hook/shared/utils/logging.hpp"

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};
// Stores the ID and version of our mod, and is sent to
// the modloader upon startup

// Called at the early stages of game loading
MOD_EXTERN_FUNC void setup(CModInfo *info) noexcept {
  *info = modInfo.to_c();

  INFO("Completed setup!");
}

// First method
// MAKE_HOOK_MATCH(
//     AudioTimeSyncController_Awake,
//     &GlobalNamespace::AudioTimeSyncController::Awake,
//     void,
//     GlobalNamespace::AudioTimeSyncController* self
// ) {
//     // DEBUG("Set forced sync delta time to {}", self->_forcedSyncDeltaTime);
//     self->_forcedSyncDeltaTime = 0.1f; // 100ms
//     AudioTimeSyncController_Awake(self);
// };

// Second method 
MAKE_HOOK_MATCH(
    AudioTimeSyncController_Start,
    &GlobalNamespace::AudioTimeSyncController::Start,
    void,
    GlobalNamespace::AudioTimeSyncController* self
) {
    AudioTimeSyncController_Start(self);
    self->_startSyncDeltaTime = 0.2f; // 200ms
    self->_forcedSyncDeltaTime = 0.01f; // 10ms
}

MAKE_HOOK_MATCH(
    AudioTimeSyncController_Update,
    &GlobalNamespace::AudioTimeSyncController::Update,
    void,
    GlobalNamespace::AudioTimeSyncController* self
) {
    AudioTimeSyncController_Update(self);
    if (
      (self->songTime > 0.6f && UnityEngine::Time::get_timeSinceLevelLoad() > 1.0f) 
      || self->songLength < 15.0f)
    {
        self->_forcedSyncDeltaTime = 0.3f; // 300ms
    }
}

// Called later on in the game loading - a good time to install function hooks
MOD_EXTERN_FUNC void late_load() noexcept {
  il2cpp_functions::Init();

  INFO("Installing hooks...");

  // First method hook
  // INSTALL_HOOK(Logger, AudioTimeSyncController_Awake);

  // Second method hook
  INSTALL_HOOK(Logger, AudioTimeSyncController_Start);
  INSTALL_HOOK(Logger, AudioTimeSyncController_Update);

  INFO("Installed all hooks!");
}

