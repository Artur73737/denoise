"""RAW and codec integration; stdlib-only fixtures, no camera files bundled."""
import hashlib
import json
import struct
import zlib
from pathlib import Path
from integration import OUT, ROOT, REPORT, run, tiff


def ifd(path):
    data=path.read_bytes()
    endian='<' if data[:2]==b'II' else '>'
    start=struct.unpack_from(endian+'I',data,4)[0]
    count=struct.unpack_from(endian+'H',data,start)[0]
    tags={}
    widths={1:1,2:1,3:2,4:4,5:8,7:1,9:4,10:8}
    for i in range(count):
        at=start+2+i*12
        tag,typ,n=struct.unpack_from(endian+'HHI',data,at)
        size=widths[typ]*n
        loc=at+8 if size<=4 else struct.unpack_from(endian+'I',data,at+8)[0]
        tags[tag]=(typ,n,data[loc:loc+size],at)
    return data,endian,tags


def values(entry,endian):
    typ,n,data,_=entry
    return struct.unpack(endian+{1:'B',2:'c',3:'H',4:'I',7:'B',9:'i'}[typ]*n,data)


def make_dng():
    w,h=64,48
    specs=[(254,4,[0]),(256,4,[w]),(257,4,[h]),(258,3,[16]),(259,3,[1]),(262,3,[32803]),
           (271,2,b'Synthetic\0'),(272,2,b'Test CFA DNG\0'),(273,4,[0]),(274,3,[1]),(277,3,[1]),
           (278,4,[h]),(279,4,[w*h*2]),(284,3,[1]),(33421,3,[2,2]),(33422,1,[0,1,1,2]),
           (50706,1,[1,4,0,0]),(50707,1,[1,1,0,0]),(50708,2,b'Synthetic Test CFA DNG\0'),
           (50710,1,[0,1,2]),(50711,3,[1]),(50714,3,[64]),(50717,4,[4095]),
           (50719,3,[0,0]),(50720,3,[w,h]),
           (50721,10,[1,1,0,1,0,1,0,1,1,1,0,1,0,1,0,1,1,1]),
           (50728,5,[1,1,1,1,1,1]),(50778,3,[21]),(50829,4,[0,0,h,w])]
    specs.sort()
    base=8+2+12*len(specs)+4
    extra=bytearray();entries=[]
    pixel_entry=None
    for tag,typ,vals in specs:
        if typ==2: payload=vals;n=len(vals)
        else:
            fmt={1:'B',3:'H',4:'I',5:'I',10:'i'}[typ]
            payload=struct.pack('<'+fmt*len(vals),*vals)
            n=len(vals)//2 if typ in (5,10) else len(vals)
        if len(payload)>4:
            field=struct.pack('<I',base+len(extra));extra.extend(payload)
            if len(extra)%2:extra.append(0)
        else:field=payload.ljust(4,b'\0')
        if tag==273:pixel_entry=len(entries)
        entries.append(struct.pack('<HHI',tag,typ,n)+field)
    pixel_start=base+len(extra)
    entries[pixel_entry]=struct.pack('<HHII',273,4,1,pixel_start)
    pixels=[800+((x*37+y*71)%1900) for y in range(h) for x in range(w)]
    path=OUT/'synthetic.DNG'
    path.write_bytes(b'II'+struct.pack('<HIH',42,8,len(entries))+b''.join(entries)+b'\0'*4+extra+struct.pack('<'+'H'*len(pixels),*pixels))
    return path


def make_png(bits=16,gray=False):
    w,h=17,13
    channels=1 if gray else 4
    pixels=[(i*3571)%65536 if bits==16 else (i*71)%256 for i in range(w*h*channels)]
    def chunk(name,data):return struct.pack('>I',len(data))+name+data+struct.pack('>I',zlib.crc32(name+data))
    raw=b''.join(b'\0'+struct.pack('>'+('H' if bits==16 else 'B')*(w*channels),*pixels[y*w*channels:(y+1)*w*channels]) for y in range(h))
    path=OUT/f'input_{bits}_{channels}.png'
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,bits,0 if gray else 6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
    return path,pixels,w,h,channels


