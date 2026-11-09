/* No device or sound output: the actual Cube backend talks to a local fake helper. */
#include "audio.h"
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int listener, earcons;
static const int16_t pcm[] = { -1000, -1, 0, 1, 1000 };
void wake_property(const char *name, int value) { (void)name; (void)value; }
static void exact(int fd, void *p, size_t n, int sending) {
    while (n) { ssize_t k = sending ? write(fd, p, n) : read(fd, p, n); assert(k > 0); p = (char *)p + k; n -= k; }
}
static uint32_t get(int fd) { uint32_t n; exact(fd, &n, 4, 0); return ntohl(n); }
static void put(int fd, uint32_t n) { n = htonl(n); exact(fd, &n, 4, 1); }
static void started(unsigned lead_ms) { assert(lead_ms == 0); earcons++; }
static void *helper(void *unused) {
    (void)unused;
    for (int i = 0; i < 4; i++) {
        int fd = accept(listener, NULL, NULL); assert(fd >= 0);
        assert(get(fd) == 0x43554241);
        uint32_t role = get(fd), a = get(fd), b = get(fd);
        put(fd, 0);
        if (i < 2) {
            assert(role == 6 && a == 2 && b == (uint32_t)(i == 0)); put(fd, 0);
        } else if (i == 2) {
            assert(role == 0 && a == 16000 && b == 1);
            put(fd, sizeof pcm); exact(fd, (void *)pcm, sizeof pcm, 1);
        } else {
            assert(role == 3 && a == 16000 && b == 1);
            assert(get(fd) == 1 && get(fd) == sizeof pcm);
            int16_t received[5]; exact(fd, received, sizeof received, 0);
            assert(!memcmp(received, pcm, sizeof pcm)); put(fd, sizeof pcm);
            assert(get(fd) == 3 && get(fd) == 1); put(fd, 0);
        }
        close(fd);
    }
    return NULL;
}
int main(void) {
    assert(music_prime_us() == 0);
    alarm(15); /* Bound any failed IPC assertion or handshake. */
    assert(vol_read(VOL_MAIN, 99) == 40 && vol_read(VOL_TTS, 99) == 40);
    vol_write(VOL_MAIN, 53); vol_write(VOL_TTS, 44); vol_write(VOL_MAIN, 101);
    assert(vol_read(VOL_MAIN, 99) == 53 && vol_read(VOL_TTS, 99) == 44);
    assert(vol_read(VOL_MUTE, 1) == 0 && vol_read((enum vol)99, 27) == 27);
    assert(audio_a2dp_route(1, 0, "test") == -1 && audio_a2dp_route(0, 0, "test") == 0);
    assert(audio_a2dp_lost(0) == 1 && audio_a2dp_aipc() == 0); audio_a2dp_reset();
    char json[16] = "old";
    assert(afe_arbitration(0, 1, json, sizeof json) == 0 && !json[0]);
    assert(afe_eq_get(json, sizeof json) == 0); afe_eq_set("{}"); afe_stream_stopped();
    char name[80]; snprintf(name, sizeof name, "cube-audio-test-%d", getpid()); setenv("CUBE_AUDIO_SOCKET", name, 1);
    struct sockaddr_un address = { .sun_family = AF_UNIX }; memcpy(address.sun_path + 1, name, strlen(name));
    listener = socket(AF_UNIX, SOCK_STREAM, 0); assert(listener >= 0);
    assert(!bind(listener, (void *)&address, sizeof(sa_family_t) + 1 + strlen(name))); assert(!listen(listener, 4));
    pthread_t thread; assert(!pthread_create(&thread, NULL, helper, NULL));
    afe_listening(1); afe_listening(0);
    const void *captured; assert(!cap_open()); assert(cap_read(&captured) == sizeof pcm);
    assert(!memcmp(captured, pcm, sizeof pcm)); cap_close();
    vol_write(VOL_MAIN, 100); audio_earcon_started = started; play_earcon(pcm, 5, 16000);
    assert(earcons == 1); pthread_join(thread, NULL); close(listener);
    puts("PASS: Cube volume, AFE listening IPC, unmodified capture and earcon callback");
}
