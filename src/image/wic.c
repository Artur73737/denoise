#include "denoise.h"
#include <wincodec.h>
#include <propvarutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void text_metadata(IWICMetadataQueryReader *r, unsigned tag, char *out, size_t cap) {
    wchar_t key[64];
    PROPVARIANT value;
    HRESULT hr;
    PropVariantInit(&value);
    swprintf_s(key, 64, L"/app1/ifd/{ushort=%u}", tag);
    hr = IWICMetadataQueryReader_GetMetadataByName(r, key, &value);
    if (FAILED(hr)) { swprintf_s(key, 64, L"/ifd/{ushort=%u}", tag); hr = IWICMetadataQueryReader_GetMetadataByName(r, key, &value); }
    if (SUCCEEDED(hr)) {
        if (value.vt == VT_LPSTR && value.pszVal) strncpy_s(out, cap, value.pszVal, _TRUNCATE);
        else if (value.vt == VT_LPWSTR && value.pwszVal) {
            if (!WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, out, (int)cap, NULL, NULL)) out[0] = 0;
        }
    }
    PropVariantClear(&value);
}
static void metadata(IWICImagingFactory *factory, IWICBitmapFrameDecode *frame, DnImage *image) {
    IWICMetadataQueryReader *reader = NULL;
    IWICColorContext *contexts[16] = {0};
    UINT count = 0, actual = 0, i;
    image->metadata.orientation = 1;
    (void)IWICBitmapFrameDecode_GetResolution(frame, &image->metadata.xdpi, &image->metadata.ydpi);
    if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &reader))) {
        PROPVARIANT value;
        HRESULT hr;
        text_metadata(reader, 271, image->metadata.make, sizeof(image->metadata.make));
        text_metadata(reader, 272, image->metadata.model, sizeof(image->metadata.model));
        text_metadata(reader, 315, image->metadata.artist, sizeof(image->metadata.artist));
        text_metadata(reader, 306, image->metadata.datetime, sizeof(image->metadata.datetime));
        PropVariantInit(&value);
        hr = IWICMetadataQueryReader_GetMetadataByName(reader, L"/app1/ifd/{ushort=274}", &value);
        if (FAILED(hr)) hr = IWICMetadataQueryReader_GetMetadataByName(reader, L"/ifd/{ushort=274}", &value);
        if (SUCCEEDED(hr) && value.vt == VT_UI2 && value.uiVal >= 1 && value.uiVal <= 8) image->metadata.orientation = value.uiVal;
        PropVariantClear(&value);
        IWICMetadataQueryReader_Release(reader);
    }
    if (SUCCEEDED(IWICBitmapFrameDecode_GetColorContexts(frame, 0, NULL, &count)) && count && count <= 16) {
        for (i = 0; i < count; ++i) if (FAILED(IWICImagingFactory_CreateColorContext(factory, &contexts[i]))) break;
        if (i == count && SUCCEEDED(IWICBitmapFrameDecode_GetColorContexts(frame, count, contexts, &actual))) {
            for (i = 0; i < actual && i < count; ++i) {
                UINT bytes = 0, read = 0;
                if (SUCCEEDED(IWICColorContext_GetProfileBytes(contexts[i], 0, NULL, &bytes)) && bytes && bytes <= 16 * 1024 * 1024) {
                    image->metadata.icc = malloc(bytes);
                    if (image->metadata.icc && SUCCEEDED(IWICColorContext_GetProfileBytes(contexts[i], bytes, image->metadata.icc, &read)) && read == bytes) {
                        image->metadata.icc_size = bytes; break;
                    }
                    free(image->metadata.icc); image->metadata.icc = NULL;
                }
            }
        }
        for (i = 0; i < count; ++i) if (contexts[i]) IWICColorContext_Release(contexts[i]);
    }
}
int dn_wic_load(const wchar_t *path, DnImage *image, DnError *err) {
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    IWICBitmapSource *source;
    WICPixelFormatGUID format;
    WICPixelFormatGUID target = GUID_WICPixelFormat64bppRGBA;
    uint16_t *row = NULL;
    UINT x, y, frames = 0;
    HRESULT hr;
    int ok = 0, gray16 = 0;
    memset(image, 0, sizeof(*image));
    image->source = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (image->source == INVALID_HANDLE_VALUE) return dn_error(err, "Apertura immagine fallita: %lu", GetLastError());
#define CHECK(expr) do { hr = (expr); if (FAILED(hr)) { dn_hresult(err, "Windows image codec", hr); goto done; } } while (0)
    CHECK(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory));
    CHECK(IWICImagingFactory_CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder));
    CHECK(IWICBitmapDecoder_GetFrameCount(decoder, &frames));
    if (frames > 1) dn_status(DN_DECODE, "Input contains %u frames/pages; processing the first image", frames);
    CHECK(IWICBitmapDecoder_GetFrame(decoder, 0, &frame));
    CHECK(IWICBitmapFrameDecode_GetSize(frame, &image->width, &image->height));
    if (!image->width || !image->height || image->width > 100000 || image->height > 100000 ||
        (uint64_t)image->width * image->height > 200000000) { dn_error(err, "Image dimensions exceed the supported limit"); goto done; }
    CHECK(IWICBitmapFrameDecode_GetPixelFormat(frame, &format));
    if (IsEqualGUID(&format, &GUID_WICPixelFormat32bppCMYK) || IsEqualGUID(&format, &GUID_WICPixelFormat64bppCMYK)) {
        dn_error(err, "CMYK needs a dedicated color-management pipeline; cannot apply a CMYK profile to RGB pixels"); goto done;
    }
    gray16 = IsEqualGUID(&format, &GUID_WICPixelFormat16bppGray);
    image->has_alpha = IsEqualGUID(&format, &GUID_WICPixelFormat32bppRGBA) || IsEqualGUID(&format, &GUID_WICPixelFormat32bppBGRA) ||
        IsEqualGUID(&format, &GUID_WICPixelFormat32bppPBGRA) || IsEqualGUID(&format, &GUID_WICPixelFormat64bppRGBA) ||
        IsEqualGUID(&format, &GUID_WICPixelFormat64bppPRGBA) || IsEqualGUID(&format, &GUID_WICPixelFormat32bppPRGBA);
    source = (IWICBitmapSource *)frame;
    if (!gray16 && !IsEqualGUID(&format, &target)) {
        CHECK(IWICImagingFactory_CreateFormatConverter(factory, &converter));
        CHECK(IWICFormatConverter_Initialize(converter, source, &target, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom));
        source = (IWICBitmapSource *)converter;
    }
    row = malloc((size_t)image->width * (gray16 ? 2 : 8));
    image->rgba = malloc((size_t)image->width * image->height * 4 * sizeof(float));
    if (!row || !image->rgba) { dn_error(err, "Out of memory"); goto done; }
    image->grayscale = 1;
    for (y = 0; y < image->height; ++y) {
        if (dn_cancelled()) { dn_error(err, "Canceled"); goto done; }
        WICRect rect = {0, (INT)y, (INT)image->width, 1};
        UINT stride = image->width * (gray16 ? 2 : 8);
        CHECK(IWICBitmapSource_CopyPixels(source, &rect, stride, stride, (BYTE *)row));
        for (x = 0; x < image->width; ++x) {
            size_t dst = ((size_t)y * image->width + x) * 4;
            int c;
            for (c = 0; c < 3; ++c) image->rgba[dst + c] = row[gray16 ? x : x * 4 + c] / 65535.0f;
            image->rgba[dst + 3] = gray16 ? 1.0f : row[x * 4 + 3] / 65535.0f;
            if (image->rgba[dst + 3] != 1.0f) image->has_alpha = 1;
            if (image->rgba[dst] != image->rgba[dst + 1] || image->rgba[dst] != image->rgba[dst + 2]) image->grayscale = 0;
        }
    }
    metadata(factory, frame, image);
    strcpy_s(image->decoder, sizeof(image->decoder), "Windows Imaging Component");
    ok = 1;
done:
    free(row);
    if (converter) IWICFormatConverter_Release(converter);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (factory) IWICImagingFactory_Release(factory);
    if (!ok) dn_image_free(image);
    return ok;
}
