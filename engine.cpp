#include "engine.h"
#include <SKSE/SKSE.h>
#include "higgsinterface.h"
#include "helper.h"
#include "water_coll_det.h"
#include <thread>
#include <atomic>
#include <string>
#include <mutex>

namespace InteractiveWaterVR
{
	SKSE::Trampoline* g_trampoline = nullptr;

	// Indicates whether StartMod has already executed successfully for the current game session
	static std::atomic<bool> s_modStarted{ false };
	// Generation token to invalidate previously scheduled start tasks
	static std::atomic<uint32_t> s_startGeneration{0};

	// Ensure SpellInteractions logging runs only once per game session (reset on load)
	static std::atomic<bool> s_spellLogged{false};

	// Track reschedule count to prevent infinite loops
	static std::atomic<int> s_rescheduleCount{0};
	static constexpr int kMaxReschedules = 60; // Give up after 60 attempts (~60 seconds)

	void ResetAllRuntimeState()
	{
		IW_LOG_INFO("ResetAllRuntimeState: clearing all session state for new game/load");
		
		// Increment generation to invalidate any pending scheduled tasks
		s_startGeneration.fetch_add(1, std::memory_order_acq_rel);
		
		// Reset module started flag so StartMod can run again
		s_modStarted.store(false);
		
		// Reset spell logging flag so it logs again for new session
		s_spellLogged.store(false);
		
		// Reset reschedule counter
		s_rescheduleCount.store(0);
		
		// Stop monitoring thread so it can be restarted fresh
		StopWaterMonitoring();
		
		// CRITICAL: Clear all cached form pointers from previous session
		// Forms from previous saves are invalid in the new session!
		ClearCachedForms();
		
		// Notify that we're in a load state
		NotifyGameLoadStart();
	}

	void CancelScheduledStartMod()
	{
		// increment generation to invalidate pending tasks
		s_startGeneration.fetch_add(1, std::memory_order_acq_rel);
		// reset mod started flag so future loads can start fresh
		s_modStarted.store(false);
		// reset reschedule count
		s_rescheduleCount.store(0);
	}

	void LogSpellInteractionsVRLoaded()
	{
		// Only run once per game session
		bool expected = false;
		if (!s_spellLogged.compare_exchange_strong(expected, true)) {
			return;
		}

		auto handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			IW_LOG_WARN("LogSpellInteractionsVRLoaded: TESDataHandler not available");
			s_spellLogged.store(false); // Allow retry
			return;
		}

