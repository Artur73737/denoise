#include "denoise.h"
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>

static DnProgressFn progress_callback;
static void *progress_context;
static volatile LONG cancelled;
void dn_set_progress(DnProgressFn callback, void *context) { progress_callback = callback; progress_context = context; }
void dn_cancel_reset(void) { InterlockedExchange(&cancelled, 0); }
void dn_cancel(void) { InterlockedExchange(&cancelled, 1); }
int dn_cancelled(void) { return InterlockedCompareExchange(&cancelled, 0, 0) != 0; }
const char *dn_stage_name(DnStage stage) {
    static const char *names[] = {"Ready", "Reading image", "Verifying model", "Downloading model", "Loading model", "Denoising", "Saving image", "Completed"};
    return stage >= DN_IDLE && stage <= DN_DONE ? names[stage] : "Working";
}
void dn_progress(DnStage stage, uint64_t done, uint64_t total, const char *subject, const char *message) {
    DnProgress p = {0};
    p.stage = stage; p.done = done; p.total = total;
    if (subject) strncpy_s(p.subject, sizeof(p.subject), subject, _TRUNCATE);
    if (message) strncpy_s(p.message, sizeof(p.message), message, _TRUNCATE);
    if (progress_callback) progress_callback(progress_context, &p);
}
void dn_status(DnStage stage, const char *fmt, ...) {
    char message[768];
    va_list args;
    va_start(args, fmt); vsnprintf(message, sizeof(message), fmt, args); va_end(args);
    dn_progress(stage, 0, 0, NULL, message);
}
int dn_models_directory(wchar_t *path, DnError *err) {
    DWORD n = GetModuleFileNameW(NULL, path, DN_PATH);
    wchar_t *slash;
    if (!n || n >= DN_PATH) return dn_error(err, "Cannot locate the executable directory");
    slash = wcsrchr(path, L'\\');
    if (!slash) return dn_error(err, "Cannot locate the executable directory");
    *slash = 0;
    if (wcscat_s(path, DN_PATH, L"\\models")) return dn_error(err, "Model path is too long");
    return 1;
}

int dn_error(DnError *err, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, args);
    va_end(args);
    return 0;
}
int dn_hresult(DnError *err, const char *op, HRESULT hr) {
    return dn_error(err, "%s: HRESULT 0x%08lX", op, (unsigned long)hr);
}
int dn_path_join(wchar_t *out, size_t capacity, const wchar_t *a, const wchar_t *b, DnError *err) {
    if (swprintf_s(out, capacity, L"%ls\\%ls", a, b) < 0)
        return dn_error(err, "Path is too long");
    return 1;
}
int dn_mkdir(const wchar_t *path, DnError *err) {
    DWORD attrs;
    if (CreateDirectoryW(path, NULL)) return 1;
    attrs = GetFileAttributesW(path);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    return dn_error(err, "Cannot create the models directory (Windows error %lu)", GetLastError());
}
int dn_reflect(int x, int size) {
    int period;
    if (size <= 1) return 0;
    period = 2 * (size - 1);
    x %= period;
    if (x < 0) x += period;
    return x < size ? x : period - x;
}
int dn_same_file(const wchar_t *a, const wchar_t *b) {
    BY_HANDLE_FILE_INFORMATION ia, ib;
    HANDLE ha = CreateFileW(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hb = CreateFileW(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    int same = 0;
    if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib))
        same = ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
               ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
}
