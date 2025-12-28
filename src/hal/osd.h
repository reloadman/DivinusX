#pragma once

#include <stdbool.h>

#include "types.h"

// Returns false when OSD must not be attached to the given channel
// (e.g. MJPEG per-stream OSD disable).
bool hal_osd_is_allowed_for_channel(const hal_chnstate *st);


