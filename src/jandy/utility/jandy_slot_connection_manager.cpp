#include "utility/jandy_slot_connection_manager.h"

// JandySlotConnectionManager is header-only (it only adds an inline, templated
// RegisterSlot_FilterByDeviceId on top of the shared SlotConnectionManager base,
// whose ctor/dtor live in the core library). This translation unit exists only to
// anchor the header in the build.
