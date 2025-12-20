#pragma once
// water_state.h - Shared state for water collision detection
// This file contains all atomic flags, cached forms, and constants used across water modules

#include <atomic>
#include <chrono>
#include <thread>
#include <deque>
#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>

namespace InteractiveWaterVR {

// ============================================================================
// Constants
// ============================================================================

// Hardcoded maximum speeds (m/s) beyond which entries/exits are ignored
constexpr float kMaxEntryDownSpeed = 1500.0f;
constexpr float kMaxExitUpSpeed = 900.0f;

// Polling interval (ms)
constexpr int kPollIntervalMs = 6;

// Movement detection thresholds (m/s)
constexpr float kStationaryThreshold = 1.0f;
constexpr float kMovingThreshold = 0.1f;
constexpr float kJitterThreshold = 0.03f;
constexpr float kMaxValidSpeed = 60.0f;
constexpr float kStationaryConfirmSeconds = 1.5f;

// Player depth logging
constexpr long long kPlayerDepthLogIntervalMs = 1000;
constexpr float kPlayerDepthLogDelta = 0.05f;

// Player depth thresholds
constexpr float kPlayerDepthShutdownMeters = 90.0f;
constexpr float kSpellMonitorMinDepth = 1.0f;
constexpr float kPlayerDepthSneakShutdownMeters = 65.0f;

// Ripple timing
constexpr long long kForcedRippleWindowMs = 250;
constexpr float kMinWakeDepthMeters = 2.0f;
constexpr float kFrostSurfaceDepthTolerance = 6.0f;

// Sound timing
constexpr int kEntrySoundPlayingTimeoutMs = 2000;
constexpr long long kEntrySoundGuardMs = 1500;

// Player speed logging
constexpr long long kPlayerSpeedLogIntervalMs = 500;
constexpr float kPlayerSpeedLogDelta = 0.1f;

// ============================================================================
// Splash Bands
// ============================================================================

enum class SplashBand {
    VeryLight = 0,
    Light,
    Normal,
    Hard,
 VeryHard,
    Count
};

// Entry splash form base IDs
extern const std::uint32_t kSplashFormBaseIDs[static_cast<size_t>(SplashBand::Count)];

// Exit splash form base IDs
extern const std::uint32_t kSplashExitFormBaseIDs[static_cast<size_t>(SplashBand::Count)];

// Frost spawn form base id
constexpr std::uint32_t kFrostSpawnFormBaseId = 0x01000816u;

// ============================================================================
// Sample structure for velocity estimation
// ============================================================================

struct Sample {
    RE::NiPoint3 pos;
    RE::NiPoint3 forward;
    std::chrono::steady_clock::time_point t;
};

// ============================================================================
// Thread and running state
// ============================================================================

extern std::atomic<bool> g_running;
extern std::thread g_monitorThread;

// ============================================================================
// Game load state
// ============================================================================

extern std::atomic<bool> g_gameLoadInProgress;

// ============================================================================
// Detection flags
// ============================================================================

extern std::atomic<bool> g_suspendAllDetections;
extern std::atomic<bool> g_suspendDueToDepthSneak;
extern std::atomic<bool> g_leftDetectionActive;
extern std::atomic<bool> g_rightDetectionActive;

// ============================================================================
// Movement state
// ============================================================================

extern std::atomic<bool> g_leftIsMoving;
extern std::atomic<bool> g_rightIsMoving;
extern std::atomic<bool> g_prevLeftMoving;
extern std::atomic<bool> g_prevRightMoving;

// ============================================================================
// Submerged state
// ============================================================================

extern std::atomic<bool> g_leftSubmerged;
extern std::atomic<bool> g_rightSubmerged;
extern std::atomic<long long> g_lastLeftTransitionMs;
extern std::atomic<long long> g_lastRightTransitionMs;
extern std::atomic<long long> g_leftSubmergedStartMs;
extern std::atomic<long long> g_rightSubmergedStartMs;

// ============================================================================
// Ripple emission guards
// ============================================================================

extern std::atomic<bool> g_leftRippleEmitted;
extern std::atomic<bool> g_rightRippleEmitted;
extern std::chrono::steady_clock::time_point g_leftLastRippleTime;
extern std::chrono::steady_clock::time_point g_rightLastRippleTime;

// ============================================================================
// Wake timing
// ============================================================================

extern std::atomic<long long> g_leftLastWakeMs;
extern std::atomic<long long> g_rightLastWakeMs;

// ============================================================================
// Sound state
// ============================================================================

extern std::atomic<long long> g_leftLastEntrySoundMs;
extern std::atomic<long long> g_rightLastEntrySoundMs;
extern std::atomic<bool> g_leftEntrySoundPlaying;
extern std::atomic<bool> g_rightEntrySoundPlaying;
extern std::atomic<uint32_t> g_leftWakeMoveSoundHandle;
extern std::atomic<uint32_t> g_rightWakeMoveSoundHandle;
extern std::atomic<long long> g_leftLastWakeMoveMs;
extern std::atomic<long long> g_rightLastWakeMoveMs;

// ============================================================================
// Sneak/depth suppression
// ============================================================================

extern std::atomic<bool> g_leftSuppressDueToSneakDepth;
extern std::atomic<bool> g_rightSuppressDueToSneakDepth;

// ============================================================================
// Player state tracking
// ============================================================================

extern std::atomic<bool> g_prevPlayerSwimming;
extern std::atomic<bool> g_prevPlayerSneaking;
extern std::atomic<long long> g_lastPlayerDepthLogMs;
extern std::atomic<float> g_prevPlayerDepth;

// ============================================================================
// Controller depth
// ============================================================================

extern std::atomic<float> g_leftControllerDepth;
extern std::atomic<float> g_rightControllerDepth;

// ============================================================================
// Spell submerged flags (declared extern in water_coll_det.h, defined here)
// ============================================================================

extern std::atomic<bool> g_prevLeftSubmergedWithSpell;
extern std::atomic<bool> g_prevRightSubmergedWithSpell;

// ============================================================================
// Cached forms (sounds, statics)
// ============================================================================

extern RE::BGSSoundDescriptorForm* g_splashSounds[static_cast<size_t>(SplashBand::Count)];
extern RE::BGSSoundDescriptorForm* g_splashExitSounds[static_cast<size_t>(SplashBand::Count)];
extern RE::BGSSoundDescriptorForm* g_wakeMoveSoundDesc;
extern RE::BGSMovableStatic* g_frostSpawnForm;

// ============================================================================
// State reset function
// ============================================================================

void ResetAllWaterState();

// ============================================================================
// Controller hover state (above water surface)
// ============================================================================

extern std::atomic<bool> g_leftControllerHoveringAboveWater;
extern std::atomic<bool> g_rightControllerHoveringAboveWater;
extern std::atomic<float> g_leftControllerHoverHeight;
extern std::atomic<float> g_rightControllerHoverHeight;

// Hover detection threshold - how high above water surface to detect
constexpr float kHoverDetectionMaxHeight = 30.0f;  // Max height above water to consider "hovering"
constexpr float kHoverDetectionBelowTolerance = 3.0f;  // How far below water surface still counts as "hovering"

} // namespace InteractiveWaterVR
