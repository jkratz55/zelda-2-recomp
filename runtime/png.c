/* png.c - minimal uncompressed PNG writer for screenshots. */
#include <stdio.h>
#include <stdlib.h>

#include "nes.h"

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    put32(f, len);
    uint8_t *buf = malloc(len + 4);
    for (int i = 0; i < 4; i++) buf[i] = (uint8_t)type[i];
    for (uint32_t i = 0; i < len; i++) buf[4 + i] = data[i];
    fwrite(buf, 1, len + 4, f);
    put32(f, crc32_buf(0, buf, len + 4));
    free(buf);
}

bool png_write(const char *path, const uint32_t *argb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                        8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr, 13);

    /* raw scanlines (filter byte 0 + RGB), wrapped in stored deflate blocks */
    uint32_t row = (uint32_t)w * 3 + 1, raw_len = row * (uint32_t)h;
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = argb[y * w + x];
            uint8_t *o = raw + y * row + 1 + x * 3;
            o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p;
        }
    }
    uint32_t nblocks = (raw_len + 65534) / 65535;
    uint32_t z_len = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = malloc(z_len), *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (uint32_t pos = 0; pos < raw_len;) {
        uint32_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        *o++ = pos + n == raw_len;
        *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        for (uint32_t i = 0; i < n; i++) {
            *o++ = raw[pos + i];
            a = (a + raw[pos + i]) % 65521;
            b = (b + a) % 65521;
        }
        pos += n;
    }
    uint32_t adler = (b << 16) | a;
    *o++ = (uint8_t)(adler >> 24); *o++ = (uint8_t)(adler >> 16); *o++ = (uint8_t)(adler >> 8); *o++ = (uint8_t)adler;
    chunk(f, "IDAT", z, (uint32_t)(o - z));
    chunk(f, "IEND", NULL, 0);
    free(raw);
    free(z);
    return fclose(f) == 0;
}
