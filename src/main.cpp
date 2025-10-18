#include "main.hpp"

#include "scotland2/shared/modloader.h"
#include "logging.hpp"
#include "GlobalNamespace/AudioTimeSyncController.hpp"
#include "UnityEngine/AudioSettings.hpp"
#include "UnityEngine/AudioSource.hpp"
#include "UnityEngine/AudioClip.hpp"
#include "GlobalNamespace/IAudioTimeSource.hpp"
#include "GlobalNamespace/IDspTimeProvider.hpp"
#include "UnityEngine/Time.hpp"
#include <math.h>
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
#include <cmath>

// Use Unity's DSP time instead of raw DSP time
const bool UnityDSP = false;

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};
// Stores the ID and version of our mod, and is sent to
// the modloader upon startup

static char* gOutput = nullptr;

// Called at the early stages of game loading
MOD_EXTERN_FUNC void setup(CModInfo *info) noexcept {
  *info = modInfo.to_c();

  INFO("Completed setup!");
}

// First method
MAKE_HOOK_MATCH(
    AudioTimeSyncController_Awake,
    &GlobalNamespace::AudioTimeSyncController::Awake,
    void,
    GlobalNamespace::AudioTimeSyncController* self
) {
    // DEBUG("Set forced sync delta time to {}", self->_forcedSyncDeltaTime);
    // Force sync in the beginning of the song to be very tight to prevent initial desync (for 0.6s)
    self->_forcedSyncDeltaTime = 0.01f; // 10ms
    // Desync at which we start smooth correction of audio time
    self->_startSyncDeltaTime = 0.1f; // 100ms
    AudioTimeSyncController_Awake(self);
};

MAKE_HOOK_NO_CATCH(fmod_output_mix, 0x0, int, char* output, void* p1, uint p2) {
  if (output != gOutput) {
      DEBUG("setting output to {}", fmt::ptr(output));
      gOutput = output;
  }
  return fmod_output_mix(output, p1, p2);
}

long GetDSPClock() {
    char* system = *(char**) (gOutput + 0x60);
    return *(long*) (system + 0xc78);
}


