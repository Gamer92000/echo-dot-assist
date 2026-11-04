/* No voice of the Echo's own (speech.h): the Dots' firmware has no speech engine, the PC build none either */
#include "speech.h"

void speech_start(const struct speech_out *out) { (void)out; }
int  speech_installed(void) { return 0; }
int  speech_ready(void) { return 0; }
const char *speech_voice(void) { return ""; }
const char *speech_language(void) { return ""; }
int  speech_say(const char *text, int tag) { (void)text; (void)tag; return -1; }
int  speech_render(const char *text, const struct speech_sink *sink) { (void)text; (void)sink; return -1; }
void speech_cancel(void) {}
int  speech_voices(const char *const **names) { *names = 0; return 0; }
int  speech_voice_index(void) { return -1; }
void speech_voice_select(int i) { (void)i; }
