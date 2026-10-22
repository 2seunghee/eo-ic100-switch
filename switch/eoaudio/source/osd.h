#pragma once
#include "common.h"

// On-screen earphone volume indicator, laid out like the console's own volume indicator (dark
// rounded panel at the top left, one block per volume level) with an earphone icon instead of the
// speaker. Levels: volume_max_db down to volume_min_db in volume_step_db steps, plus level 0
// (silent). Shown for 1.5 s after each change, on a vi managed layer of its own (the way Tesla
// overlays draw), created when shown and closed with its vi/nv sessions once hidden. Drawing happens on a
// low-priority thread; osd_show_volume only queues the value.
void osd_init(void);
void osd_show_volume(float db, float min_db, float max_db, float step_db, bool muted);   // any thread
