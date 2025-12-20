#pragma once
// water_ripple.h - Ripple emission for water collision detection

#include <RE/Skyrim.h>

namespace InteractiveWaterVR {

// ============================================================================
// Ripple emission
// ============================================================================

// Low-level ripple emission (just adds ripple to water system)
void EmitRipple(const RE::NiPoint3& p, float amt);

// Wake ripple helper
void EmitWakeRipple(bool isLeft, const RE::NiPoint3& p, float amt);

// Emit ripple if allowed by current state
// Returns true if ripple was emitted
bool EmitRippleIfAllowed(bool isLeft, const RE::NiPoint3& p, float amt, 
          bool force = false, int requireSubmergedState = -1,
   const char* reason = nullptr);

// Splash wrapper (same as EmitRippleIfAllowed)
bool EmitSplashIfAllowed(bool isLeft, const RE::NiPoint3& p, float amt,
      bool force = false, int requireSubmergedState = -1,
        const char* reason = nullptr);

} // namespace InteractiveWaterVR
