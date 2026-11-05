#include "denoise.h"
#include <dxgi1_2.h>
#include <d3d12.h>
#include <onnxruntime_c_api.h>
#include <dml_provider_factory.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

struct DnBackend {
    const OrtApi *api;
    OrtEnv *env;
    OrtSession *session;
    OrtMemoryInfo *memory;
    OrtAllocator *allocator;
    char *input_name, *output_name;
    int input_channels, output_channels, tile, gpu;
    char name[256];
};
typedef struct { int id; SIZE_T memory; wchar_t name[128]; } DnGpu;

static int gpu_compare(const void *a, const void *b) {
    const DnGpu *x = a, *y = b;
    return x->memory < y->memory ? 1 : x->memory > y->memory ? -1 : 0;
}
static int gpu_inventory(DnGpu *out, int capacity, int print) {
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    UINT i;
    int count = 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) return 0;
    for (i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        ID3D12Device *device = NULL;
        int available = 0;
        if (!adapter) break;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))) {
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                available = SUCCEEDED(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                    &IID_ID3D12Device, (void **)&device));
            if (print) fwprintf(stdout, L"[%u] %ls - VRAM %zu MiB - DirectX 12: %ls\n", i,
                desc.Description, desc.DedicatedVideoMemory / (1024 * 1024), available ? L"yes" : L"no");
            if (available && count < capacity) {
                out[count].id = (int)i;
                out[count].memory = desc.DedicatedVideoMemory;
                wcscpy_s(out[count].name, 128, desc.Description);
                ++count;
            }
        }
        if (device) ID3D12Device_Release(device);
        IDXGIAdapter1_Release(adapter); adapter = NULL;
    }
    IDXGIFactory1_Release(factory);
    qsort(out, count, sizeof(*out), gpu_compare);
    return count;
}
int dn_gpu_list(void) { DnGpu gpus[32]; return gpu_inventory(gpus, 32, 1); }

static int ort_check(DnBackend *b, OrtStatus *status, DnError *err) {
    if (!status) return 1;
    dn_error(err, "ONNX Runtime: %s", b->api->GetErrorMessage(status));
    b->api->ReleaseStatus(status);
    return 0;
}
static void ORT_API_CALL runtime_log(void *parameter, OrtLoggingLevel severity, const char *category,
    const char *logid, const char *location, const char *message) {
    (void)parameter; (void)category; (void)logid; (void)location;
    if (severity >= ORT_LOGGING_LEVEL_ERROR) dn_status(DN_SESSION, "Runtime: %s", message);
}
#define ORT_CHECK(expr) do { if (!ort_check(b, (expr), err)) goto done; } while (0)
static int make_session(DnBackend *b, const wchar_t *path, int device, DnError *err) {
    OrtSessionOptions *options = NULL;
    const OrtDmlApi *dml = NULL;
    int ok = 0;
    ORT_CHECK(b->api->CreateSessionOptions(&options));
    ORT_CHECK(b->api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
    ORT_CHECK(b->api->SetIntraOpNumThreads(options, 4));
    ORT_CHECK(b->api->SetSessionExecutionMode(options, ORT_SEQUENTIAL));
    /* The bundled exports use these symbolic dimensions. Unknown names are harmless. */
    ORT_CHECK(b->api->AddFreeDimensionOverrideByName(options, "batch", 1));
    ORT_CHECK(b->api->AddFreeDimensionOverrideByName(options, "height", b->tile));
    ORT_CHECK(b->api->AddFreeDimensionOverrideByName(options, "width", b->tile));
    ORT_CHECK(b->api->AddFreeDimensionOverrideByName(options, "b", 1));
    ORT_CHECK(b->api->AddFreeDimensionOverrideByName(options, "h", b->tile));
    ORT_CHECK(b->api->AddFreeDimensionOverrideByName(options, "w", b->tile));
    if (device >= 0) {
        ORT_CHECK(b->api->DisableMemPattern(options));
        ORT_CHECK(b->api->GetExecutionProviderApi("DML", ORT_API_VERSION, (const void **)&dml));
        ORT_CHECK(dml->SessionOptionsAppendExecutionProvider_DML(options, device));
    }
    ORT_CHECK(b->api->CreateSession(b->env, path, options, &b->session));
    ok = 1;
done:
    if (options) b->api->ReleaseSessionOptions(options);
    if (!ok && b->session) { b->api->ReleaseSession(b->session); b->session = NULL; }
    return ok;
}
static int validate_type(DnBackend *b, int output, DnError *err) {
    OrtTypeInfo *info = NULL;
    const OrtTensorTypeAndShapeInfo *tensor = NULL;
    ONNXTensorElementDataType type;
    int64_t dims[4];
    size_t rank = 0;
    int ok = 0;
    if (output) { ORT_CHECK(b->api->SessionGetOutputTypeInfo(b->session, 0, &info)); }
    else { ORT_CHECK(b->api->SessionGetInputTypeInfo(b->session, 0, &info)); }
    ORT_CHECK(b->api->CastTypeInfoToTensorInfo(info, &tensor));
    if (!tensor) { dn_error(err, "Model input/output is not a tensor"); goto done; }
    ORT_CHECK(b->api->GetTensorElementType(tensor, &type));
    ORT_CHECK(b->api->GetDimensionsCount(tensor, &rank));
    if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || rank != 4) { dn_error(err, "Model must use float32 NCHW tensors"); goto done; }
    ORT_CHECK(b->api->GetDimensions(tensor, dims, 4));
    if ((dims[0] > 0 && dims[0] != 1) || (dims[1] > 0 && dims[1] != (output ? b->output_channels : b->input_channels)) ||
        (dims[2] > 0 && dims[2] != b->tile) || (dims[3] > 0 && dims[3] != b->tile)) {
        dn_error(err, "Model shape does not match the selected tile size/channel count"); goto done;
    }
    ok = 1;
done:
    if (info) b->api->ReleaseTypeInfo(info);
    return ok;
}
int dn_backend_open(DnBackend **out, const wchar_t *model, int input_channels, int output_channels, int tile, int cpu, int device, DnError *err) {
    DnBackend *b = calloc(1, sizeof(*b));
    DnGpu gpus[32];
    int n, i, ok = 0;
    size_t count;
    *out = NULL;
    if (!b) return dn_error(err, "Out of memory");
    b->api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    b->input_channels = input_channels; b->output_channels = output_channels; b->tile = tile;
    if (!b->api) { free(b); return dn_error(err, "Incompatible ONNX Runtime version"); }
    ORT_CHECK(b->api->CreateEnvWithCustomLogger(runtime_log, NULL, ORT_LOGGING_LEVEL_ERROR, "denoise", &b->env));
    if (!cpu) {
        n = gpu_inventory(gpus, 32, 0);
        for (i = 0; i < n; ++i) {
            if (device >= 0 && gpus[i].id != device) continue;
            if (dn_cancelled()) { dn_error(err, "Canceled"); goto done; }
            dn_status(DN_SESSION, "Loading model on DirectML GPU %d", gpus[i].id);
            if (make_session(b, model, gpus[i].id, err)) {
                b->gpu = 1;
                snprintf(b->name, sizeof(b->name), "DirectML GPU %d", gpus[i].id);
                break;
            }
            dn_status(DN_SESSION, "GPU unavailable: %s", err->message);
        }
        if (!b->session) dn_status(DN_SESSION, "CPU fallback: no usable GPU session");
    }
    if (!b->session) {
        dn_status(DN_SESSION, "Loading model on CPU");
        if (!make_session(b, model, -1, err)) goto done;
        strcpy_s(b->name, sizeof(b->name), "CPU");
    }
    ORT_CHECK(b->api->SessionGetInputCount(b->session, &count));
    if (count != 1) { dn_error(err, "Model must have exactly one input"); goto done; }
    ORT_CHECK(b->api->SessionGetOutputCount(b->session, &count));
    if (count != 1) { dn_error(err, "Model must have exactly one output"); goto done; }
    if (!validate_type(b, 0, err) || !validate_type(b, 1, err)) goto done;
    ORT_CHECK(b->api->GetAllocatorWithDefaultOptions(&b->allocator));
    ORT_CHECK(b->api->SessionGetInputName(b->session, 0, b->allocator, &b->input_name));
    ORT_CHECK(b->api->SessionGetOutputName(b->session, 0, b->allocator, &b->output_name));
    ORT_CHECK(b->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &b->memory));
    *out = b; ok = 1;
