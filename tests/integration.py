"""Development-only tests. The application itself is entirely native C.
Run: python tests/integration.py [--inference]
Inference downloads one ~77 MB public model through the C application.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'build' / 'bin'
OUT = ROOT / 'tests' / 'output' / str(time.time_ns())
OUT.mkdir(parents=True)
REPORT = []


def run(exe, *args, success=True):
    p = subprocess.run([str(BIN / exe), *map(str, args)], capture_output=True, text=True, errors='replace')
    if (p.returncode == 0) != success:
        raise AssertionError(f'{exe}: exit={p.returncode}\n{p.stdout}\n{p.stderr}')
    print(p.stdout, p.stderr, flush=True)
    REPORT.append({'command': [exe, *map(str, args)], 'returncode': p.returncode,
                   'stdout': p.stdout, 'stderr': p.stderr})
    return p


def bmp(name, w=67, h=65, bits=24):
    rng = random.Random(42)
    samples = bits // 8
    stride = (w * samples + 3) & ~3
    pixels = bytearray(stride * h)
    clean = []
    for y in range(h):
        for x in range(w):
            rgb = [90 + x * 0.35, 120 + y * 0.25, 145 + x * 0.2]
            clean.extend(rgb)
            for c in range(3):
                value = round(max(0, min(255, rgb[c] + rng.gauss(0, 25))))
                pixels[(h - 1 - y) * stride + x * samples + 2 - c] = value
            if samples == 4:
                pixels[(h - 1 - y) * stride + x * samples + 3] = (x * 3 + y * 7) % 256
        pixels[(h - 1 - y) * stride + w * samples:(h - y) * stride] = b'\xA7' * (stride - w * samples)
    trailer = b'UNKNOWN_METADATA_PRESERVE_EXACTLY_2026'
    header = struct.pack('<2sIHHI', b'BM', 54 + len(pixels) + len(trailer), 0, 0, 54)
    header += struct.pack('<IiiHHIIiiII', 40, w, h, 1, bits, 0, len(pixels), 3780, 3780, 0, 0)
    path = OUT / name
    path.write_bytes(header + pixels + trailer)
    return path, clean


def tiff(name, bits=16, channels=3, endian='<', w=19, h=17):
    # Multi-strip TIFF with a private metadata block and orientation tag retained.
    rows_per_strip = 7
    strip_count = math.ceil(h / rows_per_strip)
    specs = [(256,4,[w]),(257,4,[h]),(258,3,[bits]*channels),(259,3,[1]),
             (262,3,[1 if channels == 1 else 2]), (273,4,[0]*strip_count),
             (274,3,[6]), (277,3,[channels]),(278,4,[rows_per_strip]),
             (279,4,[min(rows_per_strip,h-i*rows_per_strip)*w*channels*(bits//8) for i in range(strip_count)]),
             (284,3,[1]),(65000,1,list(b'PRIVATE_ICC_EXIF_LIKE_DATA\0'))]
    if channels == 4:
        specs.append((338,3,[2]))
    specs.sort()
    extra_start = 8+2+12*len(specs)+4
    extra = bytearray()
    entries = []
    offset_entry = 0
    for tag, typ, vals in specs:
        fmt = {1:'B',3:'H',4:'I'}[typ]
        data = struct.pack(endian+fmt*len(vals), *vals)
        if len(data) <= 4:
            field = data.ljust(4,b'\0')
        else:
            field = struct.pack(endian+'I',extra_start+len(extra))
            if tag == 273: offset_entry = len(extra)
            extra.extend(data)
            if len(extra) % 2: extra.append(0)
        entries.append(struct.pack(endian+'HHI',tag,typ,len(vals))+field)
    pixel_start = extra_start+len(extra)
    offsets = [pixel_start + min(i*rows_per_strip,h)*w*channels*(bits//8) for i in range(strip_count)]
    extra[offset_entry:offset_entry+4*strip_count] = struct.pack(endian+'I'*strip_count,*offsets)
    rng = random.Random(7)
    values = [rng.randrange(2**bits) for _ in range(w*h*channels)]
    data = struct.pack(endian+('H' if bits==16 else 'B')*len(values), *values)
    header = (b'II' if endian=='<' else b'MM')+struct.pack(endian+'HIH',42,8,len(specs))
    path = OUT / name
    path.write_bytes(header+b''.join(entries)+struct.pack(endian+'I',0)+extra+data+b'RETAIN_TRAILER')
    return path, pixel_start, len(data)


def bmp_rgb(path):
    b = path.read_bytes()
    w,h = struct.unpack_from('<ii',b,18)
    samples = struct.unpack_from('<H',b,28)[0]//8
    stride = (w*samples+3)&~3
    return [b[54+(h-1-y)*stride+x*samples+2-c] for y in range(h) for x in range(w) for c in range(3)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--inference', action='store_true')
    args = parser.parse_args()
    for bits in [24,32]:
        src,_ = bmp(f'roundtrip{bits}.bmp',bits=bits)
        dst=src.with_stem(src.stem+'_out')
        run('io_roundtrip.exe',src,dst)
        assert src.read_bytes()==dst.read_bytes(), f'BMP{bits} changed unrelated data'
    for bits in [8,16]:
        for ch in [1,3,4]:
            for end in ['<','>']:
                src,_,_ = tiff(f'tiff{bits}_{ch}_{ord(end)}.tif',bits,ch,end)
                dst=src.with_stem(src.stem+'_out')
                run('io_roundtrip.exe',src,dst)
                assert src.read_bytes()==dst.read_bytes(), f'{src.name} changed on roundtrip'
    # Exact copy works on any format at amount=0, including opaque RAW payloads.
    opaque=OUT/'opaque.CR3'; opaque.write_bytes(bytes(range(256))*5)
    copy=OUT/'opaque_copy.CR3'
    run('denoise.exe',opaque,0,'--output',copy)
    assert opaque.read_bytes()==copy.read_bytes()
    run('denoise.exe',opaque,10,success=False)
    assert not opaque.with_stem('opaque_denoised_10').exists()
    run('denoise.exe',opaque,0,'--output',copy,success=False)
    run('denoise.exe',opaque,0,'--output',OUT/'wrong.png',success=False)
    jpeg=OUT/'test.jpg'; jpeg.write_bytes(b'\xff\xd8\xff\xe0'+b'x'*100)
    p=run('denoise.exe',jpeg,50,success=False)
    assert 'Cannot decode' in p.stderr
    # A truncated TIFF with an IFD offset outside the file is not an image.
    src=OUT/'truncated.tif'
    src.write_bytes(b'II'+struct.pack('<HI',42,0x7fffffff))
    run('denoise.exe',src,10,success=False)
    if args.inference:
        src,clean=bmp('noise.bmp')
        original=src.read_bytes()
        gpu=OUT/'noise_gpu.bmp'
        cpu=OUT/'noise_cpu.bmp'
        shared=['--model','25','--tile','128']
        run('denoise.exe',src,100,*shared,'--output',gpu)
        p=run('denoise.exe',src,100,*shared,'--device','999','--offline','--output',cpu)
        assert 'CPU fallback' in p.stderr and 'Backend: CPU' in p.stderr
        assert src.read_bytes()==original
        noisy=bmp_rgb(src); denoised=bmp_rgb(gpu); cpu_pixels=bmp_rgb(cpu)
        before=sum((x-y)**2 for x,y in zip(noisy,clean))/len(clean)
        after=sum((x-y)**2 for x,y in zip(denoised,clean))/len(clean)
        assert after < before*0.6, (before,after)
        assert max(abs(x-y) for x,y in zip(denoised,cpu_pixels)) <= 2
        for target in [gpu,cpu]:
            a,b=original,target.read_bytes()
            assert a[:54]==b[:54] and a[-36:]==b[-36:]
        # Actual 16-bit TIFF inference preserves every non-pixel byte and alpha.
        src,start,length=tiff('deep16.tif',16,4,'>')
        out=OUT/'deep16_result.tif'
        run('denoise.exe',src,50,*shared,'--offline','--output',out)
        a,b=src.read_bytes(),out.read_bytes()
        assert a[:start]==b[:start] and a[start+length:]==b[start+length:]
        for i in range(start,start+length,8): assert a[i+6:i+8]==b[i+6:i+8]
        # Exercise the actual one-channel network, automatic selection and Unicode/default output.
        src,start,length=tiff('Grigi\u00e0.TIF',16,1,'<')
        p=run('denoise.exe',src,50,'--tile','128','--cpu')
        assert 'scunet_gray_25' in p.stderr
        out=src.with_stem(src.stem+'_denoised_50')
        a,b=src.read_bytes(),out.read_bytes()
        assert a[:start]==b[:start] and a[start+length:]==b[start+length:]
        print(f'MSE: {before:.3f} -> {after:.3f}; gain {10*math.log10(before/after):.2f} dB')
        REPORT.append({'mse_before':before,'mse_after':after,'gain_db':10*math.log10(before/after)})
    (OUT/'report.json').write_text(json.dumps(REPORT,indent=2),encoding='utf-8')
    print('Integration tests OK:',OUT)


if __name__=='__main__':
    main()