// Third method
MAKE_HOOK_MATCH(
    AudioTimeSyncController_Update,
    &GlobalNamespace::AudioTimeSyncController::Update,
    void,
    GlobalNamespace::AudioTimeSyncController* self
) {
    // This offset is needed to make sure we are aligned with unity dsp time (if it goes too far off from unity then we correct it)
    static double_t dspCorrectionOffset = 0.0f;
    static double_t oldDeltaDSPUnity = 0.0f;

    // Used only for logging state changes
    if (self->_lastState != self->_state) {
      DEBUG("AudioTimeSyncController state changed from {} to {}", static_cast<int>(self->_lastState), static_cast<int>(self->_state));
      self->_lastState = self->_state;
    }

    if (self->_state == GlobalNamespace::AudioTimeSyncController::State::Stopped) {
      self->_dspTimeOffset = UnityEngine::AudioSettings::get_dspTime(); // Unity for comparison
      return;
    }

    // Delta scaled by song speed scale
    float_t _lastFrameDeltaSongTime = UnityEngine::Time::get_deltaTime() * self->_timeScale; // num1
    self->_lastFrameDeltaSongTime = _lastFrameDeltaSongTime;

    // If is running in capture framerate mode, just advance song time by delta time
    if (UnityEngine::Time::get_captureFramerate() != 0) {
      self->_songTime += _lastFrameDeltaSongTime;
      
      if (!self->_audioSource->loop) {
        self->_songTime = std::fmin(self->_songTime, self->_audioSource->clip->length - 0.01f);
        self->_audioSource->time = self->_songTime;
      } else {
        self->_audioSource->time = std::fmod(self->_songTime, self->_audioSource->clip->length);
      }
      
      self->_dspTimeOffset = self->_dspTimeProvider->dspTime - self->_songTime;
      self->_isReady = true;
      return;
    }
    
    if (self->timeSinceStart < self->_audioStartTimeOffsetSinceStart) {
      self->_songTime += _lastFrameDeltaSongTime;
      return;
    }
    
    if (!self->_audioStarted) {
      self->_audioStarted = true;
      self->_audioSource->Play();
    }
    
    // Error reporting and early exit if audio is not playing
    if (!self->_audioSource->clip || 
      (!self->_audioSource->isPlaying && !self->_forceNoAudioSyncOrAudioSyncErrorFixing)) 
    {

      // If fail reports less than 5 and state is playing, report
      if (self->_failReportCount < 5 && self->_state ==::GlobalNamespace::AudioTimeSyncController_State::Playing) {
        self->_failReportCount++;
        if (!self->_audioSource->clip) {
            ERROR("[2363] AudioTimeSyncController: audio clip is null DSP={} time={} isPlaying={} songTime={}", 
            self->_dspTimeProvider->dspTime,
            self->_audioSource->time,
            self->_audioSource->isPlaying,
            self->_songTime
          );
          return;
        }

        if (self->_songTime < self->_audioSource->clip->length) {
          ERROR("[2363] AudioTimeSyncController: audio should be playing DSP={} time={} isPlaying={} songTime={} audioTime={}", 
            self->_dspTimeProvider->dspTime,
            self->_audioSource->time,
            self->_audioSource->isPlaying,
            self->_songTime,
            self->_audioSource->time
          );
        }
      }
      return;
    }

    // timeSamples is used here to have an exact measurement of whether or not the audioSource's reported time has been updated
    int timeSamples = self->_audioSource->timeSamples;
    
    // Attempt to sync this time to DSP
    static float_t dspAudioSourceOffset = 0.0f;
    float_t audioSourceTime = self->_audioSource->time; // clock with no offset so far
    // Smooth out AudioSource time using DSP clock
    {
      double_t dspRawTime = GetDSPClock() / static_cast<double_t>(UnityEngine::AudioSettings::get_outputSampleRate());
      
      double_t delta = fabs(dspRawTime + dspAudioSourceOffset - audioSourceTime);
      if (delta > 0.5f) {
        dspAudioSourceOffset = audioSourceTime - dspRawTime;
        DEBUG("AudioSource DSP offset set to {}", dspAudioSourceOffset);
      }
      audioSourceTime = dspRawTime + dspAudioSourceOffset;
    }
    float_t unityClockTime = self->timeSinceStart - self->_audioStartTimeOffsetSinceStart; // num3

    // idk why they keep track of loops
    if (self->_prevAudioSamplePos > timeSamples) {
      self->_playbackLoopIndex++;
    }

    // it's named pretty clearly, but basically this is to compensate for the audio thread not updating its time often enough
	  // (dsp time means Digital Signal Processor time, which is the audio)
    if (self->_prevAudioSamplePos == timeSamples) {
      self->_inBetweenDSPBufferingTimeEstimate += _lastFrameDeltaSongTime;
    } else {
      self->_inBetweenDSPBufferingTimeEstimate = 0.0f;
    }
    self->_prevAudioSamplePos = timeSamples;

    // here is where they actually apply the buffer compensation (plus loops for whatever reason)
    audioSourceTime += static_cast<float_t>(self->_playbackLoopIndex) * self->_audioSource->clip->length / self->_timeScale + self->_inBetweenDSPBufferingTimeEstimate;

    // double_t dspRawTime = GetDSPClock() / static_cast<double_t>(UnityEngine::AudioSettings::get_outputSampleRate());
    // double_t unityDspTime =
    // // Correct dsp time with offset
    // {
    //   double_t delta = fabs(dspRawTime + dspCorrectionOffset - unityDspTime);
    //   if (delta > 0.5f) {
    //     dspCorrectionOffset = unityDspTime - dspRawTime;
    //     DEBUG("DSP correction offset set to {}", dspCorrectionOffset);
    //   }

    //   // Log only if update is significant
    //   double_t newDelta = (dspRawTime + dspCorrectionOffset) - unityDspTime;
    //   if (fabs(newDelta) > 0.001f  && newDelta != oldDeltaDSPUnity) {
    //     DEBUG("Delta Unity and Raw DSP: {}", newDelta);
    //     oldDeltaDSPUnity = newDelta;
    //   }
    // }
    
    // this field isn't used for syncing the song, just sound effects such as note cuts
    self->_dspTimeOffset = UnityEngine::AudioSettings::get_dspTime();;

    if (!self->_forceNoAudioSyncOrAudioSyncErrorFixing) {
      // for some context, the _audioStartTimeOffsetSinceStart field is used as the difference between main thread time and audio thread time
		  // if everything is perfectly synced, it will be equal to timeSinceStart - audioSourceTime
      float_t deltaTime = std::fabs(unityClockTime - audioSourceTime); // num4
      // here is forced sync which can cause really sharp stutters, since it immediately teleports the time
      if (
        (deltaTime > self->_forcedSyncDeltaTime || self->_state == ::GlobalNamespace::AudioTimeSyncController_State::Paused) && 
        (!self->forcedNoAudioSync)
      ) {
        DEBUG("Forcing audio sync: unityClockTime={}, audioSourceTime={}, difference={}, audioLatency={}, songTime={}", unityClockTime, audioSourceTime, (unityClockTime - audioSourceTime), self->_audioLatency, self->_songTime);
        self->_audioStartTimeOffsetSinceStart =  self->timeSinceStart - audioSourceTime;
        unityClockTime = audioSourceTime;
      // meanwhile this code does a lerp to bring the song and notes back into sync more smoothly
      } else {
        if (self->____fixingAudioSyncError) {
          if (deltaTime < self->_stopSyncDeltaTime) {
            self->_fixingAudioSyncError = false;
          }
        } else if (deltaTime > self->_startSyncDeltaTime) {
          self->_fixingAudioSyncError = true;
        }
        if (self->_fixingAudioSyncError) {
          DEBUG("Fixing audio sync error: unityClockTime={}, audioSourceTime={}, difference={}, audioLatency={}, songTime={}", unityClockTime, audioSourceTime,  (unityClockTime - audioSourceTime), self->_audioLatency, self->_songTime);
          self->_audioStartTimeOffsetSinceStart = std::lerp(self->_audioStartTimeOffsetSinceStart, self->timeSinceStart - audioSourceTime, self->_lastFrameDeltaSongTime * self->_audioSyncLerpSpeed);
        }
      }
    }

    float_t newSongTime = std::fmax(self->_songTime, unityClockTime - (self->_songTimeOffset + self->_audioLatency));
    self->_lastFrameDeltaSongTime = newSongTime - self->_songTime;
    self->_songTime = newSongTime;
    self->_isReady = true;

    // Adjust forced sync delta time based on song length and time to prevent stutter after start of song (related to Awake hook)
    if (
      (self->songTime > 0.6f && UnityEngine::Time::get_timeSinceLevelLoad() > 1.0f) 
      || self->songLength < 15.0f)
    {
        self->_forcedSyncDeltaTime = 0.3f; // 300ms
    }
}


