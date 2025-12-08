#include "water_detection.h"
#include "helper.h"
#include "config.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>
#include <cmath>
#include <algorithm>
#include <functional>
#include <deque>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#undef max
#undef min

// NOTE: enable real logging (use IW_LOG_* macros and AppendToPluginLog)
//#define AppendToPluginLog(...) do { (void)0; } while(0)

namespace InteractiveWaterVR {

static std::atomic<bool> s_running{false};
static std::thread s_thread;
// Persisted prev-moving flags used for transition logging; reset on start/stop
static std::atomic<bool> s_prevLeftMoving{false};
static std::atomic<bool> s_prevRightMoving{false};

// Per-controller ripple emission state
static std::atomic<bool> s_leftRippleEmitted{false};
static std::atomic<bool> s_rightRippleEmitted{false};

// Per-controller last ripple time (for adaptive scaling)
static std::chrono::steady_clock::time_point s_leftLastRippleTime{};
static std::chrono::steady_clock::time_point s_rightLastRippleTime{};
// Per-controller last wake scheduling time (ms since epoch)
static std::atomic<long long> s_leftLastWakeMs{0};
static std::atomic<long long> s_rightLastWakeMs{0};
// Per-controller last entry-splash sound start time (ms since epoch)
static std::atomic<long long> s_leftLastEntrySoundMs{0};
static std::atomic<long long> s_rightLastEntrySoundMs{0};
// Window (ms) during which an exit sound must be suppressed if an entry sound just played
constexpr long long kEntrySoundGuardMs =1500;
// Wake-move sound played once when hand starts moving underwater after delay
static RE::BGSSoundDescriptorForm* s_wakeMoveSoundDesc = nullptr;
static std::atomic<uint32_t> s_leftWakeMoveSoundHandle{0};
static std::atomic<uint32_t> s_rightWakeMoveSoundHandle{0};
// Minimum time submerged before wake-move sound allowed (ms)
constexpr long long kWakeMoveMinSubmergedMs =2000;
// Minimum interval between wake-move sound plays for the same hand (ms)
constexpr long long kWakeMoveRepeatMs =2000;
static std::atomic<long long> s_leftLastWakeMoveMs{0};
static std::atomic<long long> s_rightLastWakeMoveMs{0};

// Polling interval (ms) - lowered to sample closer to higher frame rates (e.g.90Hz VR ~11ms)
// Reduced to6 ms to improve responsiveness for fast controller motion (tradeoff: higher CPU).
constexpr int kPollIntervalMs =6;

// Movement detection thresholds (m/s)
constexpr float kStationaryThreshold =0.02f; // below => consider stationary (instant threshold)
constexpr float kMovingThreshold =0.05f; // above => consider moving (instant)
constexpr float kJitterThreshold =0.01f; // tiny jitter to update last movement time
constexpr float kMaxValidSpeed =50.0f; // ignore implausible spikes
constexpr float kStationaryConfirmSeconds =2.0f; // require this much time without movement to mark stationary

// Flag set while a game load is in progress to avoid using engine objects.
static std::atomic<bool> s_gameLoadInProgress{false};

// Notify functions
void NotifyGameLoadStart() { s_gameLoadInProgress.store(true); }
void NotifyGameLoadEnd() { s_gameLoadInProgress.store(false); }

bool IsGameLoadInProgress() { return s_gameLoadInProgress.load(); }

// Movement state (true == moving)
bool leftMoving = false;
bool rightMoving = false;

// Last time any meaningful movement was seen (used to confirm stationary after delay)
auto leftLastMovementTime = std::chrono::steady_clock::now();
auto rightLastMovementTime = std::chrono::steady_clock::now();

// Movement start candidate timers (require sustained movement before reporting 'started moving')
std::chrono::steady_clock::time_point leftMovementCandidateTime{};
std::chrono::steady_clock::time_point rightMovementCandidateTime{};