		const auto mod = handler->LookupLoadedModByName("SpellInteractionsVR.esp");
		if (mod) {
			auto idx = handler->GetLoadedModIndex("SpellInteractionsVR.esp");
			if (idx && *idx != 0xFF) {
				unsigned int modIndex = static_cast<unsigned int>(*idx);
				SKSE::log::info("SpellInteractionsVR.esp is loaded. Mod index:0x{:02X}", modIndex);
				InteractiveWaterVR::AppendToPluginLog("INFO", "SpellInteractionsVR.esp is loaded. Mod index:0x%02X", modIndex);
			} else {
				IW_LOG_WARN("SpellInteractionsVR.esp is loaded but mod index invalid");
			}

			auto allFormsPair = RE::TESForm::GetAllForms();
			auto& allFormsMap = *allFormsPair.first;
			auto& lock = allFormsPair.second.get();
			std::size_t found = 0;
			{
				RE::BSReadLockGuard guard{ lock };
				for (const auto& kv : allFormsMap) {
					const auto form = kv.second;
					if (!form) {
						continue;
					}
					const auto file = form->GetFile();
					if (file == mod) {
						++found;
						const char* editorID = form->GetFormEditorID();
						const char* name = form->GetName();

						std::string typeStr;
						auto typeView = RE::FormTypeToString(form->GetFormType());
						if (!typeView.empty()) {
							typeStr = std::string(typeView);
						} else {
							typeStr = "<none>";
						}

						std::string editorIDStr = (editorID && *editorID) ? std::string(editorID) : std::string("<none>");
						std::string nameStr = (name && *name) ? std::string(name) : std::string("<none>");

						SKSE::log::info(
							"SpellInteractionsVR record #{}: FormID0x{:08X} Type {} EditorID '{}' Name '{}'",
							found,
							static_cast<unsigned int>(form->GetFormID()),
							typeStr,
							editorIDStr,
							nameStr);

						InteractiveWaterVR::AppendToPluginLog(
							"INFO",
							"SpellInteractionsVR record #%zu: FormID0x%08X Type %s EditorID '%s' Name '%s'",
							found,
							static_cast<unsigned int>(form->GetFormID()),
							typeStr.c_str(),
							editorIDStr.c_str(),
							nameStr.c_str());
					}
				}
			}

			SKSE::log::info("SpellInteractionsVR.esp: logged {} records", found);
			InteractiveWaterVR::AppendToPluginLog("INFO", "SpellInteractionsVR.esp: logged %zu records", found);

		} else {
			IW_LOG_WARN("SpellInteractionsVR.esp is NOT loaded");
		}
	}

	// Internal function that does the actual initialization work
	static bool TryInitialize()
	{
		// Check if player and 3D root are available
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}
		
		auto root = player->Get3D();
		if (!root) {
			return false;
		}

		IW_LOG_INFO("TryInitialize: player and 3D root available, proceeding with initialization");

		// Initialize SetAngle relocation for VM call
		try {
			SetAngle = REL::Relocation<_SetAngle>{ REL::VariantID(0, 0, 0x009D18F0) };
			IW_LOG_INFO("SetAngle relocation initialized");
		} catch (...) {
			IW_LOG_WARN("TryInitialize: failed to initialize SetAngle relocation");
		}

		// Initialize MoveTo relocation for VM call (VR address)
		try {
			MoveTo = REL::Relocation<_MoveTo>{ REL::VariantID(0, 0, 0x009CF360) };
			IW_LOG_INFO("MoveTo relocation initialized");
		} catch (...) {
			IW_LOG_WARN("TryInitialize: failed to initialize MoveTo relocation");
		}

		// Initialize Delete relocation for despawning (VR address)
		try {
			Delete = REL::Relocation<_Delete>{ REL::VariantID(0, 0, 0x009CE380) };
			IW_LOG_INFO("Delete relocation initialized");
		} catch (...) {
			IW_LOG_WARN("TryInitialize: failed to initialize Delete relocation");
		}

		// Clear game-load flag now that we are ready to resume monitoring
		InteractiveWaterVR::NotifyGameLoadEnd();

		// Start water monitoring
		StartWaterMonitoring();
		IW_LOG_INFO("TryInitialize: water monitoring started successfully");

		return true;
	}

	void StartMod()
	{
		IW_LOG_INFO("Interactive_Water_VR: StartMod called");

		// Check if already started
		if (s_modStarted.load()) {
			IW_LOG_INFO("StartMod: module already started; ignoring duplicate call");
			return;
		}

		// Try to initialize
		if (TryInitialize()) {
			// Success! Mark as started
			s_modStarted.store(true);
			IW_LOG_INFO("StartMod: initialization successful");
			
			// Schedule logging of SpellInteractionsVR records after a short delay
			std::thread([]() {
				std::this_thread::sleep_for(std::chrono::seconds(3));
				auto taskIntf = SKSE::GetTaskInterface();
				if (taskIntf) {
					taskIntf->AddTask([]() { LogSpellInteractionsVRLoaded(); });
				} else {
					LogSpellInteractionsVRLoaded();
				}
			}).detach();
		} else {
			// Failed - will be retried by ScheduleStartMod polling loop
			IW_LOG_WARN("StartMod: player not ready yet, waiting for retry...");
		}
	}

	void ScheduleStartMod(int delaySeconds)
	{
		// If already started, nothing to do
		if (s_modStarted.load()) {
			IW_LOG_INFO("ScheduleStartMod: module already started, skipping");
			return;
		}

		IW_LOG_INFO("ScheduleStartMod: starting initialization polling (delay=%d seconds)", delaySeconds);

		// Capture the current generation to detect cancellation
		uint32_t myGeneration = s_startGeneration.load();

		// Launch a polling thread that will keep trying until success or cancellation
		std::thread([delaySeconds, myGeneration]() {
			try {
				// Initial delay
				std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
				
				int attempts = 0;
				
				// Keep polling until we succeed, get cancelled, or hit max attempts
				while (attempts < kMaxReschedules) {
					// Check if cancelled
					if (s_startGeneration.load() != myGeneration) {
						IW_LOG_INFO("ScheduleStartMod: cancelled (generation mismatch)");
						return;
					}
					
					// Check if already started by another path
					if (s_modStarted.load()) {
						IW_LOG_INFO("ScheduleStartMod: module already started by another path");
						return;
					}
					
					attempts++;
					IW_LOG_INFO("ScheduleStartMod: attempt %d of %d", attempts, kMaxReschedules);
					
					// Try to initialize on the main thread
					auto taskIntf = SKSE::GetTaskInterface();
					if (taskIntf) {
						// Use a flag to communicate success back
						std::atomic<bool> tryResult{false};
						std::atomic<bool> taskDone{false};
						
						taskIntf->AddTask([&tryResult, &taskDone, myGeneration]() {
							// Check cancellation again on main thread
							if (s_startGeneration.load() != myGeneration || s_modStarted.load()) {
								taskDone.store(true);
								return;
							}
							
							if (TryInitialize()) {
								s_modStarted.store(true);
								tryResult.store(true);
								IW_LOG_INFO("ScheduleStartMod: initialization successful on main thread");
								
								// Schedule spell logging
								std::thread([]() {
									std::this_thread::sleep_for(std::chrono::seconds(3));
									auto ti = SKSE::GetTaskInterface();
									if (ti) {
										ti->AddTask([]() { LogSpellInteractionsVRLoaded(); });
									} else {
										LogSpellInteractionsVRLoaded();
									}
								}).detach();
							}
							taskDone.store(true);
						});
						
						// Wait for task to complete (with timeout)
						int waitMs = 0;
						while (!taskDone.load() && waitMs < 5000) {
							std::this_thread::sleep_for(std::chrono::milliseconds(50));
							waitMs += 50;
						}
						
						if (tryResult.load()) {
							// Success!
							return;
						}
					} else {
						// No task interface - try directly (risky but better than nothing)
						if (TryInitialize()) {
							s_modStarted.store(true);
							IW_LOG_INFO("ScheduleStartMod: initialization successful (direct)");
							return;
						}
					}
					
					// Wait before next attempt
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
				
				IW_LOG_ERROR("ScheduleStartMod: exceeded max attempts (%d), giving up", kMaxReschedules);
				
			} catch (const std::exception& e) {
				IW_LOG_ERROR("ScheduleStartMod: exception: %s", e.what());
			} catch (...) {
				IW_LOG_ERROR("ScheduleStartMod: unknown exception");
			}
		}).detach();
	}
} // namespace InteractiveWaterVR
