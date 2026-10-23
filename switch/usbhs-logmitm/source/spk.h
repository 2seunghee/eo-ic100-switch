#pragma once
#include <stdbool.h>

// Output modes and the background radio on the console speaker (spk.c). eofm / eofm-ovl choose the
// mode through APP_DIR/mode.txt and ask for the speaker through APP_DIR/speaker.txt ("on <gain>");
// this process answers in mode_status.txt and speaker_status.txt.
void spk_init_mode(bool speaker);             // before the mitm starts: the boot mode
void spk_service_start(bool boot_speaker);   // boot_speaker: deny_mic=1 in config.ini
// Speaker mode: no descriptor rewrite, and the audio sysmodule may not take the microphone. Safe to
// call from the mitm thread (reads a variable only).
bool spk_speaker_mode(void);
