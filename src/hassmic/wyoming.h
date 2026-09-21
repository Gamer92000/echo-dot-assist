/* Minimal Wyoming protocol framing: JSON header line, optional JSON data, optional binary payload. */
#ifndef WYOMING_H
#define WYOMING_H
#include <stddef.h>

#define WY_MAX_JSON    8192
#define WY_MAX_PAYLOAD (1 << 20)

struct wy_event {
    char  type[48];
    char  json[WY_MAX_JSON];      /* header line followed by the data block, NUL terminated */
    unsigned char *payload;       /* points into a buffer owned by the reader */
    size_t payload_len;
};

struct wy_reader { int fd; unsigned char *buf; };

int  wy_reader_init(struct wy_reader *r, int fd);
void wy_reader_free(struct wy_reader *r);
/* 1 = event read, 0 = peer closed, -1 = error */
int  wy_read(struct wy_reader *r, struct wy_event *ev);

/* data may be NULL; payload may be NULL.  Not thread safe: callers serialise. */
int  wy_write(int fd, const char *type, const char *data, const void *payload, size_t payload_len);

/* Flat key lookup good enough for the fixed messages Home Assistant sends. */
int  wy_json_int(const char *json, const char *key, long *out);
int  wy_json_str(const char *json, const char *key, char *out, size_t outsz);
#endif
