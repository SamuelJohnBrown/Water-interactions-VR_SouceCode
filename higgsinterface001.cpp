#include "higgsinterface.h"
#include <SKSE/SKSE.h>
#include <cassert>

// Define the global interface pointer declared as extern in the header
namespace HiggsPluginAPI {
 IHiggsInterface001* g_higgsInterface = nullptr;
}

// A message used to fetch HIGGS's interface
struct HiggsMessage
{
	// Make the message id unsigned to avoid signed/unsigned conversion warnings
	enum { kMessage_GetInterface =0xF9279A57u };
	void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
};

// Fetches the interface to use from HIGGS
HiggsPluginAPI::IHiggsInterface001* HiggsPluginAPI::GetHiggsInterface001(const SKSE::PluginHandle& pluginHandle, SKSE::MessagingInterface* messagingInterface)
{
	// Mark pluginHandle as intentionally unused in this implementation
	(void)pluginHandle;

	// If the interface has already been fetched, return the same object
	if (g_higgsInterface) {
		SKSE::log::info("GetHiggsInterface001: returning cached interface");
		return g_higgsInterface;
	}

	if (!messagingInterface) {
		SKSE::log::warn("GetHiggsInterface001: messagingInterface is null");
		return nullptr;
	}

	// Dispatch a message to get the plugin interface from HIGGS
	HiggsMessage higgsMessage{};
	bool dispatched = messagingInterface->Dispatch(static_cast<std::uint32_t>(HiggsMessage::kMessage_GetInterface), &higgsMessage, static_cast<std::uint32_t>(sizeof(higgsMessage)), "HIGGS");
	SKSE::log::info("GetHiggsInterface001: Dispatch returned {}", dispatched);

	if (!dispatched) {
		SKSE::log::warn("GetHiggsInterface001: dispatch failed or no recipient handled the message");
	}

	if (!higgsMessage.GetApiFunction) {
		SKSE::log::info("GetHiggsInterface001: Higgs did not provide GetApiFunction (null)");
		return nullptr;
	}

	// Fetch the API for this version of the HIGGS interface
	g_higgsInterface = static_cast<HiggsPluginAPI::IHiggsInterface001*>(higgsMessage.GetApiFunction(1));
	if (g_higgsInterface) {
		SKSE::log::info("GetHiggsInterface001: obtained Higgs interface: {}", reinterpret_cast<uintptr_t>(g_higgsInterface));
	} else {
		SKSE::log::warn("GetHiggsInterface001: GetApiFunction returned null for revision1");
	}

	return g_higgsInterface;
}