done:
    if (!ok) dn_backend_close(b);
    return ok;
}
int dn_backend_run(DnBackend *b, float *input, float *output, DnError *err) {
    OrtValue *in = NULL, *out = NULL;
    int64_t input_shape[] = {1, b->input_channels, b->tile, b->tile};
    int64_t output_shape[] = {1, b->output_channels, b->tile, b->tile};
    size_t input_bytes = (size_t)b->input_channels * b->tile * b->tile * sizeof(float);
    size_t output_bytes = (size_t)b->output_channels * b->tile * b->tile * sizeof(float);
    const OrtValue *inputs[1];
    const char *in_names[] = {b->input_name}, *out_names[] = {b->output_name};
    size_t i;
    int ok = 0;
    ORT_CHECK(b->api->CreateTensorWithDataAsOrtValue(b->memory, input, input_bytes, input_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in));
    ORT_CHECK(b->api->CreateTensorWithDataAsOrtValue(b->memory, output, output_bytes, output_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &out));
    inputs[0] = in;
    ORT_CHECK(b->api->Run(b->session, NULL, in_names, inputs, 1, out_names, 1, &out));
    for (i = 0; i < output_bytes / sizeof(float); ++i) {
        if (!isfinite(output[i])) { dn_error(err, "Model produced non-finite output values"); goto done; }
    }
    ok = 1;
done:
    if (in) b->api->ReleaseValue(in);
    if (out) b->api->ReleaseValue(out);
    return ok;
}
const char *dn_backend_name(const DnBackend *backend) { return backend->name; }
void dn_backend_close(DnBackend *b) {
    if (!b) return;
    if (b->allocator && b->input_name) b->allocator->Free(b->allocator, b->input_name);
    if (b->allocator && b->output_name) b->allocator->Free(b->allocator, b->output_name);
    if (b->memory) b->api->ReleaseMemoryInfo(b->memory);
    if (b->session) b->api->ReleaseSession(b->session);
    if (b->env) b->api->ReleaseEnv(b->env);
    free(b);
}
