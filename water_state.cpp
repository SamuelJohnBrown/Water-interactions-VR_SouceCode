// water_state.cpp - Shared state definitions for water collision detection

#include "water_state.h"
#include "water_coll_det.h"
#include "helper.h"
#include "equipped_spell_interaction.h"

namespace InteractiveWaterVR {

// ============================================================================
// Splash form base IDs
// ============================================================================

const std::uint32_t kSplashFormBaseIDs[static_cast<size_t>(SplashBand::Count)] = {
    0x01000819u, // VeryLight
    0x01000806u, // Light
    0x01000807u, // Normal/Medium
    0x01000808u, // Hard/Large
    0x01000808u  // VeryHard/VeryLarge
};

const std::uint32_t kSplashExitFormBaseIDs[static_cast<size_t>(SplashBand::Count)] = {
    0x01000810u, // VeryLight
    0x0100081Au, // Light
    0x0100081Bu, // Normal
    0x0100080Cu, // Hard
    0x0100080Eu  // VeryHard
};

// ============================================================================
// Thread and running state
// ============================================================================

std::atomic<bool> g_running{false};
std::thread g_monitorThread;

// ============================================================================
// Game load state
// ============================================================================

std::atomic<bool> g_gameLoadInProgress{false};

// ============================================================================
// Detection flags
// ============================================================================

std::atomic<bool> g_suspendAllDetections{false};
std::atomic<bool> g_suspendDueToDepthSneak{false};
std::atomic<bool> g_leftDetectionActive{true};
std::atomic<bool> g_rightDetectionActive{true};

// ============================================================================
// Movement state
// ============================================================================

std::atomic<bool> g_leftIsMoving{false};
std::atomic<bool> g_rightIsMoving{false};
std::atomic<bool> g_prevLeftMoving{false};
std::atomic<bool> g_prevRightMoving{false};

// ============================================================================
// Submerged state
// ============================================================================

std::atomic<bool> g_leftSubmerged{false};
std::atomic<bool> g_rightSubmerged{false};
std::atomic<long long> g_lastLeftTransitionMs{0};
std::atomic<long long> g_lastRightTransitionMs{0};
std::atomic<long long> g_leftSubmergedStartMs{0};
std::atomic<long long> g_rightSubmergedStartMs{0};

// ============================================================================
// Ripple emission guards
// ============================================================================

std::atomic<bool> g_leftRippleEmitted{false};
std::atomic<bool> g_rightRippleEmitted{false};
std::chrono::steady_clock::time_point g_leftLastRippleTime{};
std::chrono::steady_clock::time_point g_rightLastRippleTime{};

// ============================================================================
// Wake timing
// ============================================================================

std::atomic<long long> g_leftLastWakeMs{0};
std::atomic<long long> g_rightLastWakeMs{0};

// ============================================================================
// Sound state
// ============================================================================

std::atomic<long long> g_leftLastEntrySoundMs{0};
std::atomic<long long> g_rightLastEntrySoundMs{0};
std::atomic<bool> g_leftEntrySoundPlaying{false};
std::atomic<bool> g_rightEntrySoundPlaying{false};
std::atomic<uint32_t> g_leftWakeMoveSoundHandle{0};
std::atomic<uint32_t> g_rightWakeMoveSoundHandle{0};
std::atomic<long long> g_leftLastWakeMoveMs{0};
std::atomic<long long> g_rightLastWakeMoveMs{0};

// ============================================================================
// Sneak/depth suppression
// ============================================================================

std::atomic<bool> g_leftSuppressDueToSneakDepth{false};
std::atomic<bool> g_rightSuppressDueToSneakDepth{false};

// ============================================================================
// Player state tracking
// ============================================================================

std::atomic<bool> g_prevPlayerSwimming{false};
std::atomic<bool> g_prevPlayerSneaking{false};
std::atomic<long long> g_lastPlayerDepthLogMs{0};
std::atomic<float> g_prevPlayerDepth{0.0f};

// ============================================================================
// Controller depth
// ============================================================================

std::atomic<float> g_leftControllerDepth{0.0f};
std::atomic<float> g_rightControllerDepth{0.0f};

// ============================================================================
// Spell submerged logging flags
// ============================================================================

std::atomic<bool> g_prevLeftSubmergedWithSpell{false};
std::atomic<bool> g_prevRightSubmergedWithSpell{false};

// ============================================================================
// Cached forms
// ============================================================================

RE::BGSSoundDescriptorForm* g_splashSounds[static_cast<size_t>(SplashBand::Count)] = {nullptr};
RE::BGSSoundDescriptorForm* g_splashExitSounds[static_cast<size_t>(SplashBand::Count)] = {nullptr};
RE::BGSSoundDescriptorForm* g_wakeMoveSoundDesc = nullptr;
RE::BGSMovableStatic* g_frostSpawnForm = nullptr;

// ============================================================================
// State reset function
// ============================================================================

void ResetAllWaterState()
{
    IW_LOG_INFO("ResetAllWaterState: clearing all water state for new session");
    
    // Clear spell interaction cached forms first
    ClearSpellInteractionCachedForms();
    
    // Clear splash sound caches
    for (size_t i = 0; i < static_cast<size_t>(SplashBand::Count); ++i) {
        g_splashSounds[i] = nullptr;
        g_splashExitSounds[i] = nullptr;
 }
    
    // Clear other cached forms
    g_wakeMoveSoundDesc = nullptr;
    g_frostSpawnForm = nullptr;
    
    // Reset magic damage flags (these are in water_coll_det.h extern declarations)
    s_submergedMagicDamageFire.store(false);
    s_submergedMagicDamageShock.store(false);
    s_submergedMagicDamageFrost.store(false);
    s_submergedMagicDamageFireLeft.store(false);
    s_submergedMagicDamageFireRight.store(false);
  s_submergedMagicDamageFrostLeft.store(false);
    s_submergedMagicDamageFrostRight.store(false);
    
    // Reset movement state
    g_leftIsMoving.store(false);
    g_rightIsMoving.store(false);
    g_prevLeftMoving.store(false);
 g_prevRightMoving.store(false);
    
    // Reset submerged state
    g_leftSubmerged.store(false);
    g_rightSubmerged.store(false);
    g_lastLeftTransitionMs.store(0);
    g_lastRightTransitionMs.store(0);
    g_leftSubmergedStartMs.store(0);
    g_rightSubmergedStartMs.store(0);
    
    // Reset ripple emission guards
    g_leftRippleEmitted.store(false);
    g_rightRippleEmitted.store(false);
  
    // Detection must be ENABLED by default
    g_leftDetectionActive.store(true);
    g_rightDetectionActive.store(true);
  g_suspendAllDetections.store(false);
    g_suspendDueToDepthSneak.store(false);
    
    // Reset sound handles
    g_leftWakeMoveSoundHandle.store(0);
    g_rightWakeMoveSoundHandle.store(0);
    g_leftLastWakeMoveMs.store(0);
    g_rightLastWakeMoveMs.store(0);
 g_leftLastEntrySoundMs.store(0);
    g_rightLastEntrySoundMs.store(0);
    g_leftEntrySoundPlaying.store(false);
    g_rightEntrySoundPlaying.store(false);
    
    // Reset wake timing
    g_leftLastWakeMs.store(0);
    g_rightLastWakeMs.store(0);
    
    // Reset controller positions (these are in water_coll_det.h)
  s_leftControllerWorldX.store(0.0f);
    s_leftControllerWorldY.store(0.0f);
    s_rightControllerWorldX.store(0.0f);
    s_rightControllerWorldY.store(0.0f);
    s_frostSpawnWaterHeight.store(0.0f);
    
    // Reset depth tracking
    g_leftControllerDepth.store(0.0f);
    g_rightControllerDepth.store(0.0f);
    g_prevPlayerDepth.store(0.0f);
    g_lastPlayerDepthLogMs.store(0);
    
    // Reset hover state
    g_leftControllerHoveringAboveWater.store(false);
    g_rightControllerHoveringAboveWater.store(false);
    g_leftControllerHoverHeight.store(0.0f);
    g_rightControllerHoverHeight.store(0.0f);
    
 // Reset sneak/swim state
    g_prevPlayerSwimming.store(false);
    g_prevPlayerSneaking.store(false);
    g_leftSuppressDueToSneakDepth.store(false);
    g_rightSuppressDueToSneakDepth.store(false);
    
    // Reset spell submerged logging state
    g_prevLeftSubmergedWithSpell.store(false);
    g_prevRightSubmergedWithSpell.store(false);
    
    // NOTE: Do NOT modify g_gameLoadInProgress here!
    // That flag is managed by engine.cpp (NotifyGameLoadStart/NotifyGameLoadEnd)
    // and should not be touched during state reset.
    
    IW_LOG_INFO("ResetAllWaterState: all state cleared (detection enabled, gameLoadInProgress unchanged)");
}

// ============================================================================
// Controller hover state (above water)
// ============================================================================

std::atomic<bool> g_leftControllerHoveringAboveWater{false};
std::atomic<bool> g_rightControllerHoveringAboveWater{false};
std::atomic<float> g_leftControllerHoverHeight{0.0f};
std::atomic<float> g_rightControllerHoverHeight{0.0f};

} // namespace InteractiveWaterVR
