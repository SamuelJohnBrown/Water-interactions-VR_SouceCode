#pragma once

namespace InteractiveWaterVR {
// Start/stop monitor that will unequip selected spells / manage shock/frost behaviors.
void StartSpellUnequipMonitor();
void StopSpellUnequipMonitor();

// Clear all cached form pointers for spell interactions - MUST be called on game load
void ClearSpellInteractionCachedForms();

// Note: historical names kept for compatibility with other code that may include the old header
inline void StartSpellUnequipMonitorAlias() { StartSpellUnequipMonitor(); }
inline void StopSpellUnequipMonitorAlias() { StopSpellUnequipMonitor(); }
}
