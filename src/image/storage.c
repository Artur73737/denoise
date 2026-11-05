/* Preserve the original container byte-for-byte except for denoised samples.
 * Supported: uncompressed integer TIFF strips and uncompressed 24/32-bit BMP.
 * Never silently transcode a lossy/proprietary format or discard metadata. */
#include "denoise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct DnStorage {
    uint64_t size;
    UINT width, height, bits, samples;
    int bmp, little, bottom_up;
    uint64_t bmp_offset, stride;
    UINT rows_per_strip, strip_count;
    uint32_t *offsets, *counts;
};
static int read_at(HANDLE f, uint64_t offset, void *data, DWORD count) {
    LARGE_INTEGER pos;
    DWORD got;
    pos.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(f, pos, NULL, FILE_BEGIN) && ReadFile(f, data, count, &got, NULL) && got == count;
}
static int write_at(HANDLE f, uint64_t offset, const void *data, DWORD count) {
    LARGE_INTEGER pos;
    DWORD got;
    pos.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(f, pos, NULL, FILE_BEGIN) && WriteFile(f, data, count, &got, NULL) && got == count;
}
static uint16_t get16(const unsigned char *p, int le) {
    return le ? (uint16_t)(p[0] | p[1] << 8) : (uint16_t)(p[0] << 8 | p[1]);
}
static uint32_t get32(const unsigned char *p, int le) {
    return le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 :
        (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static const unsigned char *tag_find(const unsigned char *entries, UINT n, unsigned tag, int le) {
    UINT i;
    for (i = 0; i < n; ++i) if (get16(entries + i * 12, le) == tag) return entries + i * 12;
    return NULL;
}
static int tag_value(DnImage *image, const unsigned char *entry, UINT index, uint32_t *out) {
    DnStorage *s = image->storage;
    unsigned char value[4];
    unsigned type;
    uint32_t count, width;
    uint64_t offset;
    if (!entry) return 0;
    type = get16(entry + 2, s->little);
    count = get32(entry + 4, s->little);
    width = type == 3 ? 2 : type == 4 ? 4 : 0;
    if (!width || index >= count) return 0;
    if ((uint64_t)count * width <= 4) { *out = width == 2 ? get16(entry + 8 + index * width, s->little) : get32(entry + 8, s->little); return 1; }
    offset = (uint64_t)get32(entry + 8, s->little) + (uint64_t)index * width;
    if (offset + width > s->size || !read_at(image->source, offset, value, width)) return 0;
    *out = width == 2 ? get16(value, s->little) : get32(value, s->little);
    return 1;
}
static int scalar(DnImage *image, const unsigned char *entries, UINT n, unsigned tag, uint32_t fallback, uint32_t *out) {
    const unsigned char *entry = tag_find(entries, n, tag, image->storage->little);
    if (!entry) { *out = fallback; return 1; }
    if (get32(entry + 4, image->storage->little) != 1) return 0;
    return tag_value(image, entry, 0, out);
}
static int parse_tiff(DnImage *image, const unsigned char *header, DnError *err) {
    DnStorage *s = image->storage;
    unsigned char word[4], *entries = NULL;
    const unsigned char *offsets, *counts, *bits_entry, *sample_entry;
    uint32_t ifd, compression, photometric, planar, predictor, extra, next_ifd;
    uint32_t v;
    UINT n, i;
    int ok = 0;
    s->little = header[0] == 'I';
    if (get16(header + 2, s->little) != 42) return dn_error(err, "Native writer requires classic TIFF; try the RAW/image decoder for this container");
    ifd = get32(header + 4, s->little);
    if (ifd < 8 || (uint64_t)ifd + 2 > s->size || !read_at(image->source, ifd, word, 2)) goto invalid;
    n = get16(word, s->little);
    if (!n || n > 4096 || (uint64_t)ifd + 2 + n * 12 + 4 > s->size) goto invalid;
    entries = malloc((size_t)n * 12);
    if (!entries) return dn_error(err, "Out of memory");
    if (!read_at(image->source, ifd + 2, entries, n * 12) ||
        !read_at(image->source, (uint64_t)ifd + 2 + n * 12, word, 4)) goto invalid;
    next_ifd = get32(word, s->little);
#define SCALAR(tag, fallback, dst) if (!scalar(image, entries, n, tag, fallback, &dst)) goto invalid
    SCALAR(256, 0, s->width); SCALAR(257, 0, s->height);
    SCALAR(259, 1, compression); SCALAR(262, 0, photometric);
    SCALAR(277, 1, s->samples); SCALAR(278, s->height, s->rows_per_strip);
    SCALAR(284, 1, planar); SCALAR(317, 1, predictor); SCALAR(338, 0, extra);
    if (tag_find(entries, n, 50706, s->little) || tag_find(entries, n, 330, s->little)) {
        dn_error(err, "DNG/SubIFD requires the RAW/image decoder, not an embedded preview"); goto done;
    }
    if (next_ifd || compression != 1 || planar != 1 || predictor != 1 ||
        tag_find(entries, n, 322, s->little) || !s->width || !s->height || !s->rows_per_strip ||
        s->width > 100000 || s->height > 100000 || (uint64_t)s->width * s->height > 200000000 ||
        !((photometric == 1 && s->samples == 1) || (photometric == 2 &&
        (s->samples == 3 || (s->samples == 4 && extra == 2))))) {
        dn_error(err, "Native writer supports single-page uncompressed RGB/grayscale TIFF, unsigned 8/16-bit samples and straight alpha"); goto done;
    }
    bits_entry = tag_find(entries, n, 258, s->little);
    sample_entry = tag_find(entries, n, 339, s->little);
    if (!bits_entry || get32(bits_entry + 4, s->little) != s->samples) goto invalid;
    for (i = 0; i < s->samples; ++i) {
        if (!tag_value(image, bits_entry, i, &v) || (v != 8 && v != 16) || (i && v != s->bits)) goto invalid;
        s->bits = v;
        if (sample_entry && (!tag_value(image, sample_entry, i, &v) || v != 1)) {
            dn_error(err, "Native writer cannot edit floating-point/signed TIFF without changing precision"); goto done;
        }
    }
    s->stride = (uint64_t)s->width * s->samples * (s->bits / 8);
    s->strip_count = (s->height - 1) / s->rows_per_strip + 1;
    offsets = tag_find(entries, n, 273, s->little); counts = tag_find(entries, n, 279, s->little);
    if (!offsets || !counts || get32(offsets + 4, s->little) != s->strip_count || get32(counts + 4, s->little) != s->strip_count) goto invalid;
    s->offsets = calloc(s->strip_count, sizeof(uint32_t)); s->counts = calloc(s->strip_count, sizeof(uint32_t));
    if (!s->offsets || !s->counts) { dn_error(err, "Out of memory"); goto done; }
    for (i = 0; i < s->strip_count; ++i) {
        uint64_t first_row = (uint64_t)i * s->rows_per_strip;
        uint64_t rows = s->height - first_row;
        if (rows > s->rows_per_strip) rows = s->rows_per_strip;
        if (!tag_value(image, offsets, i, &s->offsets[i]) || !tag_value(image, counts, i, &s->counts[i]) ||
            s->counts[i] < rows * s->stride || (uint64_t)s->offsets[i] + s->counts[i] > s->size) goto invalid;
    }
    ok = 1; goto done;
invalid:
    dn_error(err, "Invalid TIFF or unsupported native layout/precision");
done:
    free(entries); return ok;
}
static int parse_bmp(DnImage *image, const unsigned char *h, DnError *err) {
    DnStorage *s = image->storage;
    int32_t width = (int32_t)get32(h + 18, 1), height = (int32_t)get32(h + 22, 1);
    unsigned bits = get16(h + 28, 1);
    uint32_t dib = get32(h + 14, 1);
    if (dib < 40 || (uint64_t)dib + 14 > s->size || width <= 0 || width > 100000 ||
        height == 0 || height < -100000 || height > 100000 || get16(h + 26, 1) != 1 ||
        (bits != 24 && bits != 32) || get32(h + 30, 1) != 0)
        return dn_error(err, "Native writer requires uncompressed RGB24/RGB32 BMP");
    s->bmp = 1; s->little = 1; s->bits = 8; s->samples = bits / 8;
    s->width = (UINT)width; s->height = (UINT)(height < 0 ? -height : height); s->bottom_up = height > 0;
    s->stride = ((uint64_t)s->width * s->samples + 3) & ~(uint64_t)3;
    s->bmp_offset = get32(h + 10, 1);
    if (s->bmp_offset < (uint64_t)14 + dib || s->bmp_offset + s->stride * s->height > s->size ||
        (uint64_t)s->width * s->height > 200000000) return dn_error(err, "Truncated or invalid BMP");
    return 1;
}
int dn_storage_open(const wchar_t *path, DnImage *image, DnError *err) {
    unsigned char h[54] = {0};
    LARGE_INTEGER size;
    int ok;
    image->source = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (image->source == INVALID_HANDLE_VALUE) return dn_error(err, "Cannot open the image (Windows error %lu)", GetLastError());
    image->storage = calloc(1, sizeof(DnStorage));
    if (!image->storage) { dn_storage_close(image); return dn_error(err, "Out of memory"); }
    if (!GetFileSizeEx(image->source, &size) || size.QuadPart < 8) { dn_storage_close(image); return dn_error(err, "Invalid image file"); }
    image->storage->size = (uint64_t)size.QuadPart;
    if (!read_at(image->source, 0, h, size.QuadPart < 54 ? (DWORD)size.QuadPart : 54)) {
        dn_storage_close(image); return dn_error(err, "Image read failed");
    }
    if (h[0] == 'I' && h[1] == 'I' && h[8] == 'C' && h[9] == 'R')
        ok = dn_error(err, "Canon CR2 requires the RAW decoder, not the TIFF preview");
    else if ((h[0] == 'I' && h[1] == 'I') || (h[0] == 'M' && h[1] == 'M')) ok = parse_tiff(image, h, err);
    else if (h[0] == 'B' && h[1] == 'M' && size.QuadPart >= 54) ok = parse_bmp(image, h, err);
    else if (h[0] == 255 && h[1] == 216)
        ok = dn_error(err, "JPEG requires decoding to lossless TIFF output");
    else ok = dn_error(err, "Not a native editable TIFF/BMP container; try the RAW/image decoder");
    if (!ok) dn_storage_close(image);
    return ok;
}
void dn_storage_close(DnImage *image) {
    if (image->source && image->source != INVALID_HANDLE_VALUE) CloseHandle(image->source);
    image->source = NULL;
    if (image->storage) { free(image->storage->offsets); free(image->storage->counts); free(image->storage); image->storage = NULL; }
}
int dn_native_load(const wchar_t *path, DnImage *image, DnError *err) {
    DnStorage *s;
    unsigned char *row = NULL;
    UINT x, y, c;
    int ok = 0;
    memset(image, 0, sizeof(*image));
    if (!dn_storage_open(path, image, err)) return 0;
    s = image->storage;
    image->has_alpha = !s->bmp && s->samples == 4;
    strcpy_s(image->decoder, sizeof(image->decoder), "TIFF/BMP conservativo");
    image->width = s->width; image->height = s->height;
    image->rgba = malloc((size_t)s->width * s->height * 4 * sizeof(float));
    row = malloc((size_t)s->stride);
    if (!row || !image->rgba) { dn_error(err, "Out of memory"); goto done; }
    image->grayscale = 1;
    for (y = 0; y < s->height; ++y) {
        if (dn_cancelled()) { dn_error(err, "Canceled"); goto done; }
        uint64_t pos = s->bmp ? s->bmp_offset + (uint64_t)(s->bottom_up ? s->height - 1 - y : y) * s->stride :
            (uint64_t)s->offsets[y / s->rows_per_strip] + (uint64_t)(y % s->rows_per_strip) * s->stride;
        if (!read_at(image->source, pos, row, (DWORD)s->stride)) { dn_error(err, "Pixel read failed"); goto done; }
        for (x = 0; x < s->width; ++x) {
            size_t dst = ((size_t)y * s->width + x) * 4;
            for (c = 0; c < 3; ++c) {
                size_t src = ((size_t)x * s->samples + (s->bmp ? 2 - c : s->samples == 1 ? 0 : c)) * (s->bits / 8);
                unsigned value = s->bits == 8 ? row[src] : get16(row + src, s->little);
                image->rgba[dst + c] = value / (s->bits == 8 ? 255.0f : 65535.0f);
            }
            image->rgba[dst + 3] = 1.0f;
            if (!s->bmp && s->samples == 4) {
                size_t src = ((size_t)x * 4 + 3) * (s->bits / 8);
                unsigned value = s->bits == 8 ? row[src] : get16(row + src, s->little);
                image->rgba[dst + 3] = value / (s->bits == 8 ? 255.0f : 65535.0f);
            }
            if (image->rgba[dst] != image->rgba[dst + 1] || image->rgba[dst] != image->rgba[dst + 2]) image->grayscale = 0;
        }
    }
    ok = 1;
done:
    free(row);
    if (!ok) dn_image_free(image);
    return ok;
}
void dn_image_free(DnImage *image) {
    free(image->rgba);
    free(image->metadata.icc);
    dn_storage_close(image);
    memset(image, 0, sizeof(*image));
}
int dn_image_save_new(const wchar_t *path, const DnImage *image, DnError *err) {
    const DnStorage *s = image->storage;
    if (!s) return dn_tiff_save_new(path, image, err);
    wchar_t *temp = calloc(DN_PATH, sizeof(wchar_t));
    HANDLE out = INVALID_HANDLE_VALUE;
    unsigned char buffer[65536], *row = NULL;
    uint64_t pos;
    UINT y, x, c;
    int ok = 0, reserved = 0;
    if (!s || s->width != image->width || s->height != image->height) {
        free(temp); return dn_error(err, "Image dimensions changed; native save refused");
    }
    if (!temp) return dn_error(err, "Out of memory");
    if (swprintf_s(temp, DN_PATH, L"%ls.part-%lu-%llu", path, GetCurrentProcessId(), GetTickCount64()) < 0) {
        dn_error(err, "Path is too long"); goto done;
    }
    out = CreateFileW(temp, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) goto io_error;
    reserved = 1;
    /* Copy all original bytes, including metadata, ICC profiles and unknown tags. */
    for (pos = 0; pos < s->size;) {
        DWORD n = s->size - pos < sizeof(buffer) ? (DWORD)(s->size - pos) : sizeof(buffer);
        if (!read_at(image->source, pos, buffer, n) || !write_at(out, pos, buffer, n)) goto io_error;
        pos += n;
    }
    row = malloc((size_t)s->stride);
    if (!row) { dn_error(err, "Out of memory"); goto done; }
    for (y = 0; y < s->height; ++y) {
        if (dn_cancelled()) { dn_error(err, "Canceled; temporary output removed"); goto done; }
        if (s->bmp) pos = s->bmp_offset + (uint64_t)(s->bottom_up ? s->height - 1 - y : y) * s->stride;
        else pos = (uint64_t)s->offsets[y / s->rows_per_strip] + (uint64_t)(y % s->rows_per_strip) * s->stride;
        if (!read_at(image->source, pos, row, (DWORD)s->stride)) goto io_error;
        for (x = 0; x < s->width; ++x) {
            for (c = 0; c < (s->samples == 1 ? 1u : 3u); ++c) {
                size_t dest = ((size_t)x * s->samples + (s->bmp ? 2 - c : c)) * (s->bits / 8);
                float v = image->rgba[((size_t)y * s->width + x) * 4 + c];
                unsigned value;
                if (!isfinite(v)) { dn_error(err, "Non-finite pixel value"); goto done; }
                value = (unsigned)(fmaxf(0, fminf(1, v)) * (s->bits == 8 ? 255.0f : 65535.0f) + 0.5f);
                if (s->bits == 8) row[dest] = (unsigned char)value;
                else if (s->little) { row[dest] = (unsigned char)value; row[dest + 1] = (unsigned char)(value >> 8); }
                else { row[dest] = (unsigned char)(value >> 8); row[dest + 1] = (unsigned char)value; }
            }
        }
        /* Alpha, BMP row padding and reserved samples retain original bytes. */
        if (!write_at(out, pos, row, (DWORD)s->stride)) goto io_error;
    }
    if (!FlushFileBuffers(out)) goto io_error;
    CloseHandle(out); out = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temp, path, MOVEFILE_WRITE_THROUGH)) goto io_error;
    ok = 1; goto done;
io_error:
    dn_error(err, "Cannot save without overwriting (Windows error %lu)", GetLastError());
done:
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (reserved && !ok) DeleteFileW(temp);
    free(row); free(temp); return ok;
}