def main():
    p=run('denoise.exe','--formats')
    assert all(s in p.stdout for s in ['CR2','CR3','DNG','NEF','ARW'])
    src=make_dng();original=src.read_bytes()
    p=run('denoise.exe','--inspect',src)
    assert 'DNG: yes' in p.stdout
    # Actual RAW decoding, 16-bit development and TIFF writing, without inference.
    dst=OUT/'dng_rendered.tif'
    run('io_roundtrip.exe',src,dst)
    data,endian,tags=ifd(dst)
    assert values(tags[258],endian)==(16,16,16)
    assert values(tags[259],endian)==(1,)
    assert values(tags[256],endian)[0]>0 and values(tags[257],endian)[0]>0
    assert 34675 in tags, 'Expected sRGB ICC profile'
    assert src.read_bytes()==original
    for bits in [8,16]:
        for gray in [False,True]:
            src,pixels,w,h,channels=make_png(bits,gray)
            dst=OUT/(src.stem+'.tif')
            run('io_roundtrip.exe',src,dst)
            data,endian,tags=ifd(dst)
            offset=values(tags[273],endian)[0]
            nout=values(tags[277],endian)[0]
            actual=struct.unpack_from('<'+'H'*(w*h*nout),data,offset)
            expected=[]
            for i in range(w*h):
                if gray: expected.extend([pixels[i]]*3)
                else:expected.extend(pixels[i*4:i*4+4])
            if bits==8:expected=[x*257 for x in expected]
            assert tuple(expected)==actual,(bits,gray,'pixel/alpha precision changed')
    for channels in [1,3,4]:
        # Valid deflate TIFF strips are decoded without reducing bit depth.
        src,start,size=tiff(f'deflate{channels}.tif',16,channels)
        original=src.read_bytes()
        data,endian,tags=ifd(src)
        b=bytearray(data)
        offsets=values(tags[273],endian);counts=values(tags[279],endian)
        packed_offsets=[];packed_counts=[]
        for off,n in zip(offsets,counts):
            packed=zlib.compress(data[off:off+n]);packed_offsets.append(len(b));packed_counts.append(len(packed));b.extend(packed)
        for tag,vals in [(273,packed_offsets),(279,packed_counts)]:
            entry=tags[tag][3]
            loc=entry+8 if len(vals)==1 else struct.unpack_from('<I',b,entry+8)[0]
            struct.pack_into('<'+'I'*len(vals),b,loc,*vals)
        struct.pack_into('<H',b,tags[259][3]+8,8)
        src.write_bytes(b)
        dst=OUT/f'deflate{channels}_out.tif'
        run('io_roundtrip.exe',src,dst)
        result,endian,outtags=ifd(dst)
        loc=values(outtags[273],endian)[0]
        sample_values=struct.unpack('<'+'H'*(size//2),original[start:start+size])
        expected=[]
        for i in range(19*17):expected.extend([sample_values[i]]*3 if channels==1 else sample_values[i*channels:(i+1)*channels])
        actual=struct.unpack_from('<'+'H'*len(expected),result,loc)
        assert tuple(expected)==actual,f'TIFF16 deflate {channels} changed precision'
    jpeg=ROOT/'tests/fixtures/rgb.jpg'
    original_jpeg=jpeg.read_bytes()
    dst=OUT/'jpeg_denoised.tif'
    run('denoise.exe',jpeg,50,'--offline','--tile','128','--output',dst)
    data,endian,tags=ifd(dst)
    assert values(tags[258],endian)==(16,16,16) and values(tags[259],endian)==(1,)
    assert jpeg.read_bytes()==original_jpeg
    # No filename-extension gate: the actual DNG contents select the decoder.
    renamed=OUT/'camera.unusual_extension'
    renamed.write_bytes(make_dng().read_bytes())
    run('denoise.exe','--inspect',renamed)
    # Full model pipeline from a genuine CFA DNG, with output chosen automatically.
    src=make_dng()
    source_bytes=src.read_bytes()
    p=run('denoise.exe',src,50,'--offline','--tile','128')
    out=src.with_name(src.stem+'_denoised_50.tif')
    assert out.is_file() and 'LibRaw' in p.stderr
    assert src.read_bytes()==source_bytes
    (OUT/'formats-report.json').write_text(json.dumps(REPORT,indent=2))
    print('RAW/codec integration OK:',OUT)


if __name__=='__main__':main()
