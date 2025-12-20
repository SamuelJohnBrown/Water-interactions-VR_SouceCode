// water_sound.cpp - Sound playback for water collision detection

#include "water_sound.h"
#include "water_state.h"
#include "water_utils.h"
#include "config.h"
#include "helper.h"
#include <thread>
#include <chrono>

namespace InteractiveWaterVR {

// ============================================================================
// Sound loading
// ============================================================================

RE::BGSSoundDescriptorForm* LoadSplashSoundDescriptor(SplashBand band) {
    auto idx = static_cast<size_t>(band);
    if (idx >= static_cast<size_t>(SplashBand::Count)) return nullptr;
    if (g_splashSounds[idx]) return g_splashSounds[idx];

    std::uint32_t fullId = 0;
    auto form = LoadFormAndLog<RE::BGSSoundDescriptorForm>(
        "SpellInteractionsVR.esp", fullId, kSplashFormBaseIDs[idx], "SplashSound");
    if (form) {
        g_splashSounds[idx] = form;
    }
    return g_splashSounds[idx];
}

RE::BGSSoundDescriptorForm* LoadSplashExitSoundDescriptor(SplashBand band) {
    auto idx = static_cast<size_t>(band);
    if (idx >= static_cast<size_t>(SplashBand::Count)) return nullptr;
    if (g_splashExitSounds[idx]) return g_splashExitSounds[idx];
    
    std::uint32_t fullId = 0;
    auto form = LoadFormAndLog<RE::BGSSoundDescriptorForm>(
  "SpellInteractionsVR.esp", fullId, kSplashExitFormBaseIDs[idx], "SplashExitSound");
    if (form) {
    g_splashExitSounds[idx] = form;
        IW_LOG_INFO("Loaded splash exit sound form for band %u -> fullId=0x%08X", (unsigned)idx, fullId);
    } else {
        IW_LOG_WARN("Failed to load splash exit sound form for band %u (base0x%08X)", 
  (unsigned)idx, kSplashExitFormBaseIDs[idx]);
    }
  return g_splashExitSounds[idx];
}

// ============================================================================
// Band selection
// ============================================================================

SplashBand GetSplashBandForDownSpeed(float downSpeed) {
    if (downSpeed <= cfgSplashVeryLightMax) return SplashBand::VeryLight;
    if (downSpeed <= cfgSplashLightMax) return SplashBand::Light;
    if (downSpeed <= cfgSplashNormalMax) return SplashBand::Normal;
    if (downSpeed <= cfgSplashHardMax) return SplashBand::Hard;
    return SplashBand::VeryHard;
}

SplashBand GetExitSplashBandForUpSpeed(float upSpeed) {
    if (upSpeed <= cfgSplashExitVeryLightMax) return SplashBand::VeryLight;
    if (upSpeed <= cfgSplashExitLightMax) return SplashBand::Light;
    if (upSpeed <= cfgSplashExitNormalMax) return SplashBand::Normal;
    if (upSpeed <= cfgSplashExitHardMax) return SplashBand::Hard;
    return SplashBand::VeryHard;
}

// ============================================================================
// Sound playback
// ============================================================================

uint32_t PlaySoundAtNode(RE::BGSSoundDescriptorForm* sound, RE::NiAVObject* node, 
            const RE::NiPoint3& location, float volume) {
    if (!sound) return 0;
    if (g_suspendAllDetections.load()) return 0;

    auto audio = RE::BSAudioManager::GetSingleton();
    if (!audio) return 0;

    RE::BSSoundHandle handle;
    if (!audio->BuildSoundDataFromDescriptor(handle, static_cast<RE::BSISoundDescriptor*>(sound), 16)) {
        return 0;
    }
 if (handle.soundID == static_cast<uint32_t>(-1)) {
        return 0;
    }

    handle.SetPosition(location);
    handle.SetObjectToFollow(node);
    handle.SetVolume(volume);
    
    if (handle.Play()) {
        return handle.soundID;
    }
    return 0;
}

void PlaySplashSoundForDownSpeed(bool isLeft, float downSpeed, bool requireMoving) {
    if (g_suspendAllDetections.load()) return;

    // Respect sneak+depth suppression
    if (isLeft) {
        if (g_leftSuppressDueToSneakDepth.load()) return;
    } else {
        if (g_rightSuppressDueToSneakDepth.load()) return;
    }

    // If caller requires controller to be moving, enforce that
    if (requireMoving) {
        if (isLeft) {
            if (!g_leftIsMoving.load()) return;
        } else {
     if (!g_rightIsMoving.load()) return;
      }
    }

  SplashBand band = GetSplashBandForDownSpeed(downSpeed);
    auto desc = LoadSplashSoundDescriptor(band);
    if (!desc) return;
    
 auto node = GetPlayerHandNode(isLeft ? false : true);
    if (!node) return;

    float vol = 0.2f;
    switch (band) {
      case SplashBand::VeryLight: vol = cfgSplashVeryLightVol; break;
        case SplashBand::Light: vol = cfgSplashLightVol; break;
        case SplashBand::Normal: vol = cfgSplashNormalVol; break;
    case SplashBand::Hard: vol = cfgSplashHardVol; break;
  case SplashBand::VeryHard: vol = cfgSplashVeryHardVol; break;
      default: vol = 0.2f; break;
    }

    uint32_t id = PlaySoundAtNode(desc, node, node->world.translate, vol);
    if (id != 0) {
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
     std::chrono::steady_clock::now().time_since_epoch()).count();
        
    if (isLeft) {
            g_leftLastEntrySoundMs.store(nowMs);
            g_leftEntrySoundPlaying.store(true);
        } else {
            g_rightLastEntrySoundMs.store(nowMs);
      g_rightEntrySoundPlaying.store(true);
        }

        // Clear playing flag after timeout
     std::thread([isLeft]() {
            try {
   std::this_thread::sleep_for(std::chrono::milliseconds(kEntrySoundPlayingTimeoutMs));
  long long last = isLeft ? g_leftLastEntrySoundMs.load() : g_rightLastEntrySoundMs.load();
           long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
     if ((now - last) >= kEntrySoundPlayingTimeoutMs) {
                if (isLeft) g_leftEntrySoundPlaying.store(false);
        else g_rightEntrySoundPlaying.store(false);
            }
          } catch (...) {}
        }).detach();
    }
}

