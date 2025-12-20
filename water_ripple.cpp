// water_ripple.cpp - Ripple emission for water collision detection

#include "water_ripple.h"
#include "water_state.h"
#include "water_coll_det.h"
#include "helper.h"
#include <chrono>

namespace InteractiveWaterVR {

// ============================================================================
// Ripple emission
// ============================================================================

void EmitRipple(const RE::NiPoint3& p, float amt) {
    if (g_suspendAllDetections.load()) return;

    // If frost-submerged flag is active, log it (spawn logic disabled)
    if (s_submergedMagicDamageFrost.load()) {
IW_LOG_INFO("EmitRipple: MagicDamageFrost flag is active - spawn logic disabled");
    }

    auto ws = RE::TESWaterSystem::GetSingleton();
  if (ws) {
        ws->AddRipple(p, amt);
    }
}

void EmitWakeRipple(bool isLeft, const RE::NiPoint3& p, float amt) {
    if (g_suspendAllDetections.load()) return;
    auto ws = RE::TESWaterSystem::GetSingleton();
    if (ws) {
        ws->AddRipple(p, amt);
    }
}

bool EmitRippleIfAllowed(bool isLeft, const RE::NiPoint3& p, float amt, 
           bool force, int requireSubmergedState, const char* reason) {
// If forced, only allow if within short window after transition
    if (force) {
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
     std::chrono::steady_clock::now().time_since_epoch()).count();
        if (isLeft) {
            long long t = g_lastLeftTransitionMs.load();
     if (nowMs - t > kForcedRippleWindowMs) {
       force = false;
  }
        } else {
            long long t = g_lastRightTransitionMs.load();
            if (nowMs - t > kForcedRippleWindowMs) {
    force = false;
  }
        }
    }

    // If a required submerged state is provided, enforce it
    if (requireSubmergedState != -1) {
        bool cur = isLeft ? g_leftSubmerged.load() : g_rightSubmerged.load();
        if (requireSubmergedState == 1 && !cur) {
            return false;
        }
        if (requireSubmergedState == 0 && cur) {
    return false;
        }
    }

    // If not forced, suppress ripples when controller is submerged
    if (!force) {
        bool cur = isLeft ? g_leftSubmerged.load() : g_rightSubmerged.load();
     if (cur) {
            return false;
  }
    }

    EmitRipple(p, amt);
    return true;
}

bool EmitSplashIfAllowed(bool isLeft, const RE::NiPoint3& p, float amt,
         bool force, int requireSubmergedState, const char* reason) {
    return EmitRippleIfAllowed(isLeft, p, amt, force, requireSubmergedState, reason);
}

} // namespace InteractiveWaterVR
