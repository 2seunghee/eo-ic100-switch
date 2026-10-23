#pragma once

// Background radio on the console speaker (spk.c), started when deny_mic=1. eofm / eofm-ovl ask for
// it through APP_DIR/speaker.txt ("on <gain>"), and read APP_DIR/speaker_status.txt.
void spk_service_start(void);