MOD_EXTERN_FUNC void load() noexcept {
  il2cpp_functions::Init();

  INFO("Installing hooks...");

  uintptr_t libunity = baseAddr("libunity.so");
    uintptr_t fmod_output_mix_addr = findPattern(
        libunity, "ff 43 03 d1 a8 04 80 52 ed 33 04 6d eb 2b 05 6d e9 23 06 6d fc 6f 07 a9 fa 67 08 a9 f8 5f 09 a9 f6 57 0a a9 f4 4f", 0x2000000
    );
  INFO("Found audio mix address: {}", fmod_output_mix_addr);
  INSTALL_HOOK_DIRECT(Logger, fmod_output_mix, (void*) fmod_output_mix_addr);

  INFO("Installed all hooks!");
}

// Called later on in the game loading - a good time to install function hooks
MOD_EXTERN_FUNC void late_load() noexcept {
  il2cpp_functions::Init();

  INFO("Installing hooks...");

  // First method hook
  INSTALL_HOOK(Logger, AudioTimeSyncController_Awake);

  // Second method hook
  // INSTALL_HOOK(Logger, AudioTimeSyncController_Start);
  // INSTALL_HOOK(Logger, AudioTimeSyncController_Update);

  // Third method hook
  INSTALL_HOOK(Logger, AudioTimeSyncController_Update);

  INFO("Installed all hooks!");
}

