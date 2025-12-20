#include "pch.h"
#include <SKSE/SKSE.h>
#include <SKSE/Trampoline.h>
#include <RE/Skyrim.h>
#include "higgsinterface.h"
#include "helper.h"
#include "engine.h"
#include "water_coll_det.h"
#include <cstdint>
#include <fstream>
#include <cstdlib>
#include <cstdio>

// Minimal plugin info struct used by many SKSE loaders
struct SKSEPluginInfo {
	std::uint32_t infoVersion;
	const char* name;
	std::uint32_t version;
};

// Helper: return the plugin-specific log file path inside the SKSE folder in Documents
static std::string GetPluginLogPath()
{
	const char* user = std::getenv("USERPROFILE");
	if (!user) {
		return {};
	}
	return std::string(user) + "\\Documents\\My Games\\Skyrim VR\\SKSE\\Interactive_Water_VR.log";
}

// Messaging callback: called for SKSE messages
static void OnSKSEMessage(SKSE::MessagingInterface::Message* msg)
{
	if (!msg) {
		return;
	}

	switch (msg->type) {
	case SKSE::MessagingInterface::kPostPostLoad: {
		// Try to obtain HIGGS interface via SKSE messaging now that PostPostLoad occurred
		auto pluginHandle = SKSE::GetPluginHandle();
		auto messaging = SKSE::GetMessagingInterface();
		auto higgs = HiggsPluginAPI::GetHiggsInterface001(pluginHandle, const_cast<SKSE::MessagingInterface*>(messaging));
		if (higgs) {
			// Cache interface for later use
			HiggsPluginAPI::g_higgsInterface = higgs;
			unsigned long build = higgs->GetBuildNumber();
			SKSE::log::info("Interactive_Water_VR: obtained HIGGS interface, build {}", build);
			InteractiveWaterVR::AppendToPluginLog("INFO", "Interactive_Water_VR: obtained HIGGS interface, build %lu", build);
			IW_LOG_INFO("Interactive_Water_VR: obtained HIGGS interface");
		} else {
			SKSE::log::info("Interactive_Water_VR: HIGGS interface not available on PostPostLoad");
			IW_LOG_WARN("Interactive_Water_VR: HIGGS interface not available on PostPostLoad");
		}
		// Do NOT start monitoring here - wait for kDataLoaded or kPostLoadGame
		break;
	}
	case SKSE::MessagingInterface::kDataLoaded: {
		IW_LOG_INFO("Interactive_Water_VR: received kDataLoaded message");
		InteractiveWaterVR::LogSpellInteractionsVRLoaded();
		// Schedule a module start attempt after data is available
		InteractiveWaterVR::ScheduleStartMod(2);
		break;
	}
	case SKSE::MessagingInterface::kPreLoadGame: {
		IW_LOG_INFO("Interactive_Water_VR: received kPreLoadGame - resetting all state");

		// CRITICAL: Reset ALL runtime state before loading a new game/save
		// This ensures the plugin reinitializes correctly for the new session
		InteractiveWaterVR::ResetAllRuntimeState();
		break;
	}
	case SKSE::MessagingInterface::kPostLoadGame: {
		IW_LOG_INFO("Interactive_Water_VR: received kPostLoadGame - scheduling module start");
		InteractiveWaterVR::AppendToPluginLog("INFO", "PostLoadGame: scheduling StartMod (from load event)");

		// CRITICAL: Always reset state here too!
		// kPreLoadGame does NOT fire when loading from the main menu (first load of session)
		// So we must ensure state is reset here as well to handle that case
		InteractiveWaterVR::ResetAllRuntimeState();

		// End the load suspension and schedule fresh initialization
		InteractiveWaterVR::NotifyGameLoadEnd();
		InteractiveWaterVR::ScheduleStartMod(2);
		break;
	}
	case SKSE::MessagingInterface::kNewGame: {
		IW_LOG_INFO("Interactive_Water_VR: received kNewGame - resetting all state for new game");
		InteractiveWaterVR::AppendToPluginLog("INFO", "NewGame: resetting state and scheduling StartMod");

		// CRITICAL: Reset ALL runtime state for new game
		// New games need complete reinitialization just like loading a save
		InteractiveWaterVR::ResetAllRuntimeState();

		// End the load suspension and schedule fresh initialization
		InteractiveWaterVR::NotifyGameLoadEnd();
		InteractiveWaterVR::ScheduleStartMod(2);
		break;
	}
	default:
		break;
	}
}

// Minimal Query export used by the SKSE loader to identify the plugin
extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const void* /*a_skse*/, SKSEPluginInfo* a_info)
{
	if (a_info) {
		a_info->infoVersion =1; // standard SKSE info version
		a_info->name = "Interactive_Water_VR"; // shown in loader log
		a_info->version =1; // plugin version
	}
	return true;
}

// Load export called after Query
extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	SKSE::log::info("Interactive_Water_VR loaded");

	// Remove old plugin log so we replace it on each load
	const std::string path = GetPluginLogPath();
	if (!path.empty()) {
		std::remove(path.c_str());
	}

	// Defer trampoline creation to CommonLibSSE-NG API init callback for compatibility
	SKSE::RegisterForAPIInitEvent([]()
	{
		try {
			auto& trampoline = SKSE::GetTrampoline();
			if (trampoline.empty()) {
				constexpr std::size_t TrampolineSize = 64; // adjust if you need more
				trampoline.create(TrampolineSize);
			}

			InteractiveWaterVR::g_trampoline = &trampoline;

			SKSE::log::info("Interactive_Water_VR: trampoline created (capacity = {} bytes)", trampoline.capacity());
			IW_LOG_INFO("Interactive_Water_VR: trampoline created");

			auto messaging = SKSE::GetMessagingInterface();
			if (messaging) {
				bool reg = messaging->RegisterListener("SKSE", OnSKSEMessage);
				SKSE::log::info("Interactive_Water_VR: registered SKSE messaging listener: {}", reg);
				if (reg) {
					IW_LOG_INFO("Interactive_Water_VR: registered SKSE messaging listener");
				} else {
					IW_LOG_ERROR("Interactive_VwaterVR: failed to register SKSE messaging listener");
				}
			} else {
				SKSE::log::warn("Interactive_Water_VR: messaging interface not available during API init");
				IW_LOG_WARN("Interactive_Water_VR: messaging interface not available during API init");
			}

		} catch (const std::exception& e) {
			SKSE::log::error("Interactive_Water_VR: trampoline creation failed: %s", e.what());
			IW_LOG_ERROR("Interactive_Water_VR: trampoline creation failed");
		} catch (...) {
			SKSE::log::error("Interactive_Water_VR: trampoline creation failed: unknown error");
			IW_LOG_ERROR("Interactive_Water_VR: trampoline creation failed");
		}
	});

	// DO NOT call ScheduleStartMod or StartWaterMonitoring here!
	// Wait for proper SKSE messaging events (kDataLoaded, kPostLoadGame, kNewGame)
	// to ensure the game engine is fully initialized before accessing game state.
	IW_LOG_INFO("Interactive_Water_VR: plugin loaded, waiting for game events");

	return true;
}


//THIS IS THE PATH WHERE THE DLL IS SENT
// C:\Users\user\Downloads\commonlibsse-ng-template-main\commonlibsse-ng-template-main\build\windows\x64\release


// TO REBUILD THE SKSE PLUGIN, RUN THIS COMMAND
// &"C:\Program Files\\xmake\\xmake.exe" build skse_plugin