 // Submerged state: set when controller enters water, cleared on exit
 static std::atomic<bool> leftSubmerged{false};
 static std::atomic<bool> rightSubmerged{false};
 // Transition timestamps (ms since steady_clock epoch) used to allow forced entry/exit ripples
 static std::atomic<long long> lastLeftTransitionMs{0};
 static std::atomic<long long> lastRightTransitionMs{0};
 // Per-controller submerged start times (ms since epoch) used to decide sound suppression
 static std::atomic<long long> leftSubmergedStartMs{0};
 static std::atomic<long long> rightSubmergedStartMs{0};
 // Window (ms) after transition during which a forced ripple is allowed
 constexpr long long kForcedRippleWindowMs =250;

// Movement thresholds are configurable via Data\SKSE\Plugins\Interactive_Water_VR.ini (see src/config.cpp)

static RE::NiAVObject* GetPlayerHandNode(bool rightHand) {
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

static RE::NiPoint3 GetControllerWorldPosition(bool rightHand) {
	auto handNode = GetPlayerHandNode(rightHand);
	if (handNode) return handNode->world.translate;
	return {0.0f,0.0f,0.0f};
}

static float VecLen(const RE::NiPoint3& v) {
	return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
}
static RE::NiPoint3 Normalize(const RE::NiPoint3& v) {
	float l = VecLen(v);
	if (l <=1e-6f) return {0.0f,1.0f,0.0f};
	return {v.x / l, v.y / l, v.z / l};
}

static RE::NiPoint3 GetControllerForward(bool rightHand) {
	auto node = GetPlayerHandNode(rightHand);
	if (!node) return {0.0f,1.0f,0.0f};
	// assume Y axis is forward for controller model
	RE::NiPoint3 localForward{0.0f,1.0f,0.0f};
	RE::NiPoint3 worldF = node->world.rotate * localForward;
	return Normalize(worldF);
}

// New: compute splash amplitude from downSpeed (magnitude units used by estimator)
static float ComputeEntrySplashAmount(float downSpeed) {
	// Very small impacts produce no visible ripple
	if (downSpeed <=0.1f) {
		AppendToPluginLog("DEBUG", "ComputeEntrySplashAmount: downSpeed=%.4f -> amt=0.000000 (below threshold)", downSpeed);
		return 0.0f;
	}

	// Use configurable band thresholds/amplitudes from config.cpp (now including a 'light' band)
	float amt =0.0f;
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

	// Apply global scale
	amt *= cfgSplashScale;

	AppendToPluginLog("DEBUG", "ComputeEntrySplashAmount: downSpeed=%.4f -> baseAmt=%.6f scaledAmt=%.6f (scale=%.3f)", downSpeed, (amt / (cfgSplashScale <=0.0f ?1.0f : cfgSplashScale)), amt, cfgSplashScale);
	return amt;
}

// New: compute splash amplitude for exit (upward) using exit-specific bands (including 'light')
static float ComputeExitSplashAmount(float upSpeed) {
	if (upSpeed <=0.1f) {
		AppendToPluginLog("DEBUG", "ComputeExitSplashAmount: upSpeed=%.4f -> amt=0.000000 (below threshold)", upSpeed);
		return 0.0f;
	}
	float amt =0.0f;
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
	AppendToPluginLog("DEBUG", "ComputeExitSplashAmount: upSpeed=%.4f -> amt=%.6f", upSpeed, amt);
	return amt;
}

// --- Sound playback helpers for splash bands ---
enum class SplashBand {
	VeryLight =0,
	Light,
	Normal,
	Hard,
	VeryHard,
	Count
};

// Map user-provided base form IDs (from SpellInteractionsVR.esp) to bands
static const std::uint32_t kSplashFormBaseIDs[static_cast<size_t>(SplashBand::Count)] = {
	0x01000806u, // VeryLight (user provided0X1000806)
	0x01000806u, // Light (same in user's mapping)
	0x01000807u, // Normal/Medium
	0x01000808u, // Hard/Large
	0x01000808u // VeryHard/VeryLarge
};

// Cache loaded descriptors to avoid repeated lookups
static RE::BGSSoundDescriptorForm* s_splashSounds[static_cast<size_t>(SplashBand::Count)] = {nullptr};

static RE::BGSSoundDescriptorForm* LoadSplashSoundDescriptor(SplashBand band) {
	auto idx = static_cast<size_t>(band);
	if (idx >= static_cast<size_t>(SplashBand::Count)) return nullptr;
	if (s_splashSounds[idx]) return s_splashSounds[idx];

	std::uint32_t fullId =0;
	// Use helper to load and log; helper will compose full form id from plugin name
	auto form = LoadFormAndLog<RE::BGSSoundDescriptorForm>("SpellInteractionsVR.esp", fullId, kSplashFormBaseIDs[idx], "SplashSound");
	if (form) {
		s_splashSounds[idx] = form;
		IW_LOG_INFO("Loaded splash sound form for band %u -> fullId=0x%08X", (unsigned)idx, fullId);
	} else {
		IW_LOG_WARN("Failed to load splash sound form for band %u (base0x%08X)", (unsigned)idx, kSplashFormBaseIDs[idx]);
	}
	return s_splashSounds[idx];
}

static SplashBand GetSplashBandForDownSpeed(float downSpeed) {
	if (downSpeed <= cfgSplashVeryLightMax) return SplashBand::VeryLight;
	if (downSpeed <= cfgSplashLightMax) return SplashBand::Light;
	if (downSpeed <= cfgSplashNormalMax) return SplashBand::Normal;
	if (downSpeed <= cfgSplashHardMax) return SplashBand::Hard;
	return SplashBand::VeryHard;
}

static uint32_t PlaySoundAtNode(RE::BGSSoundDescriptorForm* sound, RE::NiAVObject* node, const RE::NiPoint3& location, float volume)
{
	if (!sound) return 0;
	auto audio = RE::BSAudioManager::GetSingleton();
	if (!audio) return 0;

	RE::BSSoundHandle handle;
	// Build sound data from descriptor; pass a reasonable flag value similar to example (16)
	if (!audio->BuildSoundDataFromDescriptor(handle, static_cast<RE::BSISoundDescriptor*>(sound),16)) {
		AppendToPluginLog("DEBUG", "PlaySoundAtNode: BuildSoundDataFromDescriptor failed");
		return 0;
	}
	if (handle.soundID == static_cast<uint32_t>(-1)) {
		AppendToPluginLog("DEBUG", "PlaySoundAtNode: invalid soundID after BuildSoundDataFromDescriptor");
		return 0;
	}

	// log the requested volume
	AppendToPluginLog("DEBUG", "PlaySoundAtNode: setting volume=%f for sound form (node=%p)", volume, node);

	handle.SetPosition(location);
	handle.SetObjectToFollow(node);
	// apply per-band volume (SetVolume expects a float)
	handle.SetVolume(volume);
	if (handle.Play()) {
		AppendToPluginLog("DEBUG", "PlaySoundAtNode: Play() succeeded, soundID=%u", handle.soundID);
		return handle.soundID;
	}

	AppendToPluginLog("DEBUG", "PlaySoundAtNode: Play() failed for soundID=%u", handle.soundID);
	return 0;
}

static void PlaySplashSoundForDownSpeed(bool isLeft, float downSpeed) {
	// Choose band
	SplashBand band = GetSplashBandForDownSpeed(downSpeed);
	auto desc = LoadSplashSoundDescriptor(band);
	if (!desc) return;
	// Get hand node for appropriate hand
	auto node = GetPlayerHandNode(!isLeft ? true : false); // left: false, right: true
	if (!node) {
		AppendToPluginLog("DEBUG", "PlaySplashSound: hand node missing for isLeft=%d", isLeft ?1 :0);
		return;
	}

	// If the controller is already submerged and this is NOT an immediate entry event, suppress sounds.
	// Allow entry sounds only if the last transition for this hand was an 'inWater' within the forced window.
	long long lastTrans = isLeft ? lastLeftTransitionMs.load() : lastRightTransitionMs.load();
	bool justEntered = false;
	if (lastTrans !=0) {
		long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		if ((nowMs - lastTrans) <= kForcedRippleWindowMs) justEntered = true;
	}
	if (!justEntered) {
		// if currently submerged, suppress any splash sounds (this prevents sounds while moving underwater)
		if ((isLeft && leftSubmerged.load()) || (!isLeft && rightSubmerged.load())) {
			AppendToPluginLog("DEBUG", "PlaySplashSoundForDownSpeed: suppressed because hand is submerged (not entry) hand=%s", isLeft ? "L" : "R");
			return;
		}
	}

	// select volume based on band and config
	float vol =1.0f;
	switch (band) {
		case SplashBand::VeryLight: vol = cfgSplashVeryLightVol; break;
		case SplashBand::Light: vol = cfgSplashLightVol; break;
		case SplashBand::Normal: vol = cfgSplashNormalVol; break;
		case SplashBand::Hard: vol = cfgSplashHardVol; break;
		case SplashBand::VeryHard: vol = cfgSplashVeryHardVol; break;
		default: vol =1.0f; break;
	}
	// clamp to non-negative
	if (vol <0.0f) vol =0.0f;

	AppendToPluginLog("DEBUG", "PlaySplashSoundForDownSpeed: band=%d downSpeed=%.4f vol=%.3f node=%p", static_cast<int>(band), downSpeed, vol, node);

	uint32_t id = PlaySoundAtNode(desc, node, node->world.translate, vol);
	if (id == 0) {
		AppendToPluginLog("DEBUG", "PlaySplashSoundForDownSpeed: playback failed (id=0) for band=%d", static_cast<int>(band));
	} else {
		// record entry-sound start time so exit sounds can be suppressed while this plays
		long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		if (isLeft) s_leftLastEntrySoundMs.store(nowMs);
		else s_rightLastEntrySoundMs.store(nowMs);
		AppendToPluginLog("DEBUG", "PlaySplashSoundForDownSpeed: recorded entry sound timestamp for hand=%s ms=%lld", isLeft ? "L" : "R", id ==0 ?0LL : nowMs);
	}
}

// Helper that runs on main thread (or fallback) to add ripple (audio removed)
static void EmitRipple(const RE::NiPoint3& p, float amt, const char* reason = nullptr, const char* hand = "?") {
	// Only emit ripple; audio intentionally removed
	auto ws = RE::TESWaterSystem::GetSingleton();
	if (ws) {
		ws->AddRipple(p, amt);
		// classify as splash ripple in the log (semantic rename only)
		AppendToPluginLog("DEBUG", "Added splash ripple hand=%s reason=%s at pos=%f,%f,%f amt=%f", hand, reason ? reason : "(none)", p.x, p.y, p.z, amt);
	} else {
		AppendToPluginLog("DEBUG", "TESWaterSystem not available for splash ripple hand=%s reason=%s at pos=%f,%f,%f", hand, reason ? reason : "(none)", p.x, p.y, p.z);
	}
}

// Emit ripple if allowed for the given hand. If 'force' is true, emit regardless of submerged state.
// Returns true if a ripple was emitted, false if it was suppressed.
static bool EmitRippleIfAllowed(bool isLeft, const RE::NiPoint3& p, float amt, bool force = false, int requireSubmergedState = -1, const char* reason = nullptr) {
 	 // If forced, only allow if within short window after transition; otherwise treat as not forced
 	 if (force) {
 	 	 auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
 	 	 if (isLeft) {
 	 	 	 long long t = lastLeftTransitionMs.load();
 	 	 	 if (nowMs - t > kForcedRippleWindowMs) {
 	 	 	 	 // forced window expired -> treat as not forced
 	 	 	 	 force = false;
 	 	 	 }
 	 	 } else {
 	 	 	 long long t = lastRightTransitionMs.load();
 	 	 	 if (nowMs - t > kForcedRippleWindowMs) {
 	 	 	 	 force = false;
 	 	 	 }
 	 	 }
 	 }

 	 // If a required submerged state is provided, enforce it now
 	 if (requireSubmergedState != -1) {
 	 	 bool cur = isLeft ? leftSubmerged.load() : rightSubmerged.load();
 	 	 if (requireSubmergedState ==1 && !cur) {
 	 	 	 AppendToPluginLog("DEBUG", "EmitRippleIfAllowed: require submerged but currently not submerged -> suppress hand=%s pos=%f,%f,%f amt=%f", isLeft ? "L" : "R", p.x, p.y, p.z, amt);
 	 	 	 return false;
 	 	 }
 	 	 if (requireSubmergedState ==0 && cur) {
 	 	 	 AppendToPluginLog("DEBUG", "EmitRippleIfAllowed: require not submerged but currently submerged -> suppress hand=%s pos=%f,%f,%f amt=%f", isLeft ? "L" : "R", p.x, p.y, p.z, amt);
 	 	 	 return false;
 	 	 }
 	 }

 	 // If not forced, suppress ripples when the controller is already submerged (legacy behaviour)
 	 if (!force) {
 	 	 if (isLeft && leftSubmerged.load()) {
 	 	 	 AppendToPluginLog("DEBUG", "EmitRippleIfAllowed: left submerged -> suppress ripple at pos=%f,%f,%f amt=%f", p.x, p.y, p.z, amt);
 	 	  	 return false;
 	 	 }
 	 	 if (!isLeft && rightSubmerged.load()) {
 	 	  	 AppendToPluginLog("DEBUG", "EmitRippleIfAllowed: right submerged -> suppress ripple at pos=%f,%f,%f amt=%f", p.x, p.y, p.z, amt);
 	 	  	 return false;
 	 	 }
 	 }

 	 // Log full context for any emission attempt
 	 AppendToPluginLog("DEBUG", "EmitRippleIfAllowed: emitting hand=%s force=%d reqSub=%d leftSub=%d rightSub=%d reason=%s pos=%f,%f,%f amt=%f",
 	 isLeft ? "L" : "R", force ?1 :0, requireSubmergedState, leftSubmerged.load() ?1 :0, rightSubmerged.load() ?1 :0, reason ? reason : "(none)", p.x, p.y, p.z, amt);

 	 // use hand string for EmitRipple logging
 	 const char* handStr = isLeft ? "L" : "R";
 	 EmitRipple(p, amt, reason, handStr);
 	 return true;
 }

 // Splash-named wrapper that preserves behavior and is used by call sites
 static bool EmitSplashIfAllowed(bool isLeft, const RE::NiPoint3& p, float amt, bool force = false, int requireSubmergedState = -1, const char* reason = nullptr) {
	 return EmitRippleIfAllowed(isLeft, p, amt, force, requireSubmergedState, reason);
 }

// --- Exit sound support ---
// Map exit-band form IDs: default for exits is0x01000810, except hard/very hard use0x0100080E
 static const std::uint32_t kSplashExitFormBaseIDs[static_cast<size_t>(SplashBand::Count)] = {
 0x01000810u, // VeryLight
 0x01000810u, // Light
 0x01000810u, // Normal
 0x0100080Eu, // Hard
 0x0100080Eu // VeryHard
 };
 
 // Cache exit descriptors
 static RE::BGSSoundDescriptorForm* s_splashExitSounds[static_cast<size_t>(SplashBand::Count)] = { nullptr };

 static RE::BGSSoundDescriptorForm* LoadSplashExitSoundDescriptor(SplashBand band) {
  auto idx = static_cast<size_t>(band);
  if (idx >= static_cast<size_t>(SplashBand::Count)) return nullptr;
  if (s_splashExitSounds[idx]) return s_splashExitSounds[idx];
  std::uint32_t fullId =0;
  auto form = LoadFormAndLog<RE::BGSSoundDescriptorForm>("SpellInteractionsVR.esp", fullId, kSplashExitFormBaseIDs[idx], "SplashExitSound");
  if (form) {
 s_splashExitSounds[idx] = form;
 IW_LOG_INFO("Loaded splash exit sound form for band %u -> fullId=0x%08X", (unsigned)idx, fullId);
  } else {
 IW_LOG_WARN("Failed to load splash exit sound form for band %u (base0x%08X)", (unsigned)idx, kSplashExitFormBaseIDs[idx]);
  }
  return s_splashExitSounds[idx];
 }

 static SplashBand GetExitSplashBandForUpSpeed(float upSpeed) {
 if (upSpeed <= cfgSplashExitVeryLightMax) return SplashBand::VeryLight;
 if (upSpeed <= cfgSplashExitLightMax) return SplashBand::Light;
 if (upSpeed <= cfgSplashExitNormalMax) return SplashBand::Normal;
 if (upSpeed <= cfgSplashExitHardMax) return SplashBand::Hard;
 return SplashBand::VeryHard;
 }

 static void PlayExitSoundForUpSpeed(bool isLeft, float upSpeed) {
 SplashBand band = GetExitSplashBandForUpSpeed(upSpeed);
 auto desc = LoadSplashExitSoundDescriptor(band);
 if (!desc) return;
 auto node = GetPlayerHandNode(isLeft ? false : true);
 if (!node) return;
 float vol =0.2f;
 switch (band) {
 case SplashBand::VeryLight: vol = cfgSplashExitVeryLightVol; break;
 case SplashBand::Light: vol = cfgSplashExitLightVol; break;
 case SplashBand::Normal: vol = cfgSplashExitNormalVol; break;
 case SplashBand::Hard: vol = cfgSplashExitHardVol; break;
 case SplashBand::VeryHard: vol = cfgSplashExitVeryHardVol; break;
 default: vol =0.2f; break;
 }
 if (vol <0.0f) vol =0.0f;

 // Exit sounds should be played when exiting the water. However, ensure we don't accidentally
 // play sounds while the controller remains submerged. If the hand is currently submerged,
 // skip playback.
 if ((isLeft && leftSubmerged.load()) || (!isLeft && rightSubmerged.load())) {
	 AppendToPluginLog("DEBUG", "PlayExitSoundForUpSpeed: suppressed because hand still submerged hand=%s", isLeft ? "L" : "R");
	 return;
 }

 // If a recent entry splash sound was played for this hand, suppress the exit sound to avoid overlap.
 long long lastEntry = isLeft ? s_leftLastEntrySoundMs.load() : s_rightLastEntrySoundMs.load();
 if (lastEntry !=0) {
	 long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	 if ((nowMs - lastEntry) < kEntrySoundGuardMs) {
		 AppendToPluginLog("DEBUG", "PlayExitSoundForUpSpeed: suppressed because recent entry sound still within guard window for hand=%s (delta=%lldms)", isLeft ? "L" : "R", (nowMs - lastEntry));
		 return;
	 }
 }

 AppendToPluginLog("DEBUG", "PlayExitSoundForUpSpeed: band=%d upSpeed=%.4f vol=%.3f node=%p", static_cast<int>(band), upSpeed, vol, node);
 PlaySoundAtNode(desc, node, node->world.translate, vol);
}

// Definition: attempt to play the wake-move sound (once per submerged session)
static bool TryPlayWakeMoveSound(bool isLeft) {
 // Load descriptor if needed
 if (!s_wakeMoveSoundDesc) {
 std::uint32_t fullId =0;
 auto form = LoadFormAndLog<RE::BGSSoundDescriptorForm>("SpellInteractionsVR.esp", fullId,0x01000809u, "WakeMoveSound");
 if (!form) {
 IW_LOG_WARN("TryPlayWakeMoveSound: failed to load wake-move sound form (base0x01000809)");
 return false;
 }
 s_wakeMoveSoundDesc = form;
 IW_LOG_INFO("TryPlayWakeMoveSound: loaded wake-move sound form -> fullId=0x%08X", fullId);
 }

 // NOTE: per request, this sound is immune to guards. Play whenever called and hand node exists.
 auto node = GetPlayerHandNode(isLeft ? false : true);
 if (!node) return false;

 float vol = cfgWakeMoveSoundVol;
 uint32_t id = PlaySoundAtNode(s_wakeMoveSoundDesc, node, node->world.translate, vol);
 if (id ==0) return false;

 // store handle id for potential later stop/cleanup on exit
 if (isLeft) s_leftWakeMoveSoundHandle.store(id);
 else s_rightWakeMoveSoundHandle.store(id);

 AppendToPluginLog("INFO", "TryPlayWakeMoveSound: started wake-move sound (no-guard) for hand=%s id=%u vol=%.3f", isLeft ? "L" : "R", id, vol);
 return true;
}

static bool IsPointInWater(const RE::NiPoint3& a_pos, float& outWaterHeight) {
 outWaterHeight =0.0f;
 // rate-limit noisy logs to once every N seconds
 static auto s_lastIsPointLog = std::chrono::steady_clock::now() - std::chrono::seconds(10);
 constexpr auto kLogInterval = std::chrono::seconds(2);

 auto nowLog = std::chrono::steady_clock::now();

 auto tes = RE::TES::GetSingleton();
 if (!tes) {
 if (nowLog - s_lastIsPointLog > kLogInterval) {
 AppendToPluginLog("DEBUG", "IsPointInWater: TES singleton is null for pos=%f,%f,%f", a_pos.x, a_pos.y, a_pos.z);
 s_lastIsPointLog = nowLog;
 }
 return false;
 }
 auto cell = tes->GetCell(a_pos);
 if (!cell) {
 if (nowLog - s_lastIsPointLog > kLogInterval) {
 AppendToPluginLog("DEBUG", "IsPointInWater: GetCell returned null for pos=%f,%f,%f", a_pos.x, a_pos.y, a_pos.z);
 s_lastIsPointLog = nowLog;
 }
 return false;
 }
 float wh =0.0f;
 if (cell->GetWaterHeight(a_pos, wh)) {
 outWaterHeight = wh;
 if (!std::isfinite(outWaterHeight)) {
 if (nowLog - s_lastIsPointLog > kLogInterval) {
 AppendToPluginLog("DEBUG", "IsPointInWater: waterHeight not finite at pos=%f,%f,%f", a_pos.x, a_pos.y, a_pos.z);
 s_lastIsPointLog = nowLog;
 }
 return false;
 }
 constexpr float kThreshold =0.02f;
 bool inWater = (outWaterHeight - a_pos.z) > kThreshold;
 return inWater;
 }
 if (nowLog - s_lastIsPointLog > kLogInterval) {
 AppendToPluginLog("DEBUG", "IsPointInWater: GetWaterHeight false for pos=%f,%f,%f", a_pos.x, a_pos.y, a_pos.z);
 s_lastIsPointLog = nowLog;
 }
 return false;
}

static void LogWaterDetailsAtPosition(const RE::NiPoint3& a_pos) {
 float waterHeight =0.0f;
 if (!IsPointInWater(a_pos, waterHeight)) {
 AppendToPluginLog("INFO", "LogWaterDetailsAtPosition: point not in water");
 return;
 }

 AppendToPluginLog("INFO", "Water surface Z = %f", waterHeight);

 auto waterSystem = RE::TESWaterSystem::GetSingleton();
 if (!waterSystem) {
 AppendToPluginLog("INFO", "TESWaterSystem not available");
 return;
 }

 auto waterForm = waterSystem->currentWaterType;
 if (waterForm) {
 AppendToPluginLog("INFO", "WaterForm present");
 } else {
 AppendToPluginLog("INFO", "currentWaterType is null; scanning waterNormals and waterObjects");
 }

 for (std::size_t i =0; i < waterSystem->waterNormals.size(); ++i) {
 auto wn = waterSystem->waterNormals[i];
 if (!wn) continue;
 AppendToPluginLog("INFO", "waterNormals[%u] entry", static_cast<unsigned int>(i));
 }
 for (std::size_t i =0; i < waterSystem->waterObjects.size(); ++i) {
 auto wobj = waterSystem->waterObjects[i];
 if (!wobj) continue;
 AppendToPluginLog("INFO", "waterObject[%u] = %p", static_cast<unsigned int>(i), wobj.get());
 }
 for (std::size_t i =0; i < waterSystem->waterDisplacement.size(); ++i) {
 auto disp = waterSystem->waterDisplacement[i];
 if (!disp) continue;
 AppendToPluginLog("INFO", "waterDisplacement[%u] = %p", static_cast<unsigned int>(i), disp.get());
 }
}

// forward declaration for wake ripple helper
static void EmitWakeRipple(bool isLeft, const RE::NiPoint3& p, float amt);
// forward declaration for wake-move sound helper
static bool TryPlayWakeMoveSound(bool isLeft);

void MonitoringThread() {
	// call loadConfig initially
	loadConfig();

	// Reset persistent transition flags on thread start
	// (these are static inside the loop; ensure they start false)
	// Note: prev flags declared later as static within the loop; reset global atomics if present

	bool lastLeftInWater = false;
	bool lastRightInWater = false;

	RE::NiPoint3 prevLeftPos{0.0f,0.0f,0.0f};
	RE::NiPoint3 prevRightPos{0.0f,0.0f,0.0f};

	// ring buffer sample for velocity estimation
struct Sample { RE::NiPoint3 pos; RE::NiPoint3 forward; std::chrono::steady_clock::time_point t; };
std::deque<Sample> leftSamples;
std::deque<Sample> rightSamples;
	
	float prevLeftWaterHeight =0.0f;
	float prevRightWaterHeight =0.0f;
	auto prevLeftTime = std::chrono::steady_clock::now();
	auto prevRightTime = prevLeftTime;
	bool havePrevLeft = false;
	bool havePrevRight = false;

	// Movement state (true == moving)
	bool leftMoving = false;
	bool rightMoving = false;

	// recent speeds for logging
	float recentLeftSpeed =0.0f;
	float recentRightSpeed =0.0f;

	// Last time any meaningful movement was seen (used to confirm stationary after delay)
	auto leftLastMovementTime = std::chrono::steady_clock::now();
	auto rightLastMovementTime = std::chrono::steady_clock::now();

	// Movement start candidate timers (require sustained movement before reporting 'started moving')
	std::chrono::steady_clock::time_point leftMovementCandidateTime{};
	std::chrono::steady_clock::time_point rightMovementCandidateTime{};

	// Submerged state: set when controller enters water, cleared on exit
	// Use file-scope submerged flags (`leftSubmerged` / `rightSubmerged`) so helper functions
	// (EmitRippleIfAllowed) can observe the current submerged state. Do NOT redeclare locals here.

	while (s_running.load(std::memory_order_acquire)) {
		try {
			// hot-reload config each loop to ensure INI hotload on load game
			loadConfig();

			// If player or world isn't ready (during load), wait and skip this iteration
			auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
				continue;
			}
			auto root = player->Get3D();
			if (!root) {
				std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
				continue;
			}

			// If a game load is in progress, skip sampling and emission to avoid accessing torn-down engine objects
			if (s_gameLoadInProgress.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
				continue;
			}
			
			auto leftPos = GetControllerWorldPosition(false);
			auto rightPos = GetControllerWorldPosition(true);

			// capture forward directions and timestamp, push into ring buffers for velocity estimation
			RE::NiPoint3 leftForward = GetControllerForward(false);
			RE::NiPoint3 rightForward = GetControllerForward(true);
			auto sampleTime = std::chrono::steady_clock::now();
			leftSamples.push_back(Sample{leftPos, leftForward, sampleTime});
			rightSamples.push_back(Sample{rightPos, rightForward, sampleTime});
			constexpr size_t kMaxSamples =7;
			if (leftSamples.size() > kMaxSamples) leftSamples.pop_front();
			if (rightSamples.size() > kMaxSamples) rightSamples.pop_front();

			// determine water heights and in-water flags early (used later)
			float leftWaterHeight =0.0f;
			float rightWaterHeight =0.0f;
			bool leftInWater = IsPointInWater(leftPos, leftWaterHeight);
			bool rightInWater = IsPointInWater(rightPos, rightWaterHeight);

			// Update transition timestamps (ms since steady_clock epoch) so forced entry/exit ripples
	 		// are permitted only within a short window after transition. Do NOT flip the file-scope
	 		// submerged flags here — postpone until after emission logic so queued main-thread tasks
	 		// that may execute outside the forced window are not incorrectly suppressed by the
	 		// "already submerged" check.
		 if (leftInWater && !lastLeftInWater) {
			 long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			 lastLeftTransitionMs.store(nowMs);
			 // mark submerged start time
			 leftSubmergedStartMs.store(nowMs);
			 // ignore tiny Z jitter transitions: require previous pos was sufficiently above surface to count as entry
			 if (!(prevLeftPos.z - leftWaterHeight > cfgMinZDiffForEntryExit)) {
			 AppendToPluginLog("DEBUG", "Left transition -> inWater suppressed due to small Z delta (prevZ=%f waterZ=%f minDiff=%f)", prevLeftPos.z, leftWaterHeight, cfgMinZDiffForEntryExit);
			 } else {
			 AppendToPluginLog("DEBUG", "Left transition -> inWater at %lld", lastLeftTransitionMs.load());
			 }
 }
 if (!leftInWater && lastLeftInWater) {
			 long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			 lastLeftTransitionMs.store(nowMs);
			 // clear submerged start (we are exiting)
			 long long start = leftSubmergedStartMs.load();
			 long long submergedMs =0;
			 if (start !=0) submergedMs = nowMs - start;
			 // Clear start immediately so other logic knows we're no longer submerged
			 leftSubmergedStartMs.store(0);
			 // ignore tiny Z jitter transitions on exit: require current pos sufficiently above previous water height
			 if (!(leftPos.z - prevLeftWaterHeight > cfgMinZDiffForEntryExit)) {
			 AppendToPluginLog("DEBUG", "Left transition -> outOfWater suppressed due to small Z delta (posZ=%f prevWaterZ=%f minDiff=%f)", leftPos.z, prevLeftWaterHeight, cfgMinZDiffForEntryExit);
			 } else {
			 AppendToPluginLog("DEBUG", "Left transition -> outOfWater at %lld (submergedMs=%lld)", lastLeftTransitionMs.load(), submergedMs);
			 }
 }
 if (rightInWater && !lastRightInWater) {
			 long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			 lastRightTransitionMs.store(nowMs);
			 // mark submerged start time for right
			 rightSubmergedStartMs.store(nowMs);
			 if (!(prevRightPos.z - rightWaterHeight > cfgMinZDiffForEntryExit)) {
			 AppendToPluginLog("DEBUG", "Right transition -> inWater suppressed due to small Z delta (prevZ=%f waterZ=%f minDiff=%f)", prevRightPos.z, rightWaterHeight, cfgMinZDiffForEntryExit);
			 } else {
			 AppendToPluginLog("DEBUG", "Right transition -> inWater at %lld", lastRightTransitionMs.load());
			 }
 }
 if (!rightInWater && lastRightInWater) {
			 long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			 lastRightTransitionMs.store(nowMs);
			 long long start = rightSubmergedStartMs.load();
			 long long submergedMs =0;
			 if (start !=0) submergedMs = nowMs - start;
			 rightSubmergedStartMs.store(0);
			 if (!(rightPos.z - prevRightWaterHeight > cfgMinZDiffForEntryExit)) {
			 AppendToPluginLog("DEBUG", "Right transition -> outOfWater suppressed due to small Z delta (posZ=%f prevWaterZ=%f minDiff=%f)", rightPos.z, prevRightWaterHeight, cfgMinZDiffForEntryExit);
			 } else {
			 AppendToPluginLog("DEBUG", "Right transition -> outOfWater at %lld (submergedMs=%lld)", lastRightTransitionMs.load(), submergedMs);
			 }
 }

			auto now = std::chrono::steady_clock::now();
			float leftDt =0.0f;
			float rightDt = 0.0f;
			if (havePrevLeft) leftDt = std::chrono::duration<float>(now - prevLeftTime).count();
			if (havePrevRight) rightDt = std::chrono::duration<float>(now - prevRightTime).count();

			// use cfg variables for thresholds
			float movingConfirm = cfgMovingConfirmSeconds;
			float jitterThreshold = cfgJitterThresholdAdjusted;
			float movingThreshold = cfgMovingThresholdAdjusted;

			// --- Movement detection (use translational delta from sample buffer to ignore rotation) ---
			if (havePrevLeft && leftDt >1e-6f) {
				float speed =0.0f;
				if (leftSamples.size() >=2) {
					const auto &sPrev = leftSamples[leftSamples.size() -2];
					const auto &sCur = leftSamples[leftSamples.size() -1];
					double ds = std::chrono::duration<double>(sCur.t - sPrev.t).count();
					if (ds >1e-6) {
						float dx = sCur.pos.x - sPrev.pos.x;
						float dy = sCur.pos.y - sPrev.pos.y;
						speed = std::sqrt(dx * dx + dy * dy) / (float)ds;
					}
				}

				if (speed <= kMaxValidSpeed) {
					recentLeftSpeed = speed; // horizontal translational speed

					if (speed > movingThreshold) {
						leftLastMovementTime = now;
					}

					if (!leftMoving) {
						if (speed > movingThreshold) {
							if (leftMovementCandidateTime.time_since_epoch().count() ==0) {
								leftMovementCandidateTime = now;
							} else {
								float cand = std::chrono::duration<float>(now - leftMovementCandidateTime).count();
								if (cand >= movingConfirm) {
									leftMoving = true;
									leftMovementCandidateTime = std::chrono::steady_clock::time_point{};
									// mark last movement time when we deem moving
									leftLastMovementTime = now;
								}
							}
						} else {
							// reset candidate if motion drops below threshold
							leftMovementCandidateTime = std::chrono::steady_clock::time_point{};
						}
					} else {
						// already moving: check for stationary (require kStationaryConfirmSeconds of no significant movement)
						float secsSince = std::chrono::duration<float>(now - leftLastMovementTime).count();
						if (secsSince >= kStationaryConfirmSeconds) {
							leftMoving = false;
						}
					}
				}
			}

			if (havePrevRight && rightDt >1e-6f) {
				float speedR =0.0f;
				if (rightSamples.size() >=2) {
					const auto &sPrev = rightSamples[rightSamples.size() -2];
					const auto &sCur = rightSamples[rightSamples.size() -1];
					double ds = std::chrono::duration<double>(sCur.t - sPrev.t).count();
					if (ds >1e-6) {
						float dx = sCur.pos.x - sPrev.pos.x;
						float dy = sCur.pos.y - sPrev.pos.y;
						speedR = std::sqrt(dx * dx + dy * dy) / (float)ds;
					}
				}

				if (speedR <= kMaxValidSpeed) {
					recentRightSpeed = speedR;

					if (speedR > movingThreshold) rightLastMovementTime = now;

					if (!rightMoving) {
						if (speedR > movingThreshold) {
							if (rightMovementCandidateTime.time_since_epoch().count() ==0) {
								rightMovementCandidateTime = now;
							} else {
								float candR = std::chrono::duration<float>(now - rightMovementCandidateTime).count();
								if (candR >= movingConfirm) {
									rightMoving = true;
									rightMovementCandidateTime = std::chrono::steady_clock::time_point{};
									rightLastMovementTime = now;
								}
							}
						} else {
							rightMovementCandidateTime = std::chrono::steady_clock::time_point{};
						}
					} else {
						float secsSinceR = std::chrono::duration<float>(now - rightLastMovementTime).count();
						if (secsSinceR >= kStationaryConfirmSeconds) rightMoving = false;
					}
				}
			}
			// ---------------------------------------------------------

			// Emit start/stop logs on state transitions using recent speed values
	 static bool prevLeftMoving = false;
	 static bool prevRightMoving = false;

			if (leftMoving != prevLeftMoving) {
				if (leftMoving) {
					AppendToPluginLog("INFO", "Left controller started moving - speed=%.4f m/s", recentLeftSpeed);
				} else {
					AppendToPluginLog("INFO", "Left controller stopped moving - last speed=%.4f m/s", recentLeftSpeed);
					if (leftSubmerged) {
						AppendToPluginLog("INFO", "Left controller stationary - submerged and still for %.1f s", kStationaryConfirmSeconds);
					} else {
						AppendToPluginLog("INFO", "Left controller stationary - not submerged and still for %.1f s", kStationaryConfirmSeconds);
					}
				}
				prevLeftMoving = leftMoving;
			}

			if (rightMoving != prevRightMoving) {
				if (rightMoving) {
					AppendToPluginLog("INFO", "Right controller started moving - speed=%.4f m/s", recentRightSpeed);
				} else {
					AppendToPluginLog("INFO", "Right controller stopped moving - last speed=%.4f m/s", recentRightSpeed);
					if (rightSubmerged) {
						AppendToPluginLog("INFO", "Right controller stationary - submerged and still for %.1f s", kStationaryConfirmSeconds);
					} else {
						AppendToPluginLog("INFO", "Right controller stationary - not submerged and still for %.1f s", kStationaryConfirmSeconds);
					}
				}
				prevRightMoving = rightMoving;
			}
			// ---------------------------------------------------------

			// Spawn wake ripples immediately when submerged AND moving; call helper directly for zero-frame latency
		 if (cfgWakeEnabled) {
 // Use instantaneous recent translational speed. Use a more sensitive threshold so wakes spawn immediately
 float wakeSpeedThreshold = std::max(0.01f, movingThreshold *0.5f);
 auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
 // LEFT
 if (leftInWater && recentLeftSpeed > wakeSpeedThreshold) {
 long long lastMs = s_leftLastWakeMs.load();
 if (cfgWakeSpawnMs ==0 || nowMs - lastMs >= cfgWakeSpawnMs) {
 // compute multiplier from recent speed
 float mult = cfgWakeMinMultiplier;
 if (cfgWakeScaleMultiplier >0.0f) {
 mult = recentLeftSpeed * cfgWakeScaleMultiplier;
 if (mult < cfgWakeMinMultiplier) mult = cfgWakeMinMultiplier;
 if (mult > cfgWakeMaxMultiplier) mult = cfgWakeMaxMultiplier;
 }
 RE::NiPoint3 wakePos = leftPos;
 wakePos.z = leftWaterHeight;
 float base = cfgWakeAmt;
 float finalAmt = base * mult;
 
 // Log the scheduled wake request (helps debug missing wakes)
 AppendToPluginLog("INFO", "ScheduleWakeRequest: hand=L pos=%f,%f,%f recentSpeed=%.4f mult=%.4f base=%.6f final=%.6f", leftPos.x, leftPos.y, leftWaterHeight, recentLeftSpeed, mult, base, finalAmt);
 // Schedule emission on main thread via SKSE TaskInterface for safe engine access. The task reads
 // the atomic wake position when it runs so ripples follow the controller even if the task executes
 // a few ms after scheduling.
 auto taskIntfWakeLocal = SKSE::GetTaskInterface();
 if (taskIntfWakeLocal) {
 std::function<void()> wfn = [wakePos, finalAmt]() {
 if (s_gameLoadInProgress.load()) return;
 AppendToPluginLog("INFO", "EmitWakeRippleTask: hand=L pos=%f,%f,%f amt=%.6f", wakePos.x, wakePos.y, wakePos.z, finalAmt);
 EmitWakeRipple(true, wakePos, finalAmt);
 };
 taskIntfWakeLocal->AddTask(wfn);
 } else {
 // Fallback: emit directly using the captured wakePos (task interface unavailable)
 if (!s_gameLoadInProgress.load()) {
 AppendToPluginLog("INFO", "EmitWakeRippleDirect: hand=L pos=%f,%f,%f amt=%.6f", wakePos.x, wakePos.y, wakePos.z, finalAmt);
 EmitWakeRipple(true, wakePos, finalAmt);
 }
 }
 s_leftLastWakeMs.store(nowMs);
 // Attempt to play wake-move sound when a wake is first scheduled for this submerged session
 if (!TryPlayWakeMoveSound(true)) {
 AppendToPluginLog("DEBUG", "TryPlayWakeMoveSound: left did not start (maybe guard, already played, or not enough submerged time)");
 }
 }
 }
 // RIGHT
 if (rightInWater && recentRightSpeed > wakeSpeedThreshold) {
 long long lastMs = s_rightLastWakeMs.load();
 if (cfgWakeSpawnMs ==0 || nowMs - lastMs >= cfgWakeSpawnMs) {
 float mult = cfgWakeMinMultiplier;
 if (cfgWakeScaleMultiplier >0.0f) {
 mult = recentRightSpeed * cfgWakeScaleMultiplier;
 if (mult < cfgWakeMinMultiplier) mult = cfgWakeMinMultiplier;
 if (mult > cfgWakeMaxMultiplier) mult = cfgWakeMaxMultiplier;
 }
 RE::NiPoint3 wakePosR = rightPos;
 wakePosR.z = rightWaterHeight;
 float baseR = cfgWakeAmt;
 float finalAmtR = baseR * mult;
 
 AppendToPluginLog("INFO", "ScheduleWakeRequest: hand=R pos=%f,%f,%f recentSpeed=%.4f mult=%.4f base=%.6f final=%.6f", rightPos.x, rightPos.y, rightWaterHeight, recentRightSpeed, mult, baseR, finalAmtR);
 auto taskIntfWakeLocalR = SKSE::GetTaskInterface();
 if (taskIntfWakeLocalR) {
 std::function<void()> wfnR = [wakePosR, finalAmtR]() {
 if (s_gameLoadInProgress.load()) return;
 AppendToPluginLog("INFO", "EmitWakeRippleTask: hand=R pos=%f,%f,%f amt=%.6f", wakePosR.x, wakePosR.y, wakePosR.z, finalAmtR);
 EmitWakeRipple(false, wakePosR, finalAmtR);
 };
 taskIntfWakeLocalR->AddTask(wfnR);
 } else {
 if (!s_gameLoadInProgress.load()) {
 AppendToPluginLog("INFO", "EmitWakeRippleDirect: hand=R pos=%f,%f,%f amt=%.6f", wakePosR.x, wakePosR.y, wakePosR.z, finalAmtR);
 EmitWakeRipple(false, wakePosR, finalAmtR);
 }
 }
 s_rightLastWakeMs.store(nowMs);
 // Attempt to play wake-move sound for right hand as well
 if (!TryPlayWakeMoveSound(false)) {
 AppendToPluginLog("DEBUG", "TryPlayWakeMoveSound: right did not start (maybe guard, already played, or not enough submerged time)");
 }
 }
 }
				}
				// ---------------------------------------------------------

			// Left entry -> emit single entry ripple if downward Z speed exceeds configured threshold
			if (leftInWater && !lastLeftInWater) {
				AppendToPluginLog("INFO", "Left controller entered water - pos=%f,%f,%f waterZ=%f", leftPos.x, leftPos.y, leftPos.z, leftWaterHeight);

				RE::NiPoint3 impactPos = leftPos;
				if (havePrevLeft) {
					float denom = prevLeftPos.z - leftPos.z;
					if (std::abs(denom) >1e-6f) {
						float t = (prevLeftPos.z - leftWaterHeight) / denom;
						t = std::clamp(t,0.0f,1.0f);
						impactPos.x = prevLeftPos.x + (leftPos.x - prevLeftPos.x) * t;
						impactPos.y = prevLeftPos.y + (leftPos.y - prevLeftPos.y) * t;
						impactPos.z = prevLeftPos.z + (leftPos.z - prevLeftPos.z) * t;
						impactPos.z = leftWaterHeight;
					} else {
						impactPos.z = leftWaterHeight;
					}
				} else {
					impactPos.z = leftWaterHeight;
				}

				AppendToPluginLog("INFO", "Left impact pos = %f,%f,%f", impactPos.x, impactPos.y, impactPos.z);

				// compute downward speed (positive = downward) using central-difference + rotational contrib
				RE::NiPoint3 impactVel{0.0f,0.0f,0.0f};
				float downSpeed =0.0f;
				if (havePrevLeft) {
					// helper to compute central-difference velocity for a given sample index in leftSamples
					auto computeVelocity = [&leftSamples](size_t idx) -> RE::NiPoint3 {
						RE::NiPoint3 v{0.0f,0.0f,0.0f};
						if (leftSamples.size() >=3 && idx >0 && idx +1 < leftSamples.size()) {
							auto &sPrev = leftSamples[idx -1];
							auto &sNext = leftSamples[idx +1];
							double dt = std::chrono::duration<double>(sNext.t - sPrev.t).count();
							if (dt >1e-6) {
								v.x = (sNext.pos.x - sPrev.pos.x) / (float)dt;
								v.y = (sNext.pos.y - sPrev.pos.y) / (float)dt;
								v.z = (sNext.pos.z - sPrev.pos.z) / (float)dt;
							}
						} else if (leftSamples.size() >=2 && idx >0) {
							auto &sPrev = leftSamples[idx -1];
							auto &sCur = leftSamples[idx];
							double dt = std::chrono::duration<double>(sCur.t - sPrev.t).count();
							if (dt >1e-6) {
								v.x = (sCur.pos.x - sPrev.pos.x) / (float)dt;
								v.y = (sCur.pos.y - sPrev.pos.y) / (float)dt;
								v.z = (sCur.pos.z - sPrev.pos.z) / (float)dt;
							}
						}
						return v;
					};

					// determine indices for prev and current in leftSamples (they should be last two entries)
				 size_t n = leftSamples.size();
					if (n >=2) {
					 size_t idxPrev = n -2; // previous sampleound
					 size_t idxCur = n -1; // current sample
					 // compute translational velocities
					 RE::NiPoint3 vPrev = computeVelocity(idxPrev);
					 RE::NiPoint3 vCur = computeVelocity(idxCur);

					 // compute fractional impact time t in [0,1] between prev and cur (using prevLeftPos and leftPos as before)
					 float t =0.0f;
					 if (std::abs(prevLeftPos.z - leftPos.z) >1e-6f) {
						 t = (prevLeftPos.z - prevLeftWaterHeight) / (prevLeftPos.z - leftPos.z);
						 t = std::clamp(t,0.0f,1.0f);
					 }
					 // interpolate translational velocity to impact time
					 double dtPrevCur = std::chrono::duration<double>(leftSamples[idxCur].t - leftSamples[idxPrev].t).count();
					 RE::NiPoint3 vImpact{0.0f,0.0f,0.0f};
					 if (dtPrevCur >1e-9) {
						 vImpact.x = vPrev.x + (vCur.x - vPrev.x) * (t);
						 vImpact.y = vPrev.y + (vCur.y - vPrev.y) * (t);
						 vImpact.z = vPrev.z + (vCur.z - vPrev.z) * (t);
					 } else {
						 // fallback to simple delta across prev->cur
						 double dtWhole = std::chrono::duration<double>(leftSamples[idxCur].t - leftSamples[idxPrev].t).count();
						 if (dtWhole >1e-6) {
							 vImpact.x = (leftSamples[idxCur].pos.x - leftSamples[idxPrev].pos.x) / (float)dtWhole;
							 vImpact.y = (leftSamples[idxCur].pos.y - leftSamples[idxPrev].pos.y) / (float)dtWhole;
							 vImpact.z = (leftSamples[idxCur].pos.z - leftSamples[idxPrev].pos.z) / (float)dtWhole;
						 }
					 }

					 // rotational contribution was previously estimated here but caused false positives when it
					 // added vertical velocity; for impact detection we now ONLY use translational velocity
					 // (hand position delta) to determine downSpeed. This avoids rotation-triggered heavy hits.
					 RE::NiPoint3 impactVel = vImpact; // translational only

					 // derive downward speed from translational vertical velocity only (positive = downward)
					 downSpeed = std::max(0.0f, -impactVel.z);
					 // NOTE: intentionally do NOT fallback to full-magnitude to avoid counting horizontal/rotational motion
					 AppendToPluginLog("DEBUG", "Left entry translational downSpeed=%.4f (translational-only)", downSpeed);
					}
				}
				// save current water height for exit interpolation
				prevLeftWaterHeight = leftWaterHeight;

				AppendToPluginLog("DEBUG", "Left entry candidate: downSpeed=%.4f havePrev=%d cfgThreshold=%.4f", downSpeed, havePrevLeft ?1 :0, cfgEntryDownZThreshold);

				if (havePrevLeft && downSpeed >= cfgEntryDownZThreshold) {
					bool expected = false;
					if (s_leftRippleEmitted.compare_exchange_strong(expected, true)) {
						const float minEmitInterval =0.18f;
						float since = std::chrono::duration<float>(now - s_leftLastRippleTime).count();
						if (since < minEmitInterval) {
							AppendToPluginLog("DEBUG", "Left entry ripple suppressed; since last %.3f < %.3f", since, minEmitInterval);
						 s_leftRippleEmitted.store(false);
						} else {
						 s_leftLastRippleTime = now;
						 auto taskIntf = SKSE::GetTaskInterface();
						 if (taskIntf) {
							// compute splash amount based on downSpeed
							float amt = ComputeEntrySplashAmount(downSpeed);
							if (amt <=0.0f) {
								// nothing to emit
								s_leftRippleEmitted.store(false);
							} else {
								// determine whether to play sound: entry implies just entered, so allow
								bool allowSound = true;
								std::function<void()> fn = [impactPos, amt, downSpeed, allowSound]() {
									if (s_gameLoadInProgress.load()) return;
									EmitSplashIfAllowed(true, impactPos, amt, true,1, "left_entry");
									// small directional ripple (25% of main amt)
									RE::NiPoint3 dir = GetControllerForward(false);
									dir.z =0.0f; dir = Normalize(dir);
									RE::NiPoint3 ripPos = impactPos + dir *0.2f; ripPos.z = impactPos.z;
									EmitSplashIfAllowed(true, ripPos, amt *0.25f, true,1, "left_entry_dir");
									// Play splash sound based on downSpeed band from SpellInteractionsVR.esp
									if (allowSound) PlaySplashSoundForDownSpeed(true, downSpeed);
								};
								taskIntf->AddTask(fn);
							}
						 } else {
							AppendToPluginLog("WARN", "TaskInterface unavailable: skipping left entry ripple");
							s_leftRippleEmitted.store(false);
						 }
						}
					}
				} else {
					AppendToPluginLog("DEBUG", "Left entry ripple not emitted: downSpeed %.4f < threshold %.4f or missing prev sample", downSpeed, cfgEntryDownZThreshold);
				}
			}

			// Left exit -> emit single exit ripple if upward Z speed exceeds configured threshold
			if (!leftInWater && lastLeftInWater) {
				AppendToPluginLog("INFO", "Left controller exited water - pos=%f,%f,%f", leftPos.x, leftPos.y, leftPos.z);

				RE::NiPoint3 impactPos = leftPos;
				if (havePrevLeft) {
					float denom = prevLeftPos.z - leftPos.z;
					if (std::abs(denom) >1e-6f) {
						float t = (prevLeftPos.z - prevLeftWaterHeight) / denom;
					 t = std::clamp(t,0.0f,1.0f);
						impactPos.x = prevLeftPos.x + (leftPos.x - prevLeftPos.x) * t;
						impactPos.y = prevLeftPos.y + (leftPos.y - prevLeftPos.y) * t;
						impactPos.z = prevLeftPos.z + (leftPos.z - prevLeftPos.z) * t;
						impactPos.z = prevLeftWaterHeight;
					} else {
						impactPos.z = prevLeftWaterHeight;
					}
				}

				float upSpeed =0.0f;
				if (havePrevLeft && leftDt >0.0001f) {
					float vz = (leftPos.z - prevLeftPos.z) / leftDt;
					upSpeed = std::max(0.0f, vz);
				}

				AppendToPluginLog("DEBUG", "Left exit candidate: upSpeed=%.4f cfgThreshold=%.4f", upSpeed, cfgExitUpZThreshold);

				if (havePrevLeft && upSpeed >= cfgExitUpZThreshold) {
					const float minEmitInterval =0.18f;
					float since = std::chrono::duration<float>(now - s_leftLastRippleTime).count();
					if (since < minEmitInterval) {
						AppendToPluginLog("DEBUG", "Left exit ripple suppressed; since last %.3f < %.3f", since, minEmitInterval);
					} else {
					 s_leftLastRippleTime = now;
					 auto taskIntf = SKSE::GetTaskInterface();
					 if (taskIntf) {
						 // compute exit splash amount from upSpeed
						 float exitAmt = ComputeExitSplashAmount(upSpeed);
						 if (exitAmt <=0.0f) exitAmt = cfgSplashNormalAmt * cfgSplashScale; // fallback

						 // decide whether to play sound: if submerged start was at least1000ms, suppress sound
						 long long start = leftSubmergedStartMs.load();
						 long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
						 long long submergedMs =0;
						 if (start !=0) submergedMs = nowMs - start;
						 bool allowSound = (submergedMs <1000);

						 std::function<void()> fn = [impactPos, exitAmt, upSpeed, allowSound]() {
							 if (s_gameLoadInProgress.load()) return;
							 bool emitted = EmitSplashIfAllowed(true, impactPos, exitAmt, true,0, "left_exit");
							 if (emitted && allowSound) {
								 PlayExitSoundForUpSpeed(true, upSpeed);
							 }
						 };
						 taskIntf->AddTask(fn);
					 } else {
						AppendToPluginLog("WARN", "TaskInterface unavailable: skipping left exit ripple");
					 }
					}
				} else {
					AppendToPluginLog("DEBUG", "Left exit ripple not emitted: upSpeed %.4f < threshold %.4f or missing prev sample", upSpeed, cfgExitUpZThreshold);
				}

				// reset entry-emitted guard
			 s_leftRippleEmitted.store(false);
			}

			// Right entry -> emit single entry ripple if downward Z speed exceeds configured threshold
			if (rightInWater && !lastRightInWater) {
				AppendToPluginLog("INFO", "Right controller entered water - pos=%f,%f,%f waterZ=%f", rightPos.x, rightPos.y, rightPos.z, rightWaterHeight);

				RE::NiPoint3 impactPos = rightPos;
				if (havePrevRight) {
					float denom = prevRightPos.z - rightPos.z;
					if (std::abs(denom) >1e-6f) {
						float t = (prevRightPos.z - rightWaterHeight) / denom;
					 t = std::clamp(t,0.0f,1.0f);
						impactPos.x = prevRightPos.x + (rightPos.x - prevRightPos.x) * t;
						impactPos.y = prevRightPos.y + (rightPos.y - prevRightPos.y) * t;
						impactPos.z = prevRightPos.z + (rightPos.z - prevRightPos.z) * t;
						impactPos.z = rightWaterHeight;
					} else {
						impactPos.z = rightWaterHeight;
					}
				} else {
					impactPos.z = rightWaterHeight;
				}

				AppendToPluginLog("INFO", "Right impact pos = %f,%f,%f", impactPos.x, impactPos.y, impactPos.z);

				RE::NiPoint3 impactVelR{0.0f,0.0f,0.0f};
				float downSpeedR =0.0f;
				if (havePrevRight && rightDt >0.0001f) {
					// previous implementation used simple translational velocity only; maintain that but ensure Z-only is used
					impactVelR.x = (rightPos.x - prevRightPos.x) / rightDt;
					impactVelR.y = (rightPos.y - prevRightPos.y) / rightDt;
					impactVelR.z = (rightPos.z - prevRightPos.z) / rightDt;
					// Use vertical component only for downSpeed
					downSpeedR = std::max(0.0f, -impactVelR.z);
					AppendToPluginLog("DEBUG", "Right entry translational downSpeed=%.4f (translational-only)", downSpeedR);
					if (downSpeedR <=0.001f) {
						float magR = std::sqrt(impactVelR.x * impactVelR.x + impactVelR.y * impactVelR.y + impactVelR.z * impactVelR.z);
						// only use magnitude as absolute fallback when vertical is nearly zero but this may
						// reintroduce false positives; keep it conservative thresholded at0.001
						downSpeedR = magR;
					}
				}

				// save water height for exit interpolation
				prevRightWaterHeight = rightWaterHeight;

				AppendToPluginLog("DEBUG", "Right entry candidate: downSpeed=%.4f havePrev=%d cfgThreshold=%.4f", downSpeedR, havePrevRight ?1 :0, cfgEntryDownZThreshold);

				if (havePrevRight && downSpeedR >= cfgEntryDownZThreshold) {
					bool expected = false;
					if (s_rightRippleEmitted.compare_exchange_strong(expected, true)) {
						const float minEmitInterval =0.18f;
						float since = std::chrono::duration<float>(now - s_rightLastRippleTime).count();
						if (since < minEmitInterval) {
							AppendToPluginLog("DEBUG", "Right entry ripple suppressed; since last %.3f < %.3f", since, minEmitInterval);
						 s_rightRippleEmitted.store(false);
						} else {
						 s_rightLastRippleTime = now;
						 auto taskIntfR = SKSE::GetTaskInterface();
						 if (taskIntfR) {
							 // compute splash amount based on downSpeedR
							 float amtR = ComputeEntrySplashAmount(downSpeedR);
							 if (amtR <=0.0f) {
								 s_rightRippleEmitted.store(false);
							 } else {
								 std::function<void()> fnR = [impactPos, amtR, downSpeedR]() {
									 if (s_gameLoadInProgress.load()) return;
									 EmitSplashIfAllowed(false, impactPos, amtR, true,1, "right_entry");
									 RE::NiPoint3 dir = GetControllerForward(true);
									 dir.z =0.0f; dir = Normalize(dir);
									 RE::NiPoint3 ripPos = impactPos + dir *0.2f; ripPos.z = impactPos.z;
									 EmitSplashIfAllowed(false, ripPos, amtR *0.25f, true,1, "right_entry_dir");
									 // Play splash sound for right hand based on downSpeedR
									 PlaySplashSoundForDownSpeed(false, downSpeedR);
								 };
								 taskIntfR->AddTask(fnR);
							 }
						 } else {
							AppendToPluginLog("WARN", "TaskInterface unavailable: skipping right entry ripple");
						 s_rightRippleEmitted.store(false);
						 }
						}
					}
				} else {
					AppendToPluginLog("DEBUG", "Right entry ripple not emitted: downSpeed %.4f < threshold %.4f or missing prev sample", downSpeedR, cfgEntryDownZThreshold);
				}
			}

			// Right exit -> emit single exit ripple if upward Z speed exceeds configured threshold
			if (!rightInWater && lastRightInWater) {
				AppendToPluginLog("INFO", "Right controller exited water - pos=%f,%f,%f", rightPos.x, rightPos.y, rightPos.z);

				RE::NiPoint3 impactPos = rightPos;
				if (havePrevRight) {
					float denom = prevRightPos.z - rightPos.z;
					if (std::abs(denom) >1e-6f) {
						float t = (prevRightPos.z - prevRightWaterHeight) / denom;
					 t = std::clamp(t,0.0f,1.0f);
						impactPos.x = prevRightPos.x + (rightPos.x - prevRightPos.x) * t;
						impactPos.y = prevRightPos.y + (rightPos.y - prevRightPos.y) * t;
						impactPos.z = prevRightPos.z + (rightPos.z - prevRightPos.z) * t;
						impactPos.z = prevRightWaterHeight;
					} else {
						impactPos.z = prevRightWaterHeight;
					}
				}

				float upSpeedR =0.0f;
				if (havePrevRight && rightDt >0.0001f) {
					float vz = (rightPos.z - prevRightPos.z) / rightDt;
					upSpeedR = std::max(0.0f, vz);
				}

				AppendToPluginLog("DEBUG", "Right exit candidate: upSpeed=%.4f cfgThreshold=%.4f", upSpeedR, cfgExitUpZThreshold);

				if (havePrevRight && upSpeedR >= cfgExitUpZThreshold) {
					const float minEmitInterval =0.18f;
					float since = std::chrono::duration<float>(now - s_rightLastRippleTime).count();
					if (since < minEmitInterval) {
						AppendToPluginLog("DEBUG", "Right exit ripple suppressed; since last %.3f < %.3f", since, minEmitInterval);
					} else {
					 s_rightLastRippleTime = now;
					 auto taskIntfR = SKSE::GetTaskInterface();
					 if (taskIntfR) {
						 float exitAmtR = ComputeExitSplashAmount(upSpeedR);
						 if (exitAmtR <=0.0f) exitAmtR = cfgSplashNormalAmt * cfgSplashScale;

						 long long start = rightSubmergedStartMs.load();
						 long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
						 long long submergedMs =0;
						 if (start !=0) submergedMs = nowMs - start;
						 bool allowSoundR = (submergedMs <1000);

						 std::function<void()> fnR = [impactPos, exitAmtR, upSpeedR, allowSoundR]() {
							 if (s_gameLoadInProgress.load()) return;
							 bool emitted = EmitSplashIfAllowed(false, impactPos, exitAmtR, true,0, "right_exit");
							 if (emitted && allowSoundR) {
								 PlayExitSoundForUpSpeed(false, upSpeedR);
							 }
						 };
						 taskIntfR->AddTask(fnR);
					 } else {
						AppendToPluginLog("WARN", "TaskInterface unavailable: skipping right exit ripple");
					 }
 }
 } else {
 AppendToPluginLog("DEBUG", "Right exit ripple not emitted: upSpeed %.4f < threshold %.4f or missing prev sample", upSpeedR, cfgExitUpZThreshold);
 }

 // reset emitted guard
 s_rightRippleEmitted.store(false);
 }

 // --- Update file-scope submerged flags AFTER emission logic ---
	 leftSubmerged.store(leftInWater);
 rightSubmerged.store(rightInWater);

			 // update previous positions/times
			 prevLeftPos = leftPos;
			 prevRightPos = rightPos;
			 prevLeftTime = now;
			 prevRightTime = now;
			 havePrevLeft = true;
			 havePrevRight = true;

			 lastLeftInWater = leftInWater;
			 lastRightInWater = rightInWater;
		} catch (const std::exception& e) {
			AppendToPluginLog("ERROR", "MonitoringThread: exception caught: %s", e.what());
			// on exception, avoid tight loop
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			continue;
		} catch (...) {
			AppendToPluginLog("ERROR", "MonitoringThread: unknown exception caught");
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			continue;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
	}
}

// Wake ripple helper: adds a ripple at surface and rate-limited logs emission.
static void EmitWakeRipple(bool isLeft, const RE::NiPoint3& p, float amt) {
 auto ws = RE::TESWaterSystem::GetSingleton();
 if (ws) {
 ws->AddRipple(p, amt);
 }

 AppendToPluginLog("INFO", "EmitWakeRipple: hand=%s pos=%f,%f,%f amt=%f", isLeft ? "L" : "R", p.x, p.y, p.z, amt);
}

void StartWaterMonitoring() {
	if (s_running.exchange(true)) return;
	// reset persistently logged moving states
	s_prevLeftMoving.store(false);
	s_prevRightMoving.store(false);
	s_leftRippleEmitted.store(false);
	s_rightRippleEmitted.store(false);
	s_thread = std::thread(MonitoringThread);
}

void StopWaterMonitoring() {
	if (!s_running.exchange(false)) return;
	if ( s_thread.joinable()) s_thread.join();
	// reset prev-moving flags
	s_prevLeftMoving.store(false);
 s_prevRightMoving.store(false);
}

bool IsMonitoringActive() { return s_running.load(); }

} // namespace InteractiveWaterVR
