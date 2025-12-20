#pragma once
// water_utils.h - Utility functions for water collision detection

#include <RE/Skyrim.h>

namespace InteractiveWaterVR {

// ============================================================================
// Vector math utilities
// ============================================================================

float VecLen(const RE::NiPoint3& v);
RE::NiPoint3 Normalize(const RE::NiPoint3& v);

// ============================================================================
// Player/Controller node access
// ============================================================================

RE::NiAVObject* GetPlayerHandNode(bool rightHand);
RE::NiPoint3 GetControllerWorldPosition(bool rightHand);
RE::NiPoint3 GetControllerForward(bool rightHand);

// ============================================================================
// Water detection
// ============================================================================

bool IsPointInWater(const RE::NiPoint3& a_pos, float& outWaterHeight);
void LogWaterDetailsAtPosition(const RE::NiPoint3& a_pos);

// ============================================================================
// Splash amount computation
// ============================================================================

float ComputeEntrySplashAmount(float downSpeed);
float ComputeExitSplashAmount(float upSpeed);

// ============================================================================
// Spell helpers
// ============================================================================

bool SpellHasKeyword(RE::MagicItem* spell, std::string_view editorID);

} // namespace InteractiveWaterVR
