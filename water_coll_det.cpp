// water_coll_det.cpp - Main water collision detection module
#include "water_coll_det.h"
#include "water_state.h"
#include "water_utils.h"
#include "water_sound.h"
#include "water_ripple.h"
#include "helper.h"
#include "config.h"
#include "equipped_spell_interaction.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>
#include <cmath>
#include <algorithm>
#include <functional>
#include <deque>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#undef max
#undef min

namespace InteractiveWaterVR {

// Extern variable definitions
std::atomic<bool> s_submergedMagicDamageFire{false};
std::atomic<bool> s_submergedMagicDamageShock{false};
std::atomic<bool> s_submergedMagicDamageFrost{false};
std::atomic<bool> s_submergedMagicDamageFireLeft{false};
std::atomic<bool> s_submergedMagicDamageFireRight{false};
std::atomic<bool> s_submergedMagicDamageFrostLeft{false};
std::atomic<bool> s_submergedMagicDamageFrostRight{false};
std::atomic<float> s_frostSpawnWaterHeight{0.0f};
std::atomic<float> s_leftControllerWorldX{0.0f};
std::atomic<float> s_leftControllerWorldY{0.0f};
std::atomic<float> s_rightControllerWorldX{0.0f};
std::atomic<float> s_rightControllerWorldY{0.0f};

// Public API
void NotifyGameLoadStart() { g_gameLoadInProgress.store(true); }
void NotifyGameLoadEnd() { g_gameLoadInProgress.store(false); }
bool IsGameLoadInProgress() { return g_gameLoadInProgress.load(); }
void ClearCachedForms() { ResetAllWaterState(); }
void StartLeftWaterDetection() { g_leftDetectionActive.store(true); }
void StopLeftWaterDetection() { g_leftDetectionActive.store(false); }
void StartRightWaterDetection() { g_rightDetectionActive.store(true); }
void StopRightWaterDetection() { g_rightDetectionActive.store(false); }
bool IsLeftWaterDetectionActive() { return g_leftDetectionActive.load(); }
bool IsRightWaterDetectionActive() { return g_rightDetectionActive.load(); }

static std::string GatherSpellEffectKeywords(RE::MagicItem* spell) {
    if (!spell) return {};
    std::vector<std::string> kwNames;
    for (auto eff : spell->effects) {
        if (!eff) continue;
        auto base = eff->baseEffect;
    if (!base) continue;
        for (auto kw : base->GetKeywords()) {
    if (!kw) continue;
const char* id = kw->formEditorID.c_str();
            if (id && id[0]) kwNames.emplace_back(id);
            else {
      char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08X", kw->formID);
                kwNames.emplace_back(buf);
       }
        }
    }
  std::string joined;
for (size_t i = 0; i < kwNames.size(); ++i) {
    if (i) joined += ", ";
        joined += kwNames[i];
    }
return joined;
}

static void MonitoringThread() {
    loadConfig();

    // CRITICAL: Ensure detection is enabled at thread start
    g_leftDetectionActive.store(true);
    g_rightDetectionActive.store(true);
    g_suspendAllDetections.store(false);
    IW_LOG_INFO("MonitoringThread: started, detection enabled for both hands");

    bool lastLeftInWater = false;
    bool lastRightInWater = false;
    bool prevLeftMovingLocal = false;
    bool prevRightMovingLocal = false;
    bool spellMonitorActive = false;
    bool loggedLeftNodeAvailable = false;
    bool loggedRightNodeAvailable = false;

    RE::NiPoint3 prevLeftPos{0.0f, 0.0f, 0.0f};
    RE::NiPoint3 prevRightPos{0.0f, 0.0f, 0.0f};

    std::deque<Sample> leftSamples;
    std::deque<Sample> rightSamples;
    std::deque<Sample> playerSamples;

    float prevLeftWaterHeight = 0.0f;
float prevRightWaterHeight = 0.0f;
    auto prevLeftTime = std::chrono::steady_clock::now();
    auto prevRightTime = prevLeftTime;
    bool havePrevLeft = false;
    bool havePrevRight = false;

  bool leftMoving = false;
    bool rightMoving = false;

    float recentLeftSpeed = 0.0f;
    float recentRightSpeed = 0.0f;
    float recentPlayerSpeed = 0.0f;

    auto leftLastMovementTime = std::chrono::steady_clock::now();
    auto rightLastMovementTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point leftMovementCandidateTime{};
    std::chrono::steady_clock::time_point rightMovementCandidateTime{};

    // Diagnostic: track iterations and log periodically
    int iterationCount = 0;
    int lastLoggedIteration = 0;
    bool loggedFirstSuccessfulIteration = false;
    
    // Track skip reasons for diagnostics
    int skipNoPlayer = 0;
    int skipNoRoot = 0;
    int skipGameLoad = 0;
    int skipNoNodes = 0;
    int skipNoWaterType = 0;
    int skipFastTravel = 0;
    int skipDeepWater = 0;
    int skipSneakDepth = 0;
    auto lastDiagLogTime = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_acquire)) {
        try {
   loadConfig();
         iterationCount++;

      auto player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
    skipNoPlayer++;
       std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
     continue;
          }
            auto root = player->Get3D();
            if (!root) {
                skipNoRoot++;
       std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
         continue;
            }

            if (g_gameLoadInProgress.load()) {
         skipGameLoad++;
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
           continue;
      }

        auto ui = RE::UI::GetSingleton();
            if (ui) {
                while (g_running.load(std::memory_order_acquire) && (ui->GameIsPaused() || ui->IsShowingMenus())) {
   std::this_thread::sleep_for(std::chrono::milliseconds(25));
        ui = RE::UI::GetSingleton();
                }
      if (!g_running.load(std::memory_order_acquire)) break;
            }

        auto leftNode = GetPlayerHandNode(false);
            auto rightNode = GetPlayerHandNode(true);

  if (!leftNode) {
        leftSamples.clear();
    havePrevLeft = false;
         loggedLeftNodeAvailable = false;
            } else if (!loggedLeftNodeAvailable) {
   IW_LOG_INFO("MonitoringThread: left hand node now available");
    loggedLeftNodeAvailable = true;
   }

         if (!rightNode) {
          rightSamples.clear();
     havePrevRight = false;
     loggedRightNodeAvailable = false;
            } else if (!loggedRightNodeAvailable) {
      IW_LOG_INFO("MonitoringThread: right hand node now available");
                loggedRightNodeAvailable = true;
            }

   if (!leftNode && !rightNode) {
          skipNoNodes++;
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
       continue;
      }

       // Log diagnostic info every 10 seconds
      auto secsSinceLastDiag = std::chrono::duration<float>(std::chrono::steady_clock::now() - lastDiagLogTime).count();
  if (secsSinceLastDiag >= 10.0f) {
       if (skipNoPlayer > 0 || skipNoRoot > 0 || skipGameLoad > 0 || skipNoNodes > 0 || 
     skipNoWaterType > 0 || skipFastTravel > 0 || skipDeepWater > 0 || skipSneakDepth > 0) {
      IW_LOG_INFO("MonitoringThread diagnostics: iter=%d, skips: noPlayer=%d noRoot=%d gameLoad=%d noNodes=%d noWaterType=%d fastTravel=%d deepWater=%d sneakDepth=%d",
      iterationCount, skipNoPlayer, skipNoRoot, skipGameLoad, skipNoNodes, 
    skipNoWaterType, skipFastTravel, skipDeepWater, skipSneakDepth);
   }
       // Reset counters
        skipNoPlayer = 0;
        skipNoRoot = 0;
     skipGameLoad = 0;
       skipNoNodes = 0;
        skipNoWaterType = 0;
         skipFastTravel = 0;
   skipDeepWater = 0;
   skipSneakDepth = 0;
          lastDiagLogTime = std::chrono::steady_clock::now();
 }

   // Log first successful iteration
   if (!loggedFirstSuccessfulIteration) {
                IW_LOG_INFO("MonitoringThread: first successful iteration - detection is now active");
  loggedFirstSuccessfulIteration = true;
            }

            auto leftPos = leftNode ? leftNode->world.translate : RE::NiPoint3{0.0f, 0.0f, 0.0f};
       auto rightPos = rightNode ? rightNode->world.translate : RE::NiPoint3{0.0f, 0.0f, 0.0f};

            s_leftControllerWorldX.store(leftNode ? leftPos.x : 0.0f);
            s_leftControllerWorldY.store(leftNode ? leftPos.y : 0.0f);
            s_rightControllerWorldX.store(rightNode ? rightPos.x : 0.0f);
            s_rightControllerWorldY.store(rightNode ? rightPos.y : 0.0f);

 auto playerPos = root->world.translate;

            bool curSneaking = player->IsSneaking();
  bool prevSneak = g_prevPlayerSneaking.load();
            if (curSneaking != prevSneak) {
     g_prevPlayerSneaking.store(curSneaking);
       if (curSneaking) IW_LOG_INFO("Player started sneaking");
   else IW_LOG_INFO("Player stopped sneaking");
            }

     float playerDepth = 0.0f;
  {
      float wh = 0.0f;
       if (IsPointInWater(playerPos, wh)) {
           playerDepth = wh - playerPos.z;
     if (playerDepth < 0.0f) playerDepth = 0.0f;
 }
     }

      bool shouldSpellMonitorRun = playerDepth >= kSpellMonitorMinDepth;
     if (shouldSpellMonitorRun && !spellMonitorActive) {
      StartSpellUnequipMonitor();
       spellMonitorActive = true;
            } else if (!shouldSpellMonitorRun && spellMonitorActive) {
           StopSpellUnequipMonitor();
      spellMonitorActive = false;
  }

            if (playerDepth >= kPlayerDepthShutdownMeters) {
 if (!g_suspendAllDetections.load()) {
     g_suspendDueToDepthSneak.store(false);
         g_suspendAllDetections.store(true);
        }
      skipDeepWater++;
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
      continue;
          }

  if (g_suspendAllDetections.load() && playerDepth < kPlayerDepthShutdownMeters && !g_suspendDueToDepthSneak.load()) {
    g_suspendAllDetections.store(false);
      }

       if (playerDepth >= kPlayerDepthSneakShutdownMeters && curSneaking) {
            if (!g_suspendAllDetections.load()) {
    g_suspendAllDetections.store(true);
      g_suspendDueToDepthSneak.store(true);
   }
      skipSneakDepth++;
       std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  continue;
}

            if (g_suspendDueToDepthSneak.load() && (playerDepth < kPlayerDepthSneakShutdownMeters || !curSneaking)) {
  g_suspendDueToDepthSneak.store(false);
          g_suspendAllDetections.store(false);
   }

       RE::NiPoint3 leftForward = GetControllerForward(false);
            RE::NiPoint3 rightForward = GetControllerForward(true);
          auto sampleTime = std::chrono::steady_clock::now();
   leftSamples.push_back(Sample{leftPos, leftForward, sampleTime});
            rightSamples.push_back(Sample{rightPos, rightForward, sampleTime});
   playerSamples.push_back(Sample{playerPos, RE::NiPoint3{0.0f, 0.0f, 0.0f}, sampleTime});

        constexpr size_t kMaxSamples = 7;
       if (leftSamples.size() > kMaxSamples) leftSamples.pop_front();
  if (rightSamples.size() > kMaxSamples) rightSamples.pop_front();
     if (playerSamples.size() > kMaxSamples) playerSamples.pop_front();

 if (playerSamples.size() >= 2) {
      const auto& sPrev = playerSamples[playerSamples.size() - 2];
  const auto& sCur = playerSamples[playerSamples.size() - 1];
double ds = std::chrono::duration<double>(sCur.t - sPrev.t).count();
          if (ds > 1e-6) {
     float dx = sCur.pos.x - sPrev.pos.x;
     float dy = sCur.pos.y - sPrev.pos.y;
      float dz = sCur.pos.z - sPrev.pos.z;
           recentPlayerSpeed = std::sqrt(dx * dx + dy * dy + dz * dz) / (float)ds;
      constexpr float kPlayerSpeedShutdown = 220.0f;
   if (recentPlayerSpeed > kPlayerSpeedShutdown) {
            skipFastTravel++;
  std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
       continue;
     }
    }
}

float leftWaterHeight = 0.0f;
     float rightWaterHeight = 0.0f;
   bool leftInWater = IsPointInWater(leftPos, leftWaterHeight);
    bool rightInWater = IsPointInWater(rightPos, rightWaterHeight);

      auto waterSystemCheck = RE::TESWaterSystem::GetSingleton();
      if (waterSystemCheck && !waterSystemCheck->currentWaterType) {
 skipNoWaterType++;
   std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
          continue;
      }

  // Hover detection - check if controller is near water surface (above or slightly below)
      bool leftHovering = false;
            bool rightHovering = false;
float leftHoverHeight = 0.0f;
      float rightHoverHeight = 0.0f;
   
         // For left controller
         if (leftNode) {
        float waterHeightAtPos = 0.0f;
  bool hasWaterBelow = false;
      
  if (leftInWater) {
           // Controller is in water - use the water height we already have
          waterHeightAtPos = leftWaterHeight;
                hasWaterBelow = true;
       } else {
          // Controller is not in water - check if there's water below
           RE::NiPoint3 checkPos = leftPos;
    checkPos.z -= 500.0f;
    if (IsPointInWater(checkPos, waterHeightAtPos)) {
            hasWaterBelow = true;
         }
        }
                
   if (hasWaterBelow) {
      // Calculate height relative to water surface (positive = above, negative = below)
       leftHoverHeight = leftPos.z - waterHeightAtPos;
              // Hovering if within range: slightly below (-tolerance) to max height above
    if (leftHoverHeight >= -kHoverDetectionBelowTolerance && leftHoverHeight <= kHoverDetectionMaxHeight) {
leftHovering = true;
       }
    }
      }
      
            // For right controller
    if (rightNode) {
          float waterHeightAtPos = 0.0f;
          bool hasWaterBelow = false;
       
         if (rightInWater) {
   waterHeightAtPos = rightWaterHeight;
    hasWaterBelow = true;
  } else {
  RE::NiPoint3 checkPos = rightPos;
        checkPos.z -= 500.0f;
   if (IsPointInWater(checkPos, waterHeightAtPos)) {
           hasWaterBelow = true;
         }
          }
     
      if (hasWaterBelow) {
        rightHoverHeight = rightPos.z - waterHeightAtPos;
                    if (rightHoverHeight >= -kHoverDetectionBelowTolerance && rightHoverHeight <= kHoverDetectionMaxHeight) {
 rightHovering = true;
   }
    }
        }
      
       // Update hover state
       g_leftControllerHoveringAboveWater.store(leftHovering);
 g_rightControllerHoveringAboveWater.store(rightHovering);
    g_leftControllerHoverHeight.store(leftHoverHeight);
  g_rightControllerHoverHeight.store(rightHoverHeight);

float leftControllerDepth = 0.0f;
   float rightControllerDepth = 0.0f;
            if (leftInWater) {
   leftControllerDepth = leftWaterHeight - leftPos.z;
     if (leftControllerDepth < 0.0f) leftControllerDepth = 0.0f;
        s_frostSpawnWaterHeight.store(leftWaterHeight);
            }
            if (rightInWater) {
                rightControllerDepth = rightWaterHeight - rightPos.z;
     if (rightControllerDepth < 0.0f) rightControllerDepth = 0.0f;
        s_frostSpawnWaterHeight.store(rightWaterHeight);
            }
        g_leftControllerDepth.store(leftControllerDepth);
      g_rightControllerDepth.store(rightControllerDepth);

            auto now = std::chrono::steady_clock::now();
   float leftDt = havePrevLeft ? std::chrono::duration<float>(now - prevLeftTime).count() : 0.0f;
        float rightDt = havePrevRight ? std::chrono::duration<float>(now - prevRightTime).count() : 0.0f;

       float movingConfirm = cfgMovingConfirmSeconds;
            float movingThreshold = cfgMovingThresholdAdjusted;

          if (havePrevLeft && leftDt > 1e-6f && leftSamples.size() >= 2) {
            const auto& sPrev = leftSamples[leftSamples.size() - 2];
       const auto& sCur = leftSamples[leftSamples.size() - 1];
   double ds = std::chrono::duration<double>(sCur.t - sPrev.t).count();
 if (ds > 1e-6) {
     float dx = sCur.pos.x - sPrev.pos.x;
   float dy = sCur.pos.y - sPrev.pos.y;
  float dz = sCur.pos.z - sPrev.pos.z;
   float speed = std::sqrt(dx * dx + dy * dy + dz * dz) / (float)ds;
          if (speed <= kMaxValidSpeed) {
          recentLeftSpeed = speed;
         if (speed > movingThreshold) leftLastMovementTime = now;
       if (!leftMoving && speed > movingThreshold) {
           if (leftMovementCandidateTime.time_since_epoch().count() == 0) leftMovementCandidateTime = now;
         else if (std::chrono::duration<float>(now - leftMovementCandidateTime).count() >= movingConfirm) {
  leftMoving = true;
        leftMovementCandidateTime = std::chrono::steady_clock::time_point{};
             }
      } else if (!leftMoving) {
 leftMovementCandidateTime = std::chrono::steady_clock::time_point{};
       } else if (std::chrono::duration<float>(now - leftLastMovementTime).count() >= kStationaryConfirmSeconds) {
 leftMoving = false;
          }
       }
}
 }

          if (havePrevRight && rightDt > 1e-6f && rightSamples.size() >= 2) {
    const auto& sPrev = rightSamples[rightSamples.size() - 2];
   const auto& sCur = rightSamples[rightSamples.size() - 1];
       double ds = std::chrono::duration<double>(sCur.t - sPrev.t).count();
       if (ds > 1e-6) {
     float dx = sCur.pos.x - sPrev.pos.x;
             float dy = sCur.pos.y - sPrev.pos.y;
   float dz = sCur.pos.z - sPrev.pos.z;
          float speed = std::sqrt(dx * dx + dy * dy + dz * dz) / (float)ds;
           if (speed <= kMaxValidSpeed) {
           recentRightSpeed = speed;
      if (speed > movingThreshold) rightLastMovementTime = now;
               if (!rightMoving && speed > movingThreshold) {
     if (rightMovementCandidateTime.time_since_epoch().count() == 0) rightMovementCandidateTime = now;
       else if (std::chrono::duration<float>(now - rightMovementCandidateTime).count() >= movingConfirm) {
         rightMoving = true;
           rightMovementCandidateTime = std::chrono::steady_clock::time_point{};
         }
          } else if (!rightMoving) {
              rightMovementCandidateTime = std::chrono::steady_clock::time_point{};
            } else if (std::chrono::duration<float>(now - rightLastMovementTime).count() >= kStationaryConfirmSeconds) {
                    rightMoving = false;
            }
                }
       }
   }

            float wakeSpeedThreshold = std::max(0.01f, movingThreshold * 0.5f);
     g_leftIsMoving.store(leftMoving);
            g_rightIsMoving.store(rightMoving);

            if (leftMoving != prevLeftMovingLocal) {
prevLeftMovingLocal = leftMoving;
            IW_LOG_INFO("Left controller movement: %s", leftMoving ? "moving" : "stationary");
   }
      if (rightMoving != prevRightMovingLocal) {
                prevRightMovingLocal = rightMoving;
        IW_LOG_INFO("Right controller movement: %s", rightMoving ? "moving" : "stationary");
            }

      g_leftSuppressDueToSneakDepth.store(curSneaking && leftControllerDepth >= 2.0f);
  g_rightSuppressDueToSneakDepth.store(curSneaking && rightControllerDepth >= 2.0f);

      if (cfgWakeEnabled) {
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
 if (g_leftDetectionActive.load() && leftInWater && recentLeftSpeed > wakeSpeedThreshold && leftControllerDepth >= kMinWakeDepthMeters) {
  long long lastMs = g_leftLastWakeMs.load();
   if (cfgWakeSpawnMs == 0 || nowMs - lastMs >= cfgWakeSpawnMs) {
         float mult = std::clamp(recentLeftSpeed * cfgWakeScaleMultiplier, cfgWakeMinMultiplier, cfgWakeMaxMultiplier);
          RE::NiPoint3 wakePos = leftPos;
               wakePos.z = leftWaterHeight;
           float finalAmt = cfgWakeAmt * mult;
  auto taskIntf = SKSE::GetTaskInterface();
 if (taskIntf) {
                taskIntf->AddTask([wakePos, finalAmt]() {
      if (!g_gameLoadInProgress.load()) EmitWakeRipple(true, wakePos, finalAmt);
                });
                  }
       g_leftLastWakeMs.store(nowMs);
       TryPlayWakeMoveSound(true);
          }
       }
  if (g_rightDetectionActive.load() && rightInWater && recentRightSpeed > wakeSpeedThreshold && rightControllerDepth >= kMinWakeDepthMeters) {
          long long lastMs = g_rightLastWakeMs.load();
    if (cfgWakeSpawnMs == 0 || nowMs - lastMs >= cfgWakeSpawnMs) {
   float mult = std::clamp(recentRightSpeed * cfgWakeScaleMultiplier, cfgWakeMinMultiplier, cfgWakeMaxMultiplier);
         RE::NiPoint3 wakePos = rightPos;
     wakePos.z = rightWaterHeight;
             float finalAmt = cfgWakeAmt * mult;
        auto taskIntf = SKSE::GetTaskInterface();
        if (taskIntf) {
   taskIntf->AddTask([wakePos, finalAmt]() {
   if (!g_gameLoadInProgress.load()) EmitWakeRipple(false, wakePos, finalAmt);
            });
   }
     g_rightLastWakeMs.store(nowMs);
         TryPlayWakeMoveSound(false);
}
      }
   }

          // Left entry
  if (g_leftDetectionActive.load() && leftInWater && !lastLeftInWater) {
      g_lastLeftTransitionMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    g_leftSubmergedStartMs.store(g_lastLeftTransitionMs.load());
     RE::NiPoint3 impactPos = leftPos;
     impactPos.z = leftWaterHeight;
       float downSpeed = (havePrevLeft && leftDt > 0.0001f) ? std::max(0.0f, (prevLeftPos.z - leftPos.z) / leftDt) : 0.0f;
   prevLeftWaterHeight = leftWaterHeight;
            if (havePrevLeft && downSpeed >= cfgEntryDownZThreshold && downSpeed <= kMaxEntryDownSpeed) {
         float amt = ComputeEntrySplashAmount(downSpeed);
         if (amt > 0.0f) {
              auto taskIntf = SKSE::GetTaskInterface();
      if (taskIntf) {
   taskIntf->AddTask([impactPos, amt, downSpeed]() {
        if (g_gameLoadInProgress.load()) return;
      EmitSplashIfAllowed(true, impactPos, amt, true, 1, "left_entry");
    PlaySplashSoundForDownSpeed(true, downSpeed, false);
         });
      }
    }
         }
   }

     // Left exit
            if (g_leftDetectionActive.load() && !leftInWater && lastLeftInWater) {
       g_lastLeftTransitionMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    RE::NiPoint3 impactPos = leftPos;
  impactPos.z = prevLeftWaterHeight;
     float upSpeed = (havePrevLeft && leftDt > 0.0001f) ? std::max(0.0f, (leftPos.z - prevLeftPos.z) / leftDt) : 0.0f;
     if (havePrevLeft && upSpeed >= cfgExitUpZThreshold && upSpeed <= kMaxExitUpSpeed) {
               float exitAmt = ComputeExitSplashAmount(upSpeed);
        if (exitAmt <= 0.0f) exitAmt = cfgSplashNormalAmt * cfgSplashScale;
   auto taskIntf = SKSE::GetTaskInterface();
    if (taskIntf) {
       taskIntf->AddTask([impactPos, exitAmt, upSpeed]() {
      if (g_gameLoadInProgress.load()) return;
   if (EmitSplashIfAllowed(true, impactPos, exitAmt, true, 0, "left_exit")) {
                PlayExitSoundForUpSpeed(true, upSpeed);
       }
     });
          }
   }
                g_leftSubmergedStartMs.store(0);
       }

    // Right entry
            if (g_rightDetectionActive.load() && rightInWater && !lastRightInWater) {
          g_lastRightTransitionMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
  g_rightSubmergedStartMs.store(g_lastRightTransitionMs.load());
         RE::NiPoint3 impactPos = rightPos;
        impactPos.z = rightWaterHeight;
           float downSpeed = (havePrevRight && rightDt > 0.0001f) ? std::max(0.0f, (prevRightPos.z - rightPos.z) / rightDt) : 0.0f;
            prevRightWaterHeight = rightWaterHeight;
    if (havePrevRight && downSpeed >= cfgEntryDownZThreshold && downSpeed <= kMaxEntryDownSpeed) {
           float amt = ComputeEntrySplashAmount(downSpeed);
   if (amt > 0.0f) {
       auto taskIntf = SKSE::GetTaskInterface();
   if (taskIntf) {
    taskIntf->AddTask([impactPos, amt, downSpeed]() {
         if (g_gameLoadInProgress.load()) return;
       EmitSplashIfAllowed(false, impactPos, amt, true, 1, "right_entry");
  PlaySplashSoundForDownSpeed(false, downSpeed, false);
      });
 }
       }
  }
            }

   // Right exit
            if (g_rightDetectionActive.load() && !rightInWater && lastRightInWater) {
           g_lastRightTransitionMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        RE::NiPoint3 impactPos = rightPos;
     impactPos.z = prevRightWaterHeight;
    float upSpeed = (havePrevRight && rightDt > 0.0001f) ? std::max(0.0f, (rightPos.z - prevRightPos.z) / rightDt) : 0.0f;
   if (havePrevRight && upSpeed >= cfgExitUpZThreshold && upSpeed <= kMaxExitUpSpeed) {
         float exitAmt = ComputeExitSplashAmount(upSpeed);
       if (exitAmt <= 0.0f) exitAmt = cfgSplashNormalAmt * cfgSplashScale;
    auto taskIntf = SKSE::GetTaskInterface();
                if (taskIntf) {
           taskIntf->AddTask([impactPos, exitAmt, upSpeed]() {
     if (g_gameLoadInProgress.load()) return;
         if (EmitSplashIfAllowed(false, impactPos, exitAmt, true, 0, "right_exit")) {
          PlayExitSoundForUpSpeed(false, upSpeed);
   }
            });
              }
     }
        g_rightSubmergedStartMs.store(0);
        }

            // Spell detection
     bool leftSubmergedWithSpell = false;
            bool rightSubmergedWithSpell = false;
 RE::MagicItem* leftSpell = nullptr;
       RE::MagicItem* rightSpell = nullptr;

        auto& actorRt = player->GetActorRuntimeData();
     leftSpell = actorRt.selectedSpells[RE::Actor::SlotTypes::kLeftHand];
          rightSpell = actorRt.selectedSpells[RE::Actor::SlotTypes::kRightHand];
            const bool leftNearSurface = leftControllerDepth <= kFrostSurfaceDepthTolerance;
    const bool rightNearSurface = rightControllerDepth <= kFrostSurfaceDepthTolerance;
         leftSubmergedWithSpell = leftInWater && (leftSpell != nullptr) && leftNearSurface;
       rightSubmergedWithSpell = rightInWater && (rightSpell != nullptr) && rightNearSurface;

   if (leftSubmergedWithSpell != g_prevLeftSubmergedWithSpell.load()) {
   g_prevLeftSubmergedWithSpell.store(leftSubmergedWithSpell);
              if (leftSubmergedWithSpell) IW_LOG_INFO("Left controller submerged with spell");
       else IW_LOG_INFO("Left controller no longer submerged with spell");
          }
            if (rightSubmergedWithSpell != g_prevRightSubmergedWithSpell.load()) {
      g_prevRightSubmergedWithSpell.store(rightSubmergedWithSpell);
     if (rightSubmergedWithSpell) IW_LOG_INFO("Right controller submerged with spell");
         else IW_LOG_INFO("Right controller no longer submerged with spell");
  }

bool leftFireNow = leftSubmergedWithSpell && SpellHasKeyword(leftSpell, "MagicDamageFire");
   bool rightFireNow = rightSubmergedWithSpell && SpellHasKeyword(rightSpell, "MagicDamageFire");
    s_submergedMagicDamageFireLeft.store(leftFireNow);
     s_submergedMagicDamageFireRight.store(rightFireNow);
     s_submergedMagicDamageFire.store(leftFireNow || rightFireNow);

            bool shockNow = (leftSubmergedWithSpell && SpellHasKeyword(leftSpell, "MagicDamageShock")) ||
          (rightSubmergedWithSpell && SpellHasKeyword(rightSpell, "MagicDamageShock"));
s_submergedMagicDamageShock.store(shockNow);

     bool leftFrostNow = leftSubmergedWithSpell && SpellHasKeyword(leftSpell, "MagicDamageFrost");
    bool rightFrostNow = rightSubmergedWithSpell && SpellHasKeyword(rightSpell, "MagicDamageFrost");
    s_submergedMagicDamageFrostLeft.store(leftFrostNow);
    s_submergedMagicDamageFrostRight.store(rightFrostNow);
            s_submergedMagicDamageFrost.store(leftFrostNow || rightFrostNow);

     g_leftSubmerged.store(leftInWater);
         g_rightSubmerged.store(rightInWater);

            prevLeftPos = leftPos;
prevRightPos = rightPos;
        prevLeftTime = now;
            prevRightTime = now;
       havePrevLeft = true;
   havePrevRight = true;
          lastLeftInWater = leftInWater;
   lastRightInWater = rightInWater;

        } catch (...) {
 std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
}

void StartWaterMonitoring() {
    if (g_running.exchange(true)) return;
 g_leftDetectionActive.store(true);
  g_rightDetectionActive.store(true);
    g_suspendAllDetections.store(false);
    g_prevLeftMoving.store(false);
    g_prevRightMoving.store(false);
    g_leftRippleEmitted.store(false);
  g_rightRippleEmitted.store(false);
    g_prevPlayerSwimming.store(false);
    IW_LOG_INFO("StartWaterMonitoring: starting monitoring thread with detection enabled");
    g_monitorThread = std::thread(MonitoringThread);
}

void StopWaterMonitoring() {
    if (!g_running.exchange(false)) return;
    if (g_monitorThread.joinable()) g_monitorThread.join();
    g_prevLeftMoving.store(false);
    g_prevRightMoving.store(false);
    g_prevPlayerSwimming.store(false);
    StopSpellUnequipMonitor();
}

bool IsMonitoringActive() { return g_running.load(); }

} // namespace InteractiveWaterVR
