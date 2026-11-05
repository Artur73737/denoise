#include "denoise.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Immutable upstream revision. Hashes are the LFS SHA-256 object identifiers. */
#define REV L"6d11417ee2fbcc73783c502a238ac115097754fe"
static const DnModel catalog[] = {
 {"scunet_color_15",3,"228dfb72f8b5e23d35c0a92048b46ada31676b34e90a1ced6d221377861ffd6b","4f2cdeeade2744b8fefdae9d572c8c71d84bc153e4863579c5f8fe680a3b1c74",3795638,"Color / Light noise","RGB photos with light Gaussian noise (sigma 15). Preserves more fine detail on lightly noisy input.","Quality-focused; DirectML GPU recommended."},
 {"scunet_color_25",3,"76170f1fcb84d3b3d38c4e7133a658bbbb70504e3dc5e4e4d1f7b0c395c093be","f46aa0f533e5bb8b2485a6531a205e270f4a7be2447880a12d97f5f00b1e70c9",3795638,"Color / Moderate noise","RGB photos with moderate Gaussian noise (sigma 25). A balanced starting point for noisy photos.","Same SCUNet architecture; CPU is slower on large photos."},
 {"scunet_color_50",3,"691b995af0700809ddf882265de4a68373b2d2af9d4a89daca662b828686229d","a12e0931304ea43d3367d5f5848099e46fdafe07fd2126ed207ceca02989d446",3795638,"Color / Heavy noise","RGB photos with strong Gaussian noise (sigma 50). More smoothing; used by automatic strength 67-100.","Quality-focused; fixed tile size bounds GPU memory."},
 {"scunet_gray_15",1,"c6232300c76cbf7b616bfc6a37376c5ff1896a20714d0a055307b617a6211f22","95bf90d3d1fef6b42bfd9c721d08435c6d741bf15d8c3cca2669f8bd0cd758e9",3795216,"Grayscale / Light noise","One-channel images with light Gaussian noise (sigma 15). Requires a grayscale image.","One-channel I/O; most computation uses the same architecture."},
 {"scunet_gray_25",1,"9ce980024c318cbe73e0f5b2c654b13f8c4d361736f1b5382850896f8cb43826","22d33a24188569d12e7dbdd64dc574edaf9936eb96044ac701e8fabd5eda05e3",3795216,"Grayscale / Moderate noise","One-channel images with moderate Gaussian noise (sigma 25). Requires a grayscale image.","One-channel I/O; GPU recommended for large images."},
 {"scunet_gray_50",1,"bef6220daaddcd5c69c01239eee9677b6e92f1ae8e1e13c36718cb5c2a8aefd1","e272dbb7cc2e0887a49bdade8f32e73d057b664303e29c43edcf7f46d8d93ce3",3795216,"Grayscale / Heavy noise","One-channel images with heavy Gaussian noise (sigma 50). Requires a grayscale image.","Same SCUNet architecture; do not assume a measured speedup."},
 {"scunet_color_real_psnr",3,"231be201ab413dbc999d7951caa9844846b93a12a40a41e037d6b5888ed4e88c","98825ea1210b641c71e5f052f582c70c49fd44b35387ebe2c034268c17df3feb",3798678,"Color / Real photo noise","Blind real-world photo denoising, optimized for fidelity. Useful for mixed camera noise; avoids GAN texture synthesis.","Quality-focused; benchmark on your own GPU and tile size."}
};
size_t dn_model_count(void) { return sizeof(catalog) / sizeof(catalog[0]); }
const DnModel *dn_model_at(size_t index) { return index < dn_model_count() ? &catalog[index] : NULL; }
const DnModel *dn_model_find(const char *id) {
    char name[128];
    size_t n, i;
    if (!id || strlen(id) >= sizeof(name)) return NULL;
    strcpy_s(name, sizeof(name), id);
    n = strlen(name);
    if (n > 5 && !_stricmp(name + n - 5, ".onnx")) name[n - 5] = 0;
    for (i = 0; i < dn_model_count(); ++i) if (!_stricmp(name, catalog[i].id)) return &catalog[i];
    return NULL;
}
uint64_t dn_model_size(const DnModel *model) { return (uint64_t)model->graph_bytes + 73138176; }
int dn_model_ready(const DnModel *model, const wchar_t *cache) {
    wchar_t file[128];
    wchar_t *path = calloc(DN_PATH, sizeof(wchar_t));
    DnError err = {{0}};
    int i, ready = 1;
    if (!path) return 0;
    for (i = 0; i < 2; ++i) {
        swprintf_s(file, 128, L"%hs.onnx%ls", model->id, i ? L".data" : L"");
        if (!dn_path_join(path, DN_PATH, cache, file, &err) ||
            !dn_file_verify(path, i ? model->weights_hash : model->graph_hash, i ? 73138176 : model->graph_bytes)) { ready = 0; break; }
    }
    free(path); return ready;
}
const DnModel *dn_model_select(int grayscale, int amount, const char *override) {
    int tier = amount <= 33 ? 0 : amount <= 66 ? 1 : 2;
    const DnModel *explicit_model = dn_model_find(override);
    if (explicit_model) return explicit_model;
    if (!strcmp(override, "real")) return &catalog[6];
    if (!strcmp(override, "15")) tier = 0;
    else if (!strcmp(override, "25")) tier = 1;
    else if (!strcmp(override, "50")) tier = 2;
    else if (strcmp(override, "auto")) return NULL;
    return &catalog[(grayscale ? 3 : 0) + tier];
}
int dn_model_ensure(const DnModel *model, const wchar_t *cache, int offline, wchar_t *graph, DnError *err) {
    wchar_t file[128], url[512];
    wchar_t *path = calloc(DN_PATH, sizeof(wchar_t));
    int i, ok = 0;
    if (!path) return dn_error(err, "Out of memory");
    if (!dn_mkdir(cache, err)) goto done;
    for (i = 0; i < 2; ++i) {
        const char *hash = i ? model->weights_hash : model->graph_hash;
        size_t bytes = i ? 73138176 : model->graph_bytes;
        swprintf_s(file, 128, L"%hs.onnx%ls", model->id, i ? L".data" : L"");
        if (!dn_path_join(path, DN_PATH, cache, file, err)) goto done;
        dn_progress(DN_VERIFY, i, 2, model->id, i ? "Checking weight file (SHA-256)" : "Checking model graph (SHA-256)");
        if (dn_cancelled()) { dn_error(err, "Canceled"); goto done; }
        if (!dn_file_verify(path, hash, bytes)) {
            if (offline) { dn_error(err, "Model %s is missing or incomplete. Offline mode prevents download. Run 'denoise.exe model' to install it.", model->id); goto done; }
            swprintf_s(url, 512, L"https://huggingface.co/Heliosoph/scunet-onnx/resolve/" REV L"/%ls?download=true", file);
            if (!dn_download(url, path, hash, bytes, err)) goto done;
        }
        if (!i) wcscpy_s(graph, DN_PATH, path);
    }
    dn_progress(DN_VERIFY, 2, 2, model->id, "Model verified and ready");
    ok = 1;
done:
    free(path);
    return ok;
}
