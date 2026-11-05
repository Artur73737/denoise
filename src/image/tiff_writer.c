/* Lossless RGB/RGBA 16-bit TIFF output for developed RAW/codec inputs.
 * The source is never rewritten. This stores rendered pixels, not CFA RAW. */
#include "denoise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { uint16_t tag, type; uint32_t count, bytes, offset; const void *data; } Tag;
static void add(Tag *tags, int *count, uint16_t tag, uint16_t type, uint32_t n, const void *data) {
    unsigned width = type == 3 ? 2 : type == 4 ? 4 : type == 5 ? 8 : 1;
    tags[*count] = (Tag){tag, type, n, n * width, 0, data};
    ++*count;
}
static void text(Tag *tags, int *count, uint16_t tag, const char *s) {
    if (s[0]) add(tags, count, tag, 2, (uint32_t)strlen(s) + 1, s);
}
static uint32_t layout(Tag *tags, int count, uint32_t start) {
    int i;
    uint32_t at = start + 2 + (uint32_t)count * 12 + 4;
    for (i = 0; i < count; ++i) if (tags[i].bytes > 4) {
        at = (at + 1) & ~1u;
        tags[i].offset = at;
        at += tags[i].bytes;
    }
    return (at + 1) & ~1u;
}
static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void emit(unsigned char *header, uint32_t start, const Tag *tags, int count) {
    int i;
    put16(header + start, (uint16_t)count);
    for (i = 0; i < count; ++i) {
        unsigned char *entry = header + start + 2 + i * 12;
        put16(entry, tags[i].tag); put16(entry + 2, tags[i].type); put32(entry + 4, tags[i].count);
        if (tags[i].bytes <= 4) memcpy(entry + 8, tags[i].data, tags[i].bytes);
        else { put32(entry + 8, tags[i].offset); memcpy(header + tags[i].offset, tags[i].data, tags[i].bytes); }
    }
}
static int write_all(HANDLE file, const void *data, DWORD bytes) {
    DWORD written;
    return WriteFile(file, data, bytes, &written, NULL) && written == bytes;
}
static void rational(float value, uint32_t out[2]) {
    if (!isfinite(value) || value <= 0) { out[0] = 0; out[1] = 1; }
    else { out[1] = value < 1.0f ? 1000000 : 10000; out[0] = (uint32_t)fmin(4294967295.0, (double)value * out[1] + 0.5); }
}
int dn_tiff_save_new(const wchar_t *path, const DnImage *image, DnError *err) {
    Tag main_tags[32], exif_tags[16];
    int n = 0, ne = 0, ok = 0, reserved = 0;
    const DnMetadata *m = &image->metadata;
    uint16_t bits[4] = {16,16,16,16}, compression = 1, photo = 2;
    uint16_t samples = image->has_alpha ? 4 : 3, orient = m->orientation >= 1 && m->orientation <= 8 ? (uint16_t)m->orientation : 1;
    uint16_t planar = 1, unit = 2, extra = 2, color = image->is_raw ? 1 : 65535;
    uint16_t iso = (uint16_t)fminf(65535, fmaxf(0, m->iso));
    uint32_t stride, pixel_bytes, pixel_offset = 0, exif_offset, header_bytes;
    uint32_t exposure[2], aperture[2], focal[2], xdpi[2], ydpi[2];
    unsigned char *header = NULL;
    uint16_t *row = NULL;
    wchar_t *temp = calloc(DN_PATH, sizeof(wchar_t));
    HANDLE file = INVALID_HANDLE_VALUE;
    UINT x, y, c;
    char description[384];
    if (!temp) return dn_error(err, "Out of memory");
    if (!image->rgba || !image->width || !image->height ||
        (uint64_t)image->width * image->height * samples * 2 > UINT32_MAX - 32 * 1024 * 1024) {
        dn_error(err, "Image is too large for classic TIFF"); goto done;
    }
    stride = image->width * samples * 2;
    pixel_bytes = stride * image->height;
    rational(m->exposure, exposure); rational(m->aperture, aperture); rational(m->focal_length, focal);
    rational((float)(m->xdpi > 0 && m->xdpi < 100000 ? m->xdpi : 72), xdpi);
    rational((float)(m->ydpi > 0 && m->ydpi < 100000 ? m->ydpi : 72), ydpi);
    snprintf(description, sizeof(description), "Denoised image; 16-bit uncompressed TIFF. Source retained unchanged. Decoder: %.63s. Camera-specific metadata remains in the source file.", image->decoder);
    add(main_tags,&n,256,4,1,&image->width); add(main_tags,&n,257,4,1,&image->height);
    add(main_tags,&n,258,3,samples,bits); add(main_tags,&n,259,3,1,&compression); add(main_tags,&n,262,3,1,&photo);
    text(main_tags,&n,270,description); text(main_tags,&n,271,m->make); text(main_tags,&n,272,m->model);
    add(main_tags,&n,273,4,1,&pixel_offset); add(main_tags,&n,274,3,1,&orient);
    add(main_tags,&n,277,3,1,&samples); add(main_tags,&n,278,4,1,&image->height); add(main_tags,&n,279,4,1,&pixel_bytes);
    add(main_tags,&n,282,5,1,xdpi); add(main_tags,&n,283,5,1,ydpi); add(main_tags,&n,284,3,1,&planar); add(main_tags,&n,296,3,1,&unit);
    text(main_tags,&n,305,"Photo Denoise 1.3 (C/ONNX Runtime)"); text(main_tags,&n,306,m->datetime); text(main_tags,&n,315,m->artist);
    if (samples == 4) add(main_tags,&n,338,3,1,&extra);
    add(main_tags,&n,34665,4,1,&exif_offset);
    if (m->icc && m->icc_size) add(main_tags,&n,34675,7,m->icc_size,m->icc);
    if (m->exposure > 0) add(exif_tags,&ne,33434,5,1,exposure);
    if (m->aperture > 0) add(exif_tags,&ne,33437,5,1,aperture);
    if (m->iso > 0) add(exif_tags,&ne,34855,3,1,&iso);
    add(exif_tags,&ne,36864,7,4,"0232"); text(exif_tags,&ne,36867,m->datetime);
    if (m->focal_length > 0) add(exif_tags,&ne,37386,5,1,focal);
    add(exif_tags,&ne,40961,3,1,&color); add(exif_tags,&ne,40962,4,1,&image->width); add(exif_tags,&ne,40963,4,1,&image->height);
    exif_offset = layout(main_tags,n,8);
    header_bytes = layout(exif_tags,ne,exif_offset);
    pixel_offset = header_bytes;
    header = calloc(1, header_bytes);
    row = malloc(stride);
    if (!header || !row) { dn_error(err, "Out of memory while writing TIFF"); goto done; }
    header[0] = header[1] = 'I'; put16(header+2,42); put32(header+4,8);
    emit(header,8,main_tags,n); emit(header,exif_offset,exif_tags,ne);
    if (swprintf_s(temp,DN_PATH,L"%ls.part-%lu-%llu",path,GetCurrentProcessId(),GetTickCount64()) < 0) { dn_error(err,"Path is too long"); goto done; }
    file = CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file == INVALID_HANDLE_VALUE) goto io_error;
    reserved = 1;
    if (!write_all(file,header,header_bytes)) goto io_error;
    for (y=0;y<image->height;++y) {
        if (dn_cancelled()) { dn_error(err,"Canceled; temporary output removed"); goto done; }
        for (x=0;x<image->width;++x) for (c=0;c<samples;++c) {
            float v = image->rgba[((size_t)y*image->width+x)*4+c];
            if (!isfinite(v)) { dn_error(err,"Non-finite pixel value"); goto done; }
            row[(size_t)x*samples+c]=(uint16_t)(fmaxf(0,fminf(1,v))*65535.0f+0.5f);
        }
        if (!write_all(file,row,stride)) goto io_error;
    }
    if (!FlushFileBuffers(file)) goto io_error;
    CloseHandle(file); file=INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temp,path,MOVEFILE_WRITE_THROUGH)) goto io_error;
    ok=1; goto done;
io_error:
    dn_error(err,"Cannot write TIFF without overwriting (Windows error %lu)",GetLastError());
done:
    if (file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    if (reserved && !ok) DeleteFileW(temp);
    free(temp); free(header); free(row); return ok;
}