void PlayExitSoundForUpSpeed(bool isLeft, float upSpeed) {
    if (g_suspendAllDetections.load()) return;
    
    // If entry splash is playing, suppress exit sound
    if ((isLeft && g_leftEntrySoundPlaying.load()) || (!isLeft && g_rightEntrySoundPlaying.load())) {
        return;
    }
    
    SplashBand band = GetExitSplashBandForUpSpeed(upSpeed);
    auto desc = LoadSplashExitSoundDescriptor(band);
    if (!desc) return;
    
    auto node = GetPlayerHandNode(isLeft ? false : true);
    if (!node) return;
    
    float vol = 0.2f;
    switch (band) {
    case SplashBand::VeryLight: vol = cfgSplashExitVeryLightVol; break;
        case SplashBand::Light: vol = cfgSplashExitLightVol; break;
        case SplashBand::Normal: vol = cfgSplashExitNormalVol; break;
 case SplashBand::Hard: vol = cfgSplashExitHardVol; break;
        case SplashBand::VeryHard: vol = cfgSplashExitVeryHardVol; break;
 default: vol = 0.2f; break;
    }
    if (vol < 0.0f) vol = 0.0f;

    // Don't play if controller is still submerged
    if ((isLeft && g_leftSubmerged.load()) || (!isLeft && g_rightSubmerged.load())) {
        return;
    }

    // If recent entry sound, suppress exit
    long long lastEntry = isLeft ? g_leftLastEntrySoundMs.load() : g_rightLastEntrySoundMs.load();
 if (lastEntry != 0) {
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    if ((nowMs - lastEntry) < kEntrySoundGuardMs) {
   return;
        }
    }

    PlaySoundAtNode(desc, node, node->world.translate, vol);
}

bool TryPlayWakeMoveSound(bool isLeft) {
    if (g_suspendAllDetections.load()) return false;
    
    // Load descriptor if needed
    if (!g_wakeMoveSoundDesc) {
      std::uint32_t fullId = 0;
        auto form = LoadFormAndLog<RE::BGSSoundDescriptorForm>(
            "SpellInteractionsVR.esp", fullId, 0x01000809u, "WakeMoveSound");
        if (!form) {
    IW_LOG_WARN("TryPlayWakeMoveSound: failed to load wake-move sound form");
          return false;
        }
        g_wakeMoveSoundDesc = form;
        IW_LOG_INFO("TryPlayWakeMoveSound: loaded wake-move sound form -> fullId=0x%08X", fullId);
    }

    if (g_suspendAllDetections.load()) return false;
    
    auto node = GetPlayerHandNode(isLeft ? false : true);
    if (!node) return false;

    float vol = cfgWakeMoveSoundVol;
    uint32_t id = PlaySoundAtNode(g_wakeMoveSoundDesc, node, node->world.translate, vol);
    if (id == 0) return false;

    if (isLeft) g_leftWakeMoveSoundHandle.store(id);
    else g_rightWakeMoveSoundHandle.store(id);

    return true;
}

} // namespace InteractiveWaterVR
