/* What went wrong, said out loud (errsay.h).
 *
 * Stock Alexa answered every failure in words ("Sorry, something went wrong.", "I'm having trouble connecting. I'll
 * keep trying." — SpeechInteractionManager's system_prompt_general_error and system_prompt_error_offline_*,
 * docs/re-ivona.md); hassmic showed the red ring and said nothing, so a broken conversation agent (an Ollama server
 * down) looked like an Echo that did not hear.  The sentence says which part failed, by Home Assistant's error code
 * (homeassistant/components/assist_pipeline/error.py and pipeline.py), and when the message names a service, which
 * one and how (could not reach it, too slow, refused the key...).  Names are written as the voice should say them:
 * the engine reads "Ollama" as "o-llama" with a Spanish double l, and "OpenAI" as one word. */
#include "errsay.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* services a message may name, lower case, and how to say them */
static const char *const services[][2] = {
    { "ollama", "Oh lama" }, { "openai", "Open A I" }, { "open ai", "Open A I" }, { "anthropic", "Anthropic" },
    { "claude", "Claude" }, { "gemini", "Gemini" }, { "google generative", "Gemini" }, { "mistral", "Mistral" },
    { "groq", "Grock" }, { "openrouter", "Open Router" }, { "llama.cpp", "llama C P P" }, { "localai", "Local A I" },
    { "lm studio", "L M Studio" }, { "nabu casa", "Home Assistant Cloud" }, { "home assistant cloud", "Home Assistant Cloud" },
    { "speech-to-phrase", "speech to phrase" }, { "faster-whisper", "Whisper" }, { "faster_whisper", "Whisper" },
    { "whisper", "Whisper" }, { "piper", "Piper" }, { "wyoming", "Wyoming" }, { "elevenlabs", "Eleven Labs" },
    { "azure", "Azure" }, { "deepgram", "Deep gram" },
};

/* how it failed, by words in the message; %s: the service */
static const struct { const char *needle[6]; const char *say; } kinds[] = {
    { { "connect call failed", "connection refused", "cannot connect", "connecterror", "connection error", "all connection attempts failed" }, "I couldn't reach %s." },
    { { "name or service not known", "no route to host", "network is unreachable", "temporary failure in name resolution", "nodename nor servname", "host is unreachable" }, "I couldn't find %s on the network." },
    { { "timed out", "timeout", "deadline exceeded", NULL }, "%s took too long to answer." },
    { { "401", "unauthorized", "invalid api key", "incorrect api key", "authentication", "forbidden" }, "%s didn't accept my key." },
    { { "429", "rate limit", "quota", "too many requests", NULL }, "%s says I've asked too much. Try again later." },
    { { "model", NULL }, NULL },                                 /* "model ... not found": below */
    { { "500", "internal server error", "502", "503", "bad gateway", "service unavailable" }, "%s had a problem on its side." },
};

static void lower(const char *s, char *o, size_t n)
{
    size_t i = 0;
    for (; s && s[i] && i + 1 < n; i++) o[i] = (char)tolower((unsigned char)s[i]);
    o[i] = 0;
}

/* "I couldn't reach Oh lama." from the message, or "" when it says nothing we know.  WHO: what to call the service
 * when the message names none ("the assistant") */
static void cause(const char *msg, const char *who, char *out, size_t n)
{
    char m[1024]; const char *svc = NULL;
    out[0] = 0;
    if (!msg || !*msg) return;
    lower(msg, m, sizeof m);
    for (size_t i = 0; i < sizeof services / sizeof *services && !svc; i++) if (strstr(m, services[i][0])) svc = services[i][1];
    if (strstr(m, "model") && (strstr(m, "not found") || strstr(m, "does not exist") || strstr(m, "pull"))) {
        snprintf(out, n, "%s doesn't have that model.", svc ? svc : who);
    } else {
        for (size_t k = 0; k < sizeof kinds / sizeof *kinds && !out[0]; k++) {
            if (!kinds[k].say) continue;
            for (size_t j = 0; j < 6 && kinds[k].needle[j]; j++)
                if (strstr(m, kinds[k].needle[j])) { snprintf(out, n, kinds[k].say, svc ? svc : who); break; }
        }
        if (!out[0] && svc) snprintf(out, n, "%s had a problem.", svc);
    }
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);  /* "the assistant took..." starts a sentence */
}

void errsay_sentence(const char *code, const char *message, char *out, size_t n)
{
    char c[200];
    if (!n) return;
    out[0] = 0;
    if (!code) code = "";
    /* not failures: nothing said, another satellite answers (proto_esphome.c handles both before this) */
    if (!strcmp(code, "stt-no-text-recognized") || !strcmp(code, "duplicate_wake_up_detected")) return;

    if (!strcmp(code, "no-link")) { snprintf(out, n, "I'm having trouble connecting to Home Assistant. I'll keep trying."); return; }
    if (!strcmp(code, "no-assistant")) { snprintf(out, n, "Home Assistant is connected, but no voice assistant is running for me."); return; }
    if (!strcmp(code, "timeout")) { snprintf(out, n, "Sorry, Home Assistant took too long to answer."); return; }
    if (!strcmp(code, "refused")) { snprintf(out, n, "Sorry, Home Assistant wouldn't start listening."); return; }
    if (!strcmp(code, "pipeline-not-found")) {
        snprintf(out, n, "Sorry, my voice assistant in Home Assistant is missing. Pick one for me on my device page."); return;
    }

    const char *stage, *who;
    if (!strncmp(code, "wake-", 5))       { stage = "Sorry, Home Assistant's wake word engine isn't working."; who = "the wake word engine"; }
    else if (!strcmp(code, "stt-provider-missing")) { snprintf(out, n, "Sorry, my voice assistant has no speech to text."); return; }
    else if (!strcmp(code, "stt-provider-unsupported-metadata")) { snprintf(out, n, "Sorry, speech to text doesn't support my language or audio."); return; }
    else if (!strncmp(code, "stt-", 4))   { stage = "Sorry, speech to text failed."; who = "speech to text"; }
    else if (!strcmp(code, "intent-not-supported")) { snprintf(out, n, "Sorry, my assistant doesn't support this language."); return; }
    else if (!strncmp(code, "intent-", 7)) { stage = "Sorry, the assistant ran into a problem."; who = "the assistant"; }
    else if (!strcmp(code, "tts-not-supported")) { snprintf(out, n, "Sorry, text to speech doesn't support this language or voice."); return; }
    else if (!strncmp(code, "tts-", 4))   { stage = "Sorry, text to speech failed."; who = "text to speech"; }
    else                                   { stage = "Sorry, something went wrong."; who = "Home Assistant"; }

    /* the cause says it better than the stage when the message has one ("Sorry, I couldn't reach Oh lama.") */
    cause(message, who, c, sizeof c);
    if (!c[0]) snprintf(out, n, "%s", stage);
    else if (!strncmp(c, "The ", 4)) snprintf(out, n, "Sorry, the %s", c + 4);   /* "Sorry, the assistant took..." */
    else snprintf(out, n, "Sorry, %s", c);
}
