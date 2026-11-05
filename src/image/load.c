#include "denoise.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <icm.h>

int dn_image_load(const wchar_t *path, DnImage *image, DnError *err) {
    DnError native_error = {{0}}, raw_error = {{0}}, wic_error = {{0}};
    if (dn_native_load(path, image, &native_error)) return 1;
    if (dn_cancelled()) return dn_error(err, "Canceled");
    if (dn_raw_load(path, image, &raw_error)) return 1;
    if (dn_cancelled()) return dn_error(err, "Canceled");
    if (dn_wic_load(path, image, &wic_error)) return 1;
    return dn_error(err, "Cannot decode this file: damaged data or unsupported format/camera. RAW: %.700s. Image codec: %.500s.",
        raw_error.message, wic_error.message);
}
void dn_set_srgb_profile(DnImage *image) {
    wchar_t path[MAX_PATH];
    DWORD length = MAX_PATH, read;
    HANDLE file;
    LARGE_INTEGER size;
    if (!GetStandardColorSpaceProfileW(NULL, LCS_sRGB, path, &length)) return;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 16 * 1024 * 1024) {
        image->metadata.icc = malloc((size_t)size.QuadPart);
        if (image->metadata.icc && ReadFile(file, image->metadata.icc, (DWORD)size.QuadPart, &read, NULL) && read == size.QuadPart)
            image->metadata.icc_size = read;
        else { free(image->metadata.icc); image->metadata.icc = NULL; }
    }
    CloseHandle(file);
}
