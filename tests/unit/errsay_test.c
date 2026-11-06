/* errsay.c: Home Assistant's pipeline errors as the sentences the Echo says */
#include "errsay.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *code, const char *msg, const char *want)
{
    char out[300];
    errsay_sentence(code, msg, out, sizeof out);
    if (strcmp(out, want)) { printf("FAIL %s / %s\n  got  \"%s\"\n  want \"%s\"\n", code, msg ? msg : "(null)", out, want); fails++; }
}

int main(void)
{
    /* Ollama down (the user's case, 2026-11-02): intent-failed with HA's wording of the connection error */
    check("intent-failed", "Unexpected error during intent recognition: Error talking to Ollama: [Errno 111] Connect call failed ('192.168.1.5', 11434)",
          "Sorry, I couldn't reach Oh lama.");
    check("intent-failed", "Error talking to OpenAI: Request timed out.", "Sorry, Open A I took too long to answer.");
    check("intent-failed", "Error code: 401 - {'error': {'message': 'Incorrect API key provided'}}", "Sorry, the assistant didn't accept my key.");
    check("intent-failed", "Ollama: model \"llama3.2\" not found, try pulling it first", "Sorry, Oh lama doesn't have that model.");
    check("intent-failed", "Anthropic: 429 rate limit", "Sorry, Anthropic says I've asked too much. Try again later.");
    check("intent-failed", "Something odd", "Sorry, the assistant ran into a problem.");
    check("intent-failed", NULL, "Sorry, the assistant ran into a problem.");
    check("stt-stream-failed", "Cannot connect to host 192.168.1.5:10300 (faster-whisper)", "Sorry, I couldn't reach Whisper.");
    check("stt-stream-failed", "", "Sorry, speech to text failed.");
    check("tts-failed", "Piper: Connection refused", "Sorry, I couldn't reach Piper.");
    check("stt-provider-missing", NULL, "Sorry, my voice assistant has no speech to text.");
    check("pipeline-not-found", "Pipeline 01abc not found", "Sorry, my voice assistant in Home Assistant is missing. Pick one for me on my device page.");
    check("no-link", NULL, "I'm having trouble connecting to Home Assistant. I'll keep trying.");
    check("timeout", NULL, "Sorry, Home Assistant took too long to answer.");
    check("weird-new-code", "", "Sorry, something went wrong.");
    /* not failures: nothing to say */
    check("stt-no-text-recognized", "No text recognized", "");
    check("duplicate_wake_up_detected", NULL, "");
    if (!fails) printf("errsay: all sentences right\n");
    return fails != 0;
}
