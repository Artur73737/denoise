#include "denoise.h"
#include <objbase.h>
#include <stdio.h>
int wmain(int argc, wchar_t **argv) {
    DnImage image = {0};
    DnError err = {{0}};
    int ok;
    if (argc != 3 || FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 2;
    ok = dn_image_load(argv[1], &image, &err) && dn_image_save_new(argv[2], &image, &err);
    if (!ok) fprintf(stderr, "%s\n", err.message);
    dn_image_free(&image);
    CoUninitialize();
    return ok ? 0 : 1;
}
