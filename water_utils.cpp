// water_utils.cpp - Utility functions for water collision detection

#include "water_utils.h"
#include "water_state.h"
#include "config.h"
#include "helper.h"
#include <cmath>
#include <chrono>

namespace InteractiveWaterVR {

// ============================================================================
// Vector math utilities
// ============================================================================

float VecLen(const RE::NiPoint3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

RE::NiPoint3 Normalize(const RE::NiPoint3& v) {
    float l = VecLen(v);
    if (l <= 1e-6f) return {0.0f, 1.0f, 0.0f};
    return {v.x / l, v.y / l, v.z / l};
}

// ============================================================================
// Player/Controller node access
// ============================================================================

RE::NiAVObject* GetPlayerHandNode(bool rightHand) {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return nullptr;
    auto root = player->Get3D();
    if (!root) return nullptr;

#if defined(ENABLE_SKYRIM_VR)
    const char* nodeName = rightHand ? "NPC R Hand [RHnd]" : "NPC L Hand [LHnd]";
#else
    const char* nodeName = nullptr;
#endif
    if (nodeName) {
      return root->GetObjectByName(nodeName);
    }
    return root;
}

RE::NiPoint3 GetControllerWorldPosition(bool rightHand) {
    auto handNode = GetPlayerHandNode(rightHand);
    if (handNode) return handNode->world.translate;
    return {0.0f, 0.0f, 0.0f};
}

RE::NiPoint3 GetControllerForward(bool rightHand) {
  auto node = GetPlayerHandNode(rightHand);
    if (!node) return {0.0f, 1.0f, 0.0f};
    // assume Y axis is forward for controller model
    RE::NiPoint3 localForward{0.0f, 1.0f, 0.0f};
    RE::NiPoint3 worldF = node->world.rotate * localForward;
    return Normalize(worldF);
}

// ============================================================================
// Water detection
// ============================================================================

bool IsPointInWater(const RE::NiPoint3& a_pos, float& outWaterHeight) {
    outWaterHeight = 0.0f;
    
    // rate-limit noisy logs
    static auto s_lastIsPointLog = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    constexpr auto kLogInterval = std::chrono::seconds(2);
    auto nowLog = std::chrono::steady_clock::now();

    auto tes = RE::TES::GetSingleton();
    if (!tes) {
return false;
    }
  
    auto cell = tes->GetCell(a_pos);
    if (!cell) {
        return false;
    }
    
    float wh = 0.0f;
    if (cell->GetWaterHeight(a_pos, wh)) {
        outWaterHeight = wh;
        if (!std::isfinite(outWaterHeight)) {
            return false;
        }
        constexpr float kThreshold = 0.02f;
        bool inWater = (outWaterHeight - a_pos.z) > kThreshold;
  return inWater;
    }
    
    return false;
}

void LogWaterDetailsAtPosition(const RE::NiPoint3& a_pos) {
    // Intentionally no-op: detailed water logging removed to avoid excessive log spam.
    (void)a_pos;
}

// ============================================================================
// Splash amount computation
// ============================================================================

float ComputeEntrySplashAmount(float downSpeed) {
    if (downSpeed <= 0.1f) {
        return 0.0f;
    }

    float amt = 0.0f;
    if (downSpeed <= cfgSplashVeryLightMax) {
        amt = cfgSplashVeryLightAmt;
    } else if (downSpeed <= cfgSplashLightMax) {
     amt = cfgSplashLightAmt;
  } else if (downSpeed <= cfgSplashNormalMax) {
   amt = cfgSplashNormalAmt;
    } else if (downSpeed <= cfgSplashHardMax) {
 amt = cfgSplashHardAmt;
    } else {
        amt = cfgSplashVeryHardAmt;
    }

    amt *= cfgSplashScale;
    return amt;
}

float ComputeExitSplashAmount(float upSpeed) {
    if (upSpeed <= 0.1f) {
        return 0.0f;
    }
    
    float amt = 0.0f;
    if (upSpeed <= cfgSplashExitVeryLightMax) {
        amt = cfgSplashExitVeryLightAmt;
    } else if (upSpeed <= cfgSplashExitLightMax) {
  amt = cfgSplashExitLightAmt;
    } else if (upSpeed <= cfgSplashExitNormalMax) {
        amt = cfgSplashExitNormalAmt;
    } else if (upSpeed <= cfgSplashExitHardMax) {
        amt = cfgSplashExitHardAmt;
    } else {
        amt = cfgSplashExitVeryHardAmt;
    }
    
    amt *= cfgSplashScale;
    return amt;
}

// ============================================================================
// Spell helpers
// ============================================================================

bool SpellHasKeyword(RE::MagicItem* spell, std::string_view editorID) {
    if (!spell) return false;
    for (auto eff : spell->effects) {
    if (!eff) continue;
        auto base = eff->baseEffect;
        if (!base) continue;
        for (auto kw : base->GetKeywords()) {
       if (!kw) continue;
            if (kw->formEditorID.contains(editorID)) return true;
            const char* id = kw->formEditorID.c_str();
            if (id && editorID == id) return true;
        }
    }
    return false;
}

} // namespace InteractiveWaterVR
