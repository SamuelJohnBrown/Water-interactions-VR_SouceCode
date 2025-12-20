#pragma once
// water_sound.h - Sound playback for water collision detection

#include "water_state.h"
#include <RE/Skyrim.h>

namespace InteractiveWaterVR {

// ============================================================================
// Sound loading
// ============================================================================

RE::BGSSoundDescriptorForm* LoadSplashSoundDescriptor(SplashBand band);
RE::BGSSoundDescriptorForm* LoadSplashExitSoundDescriptor(SplashBand band);

// ============================================================================
// Band selection
// ============================================================================

SplashBand GetSplashBandForDownSpeed(float downSpeed);
SplashBand GetExitSplashBandForUpSpeed(float upSpeed);

// ============================================================================
// Sound playback
// ============================================================================

uint32_t PlaySoundAtNode(RE::BGSSoundDescriptorForm* sound, RE::NiAVObject* node, 
         const RE::NiPoint3& location, float volume);

void PlaySplashSoundForDownSpeed(bool isLeft, float downSpeed, bool requireMoving = true);
void PlayExitSoundForUpSpeed(bool isLeft, float upSpeed);
bool TryPlayWakeMoveSound(bool isLeft);

} // namespace InteractiveWaterVR
