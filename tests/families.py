"""Opt-in smoke tests for the additional downloaded model architectures.
Install the listed models with `denoise.exe model` before running this script.
"""
from pathlib import Path
import json
import struct
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
EXE=ROOT/'build/bin/denoise.exe'
OUT=ROOT/'tests/output'/('families-'+str(time.time_ns()))
OUT.mkdir(parents=True)
REPORT=[]


def run(model,cpu=False,tile=256):
    target=OUT/(model+('_cpu' if cpu else '_gpu')+'.tif')
    args=[str(EXE),str(ROOT/'tests/fixtures/rgb.jpg'),'80','--model',model,'--offline','--no-tui','--tile',str(tile),'--output',str(target)]
    if cpu:args.append('--cpu')
    p=subprocess.run(args,capture_output=True,text=True,timeout=180)
    REPORT.append({'model':model,'cpu':cpu,'code':p.returncode,'stderr':p.stderr})
    assert p.returncode==0,p.stderr
    data=target.read_bytes();start=struct.unpack_from('<I',data,4)[0]
    count=struct.unpack_from('<H',data,start)[0];tags={}
    for i in range(count):
        at=start+2+i*12;tag,typ,n=struct.unpack_from('<HHI',data,at)
        tags[tag]=(typ,n,struct.unpack_from('<I',data,at+8)[0])
    assert tags[256][2]==11 and tags[257][2]==7
    assert tags[259][2]==1
    offset=tags[273][2]
    pixels=struct.unpack_from('<'+'H'*(11*7*3),data,offset)
    assert min(pixels)<max(pixels) and max(pixels)>1000,'Empty/invalid output'
    print('Passed:',model,'CPU' if cpu else 'GPU',flush=True)
    return pixels,p.stderr


for model in ['nafnet_sidd_w64','nafnet_sidd_w32','restormer_real','swinir_color_25','drunet_color','scunet_color_real_gan']:
    pixels,log=run(model)
    assert f'Model: {model}' in log
    if model=='swinir_color_25':assert 'requires fixed 128x128 tiles' in log
    if model in ['drunet_color','nafnet_sidd_w64']:
        cpu,_=run(model,True)
        assert max(abs(a-b) for a,b in zip(pixels,cpu))<1500,'Unexpected CPU/GPU divergence'
(OUT/'result.json').write_text(json.dumps(REPORT,indent=2))
print('Additional model architecture tests passed:',OUT)
