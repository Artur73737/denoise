"""Model selection, fixed cache location, and strength-100 regressions."""
import json
from pathlib import Path
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
EXE=ROOT/'build/bin/denoise.exe'
OUT=ROOT/'tests/output'/('models-'+str(time.time_ns()))
OUT.mkdir(parents=True)
REPORT=[]


def call(*args,success=True):
    p=subprocess.run([str(EXE),*map(str,args)],cwd=OUT,capture_output=True,text=True,errors='replace',timeout=90)
    REPORT.append({'args':list(map(str,args)),'code':p.returncode,'stdout':p.stdout,'stderr':p.stderr})
    assert (p.returncode==0)==success,p.stdout+'\n'+p.stderr
    return p


source=ROOT/'tests/fixtures/rgb.jpg'
for i,model in enumerate(['auto','scunet_color_50','SCUNET_COLOR_50.onnx']):
    target=OUT/f'heavy{i}.tif'
    p=call(source,100,'--model',model,'--offline','--no-tui','--tile',128,'--output',target)
    assert 'Model: scunet_color_50 | Strength: 100' in p.stderr
    assert p.stdout == str(target)+'\n',repr(p.stdout)
    assert 'Completed in' not in p.stderr and 'Saved:' not in p.stderr
    if i:assert target.read_bytes()==(OUT/'heavy0.tif').read_bytes()
p=call(source,100,'--model','scunet_color_25','--offline','--no-tui','--tile',128,'--output',OUT/'pinned25.tif')
assert 'Model: scunet_color_25 | Strength: 100' in p.stderr
p=call(source,100,'--model','scunet_gray_25','--offline','--no-tui',success=False)
assert 'requires a grayscale image' in p.stderr
call(source,100,'--model','missing_model',success=False)
call(source,101,success=False)
call(source,9999999999999999999999999,success=False)
p=call(source,100,'--models',OUT,success=False)
assert 'directory is fixed' in p.stderr
p=call('model','list')
assert 'scunet_color_50' in p.stdout and str(EXE.parent/'models') in p.stdout
p=call('model',success=False)
assert 'interactive Windows terminal' in p.stderr
assert not (OUT/'models').exists(), 'Cache was created in the working directory'
(OUT/'result.json').write_text(json.dumps(REPORT,indent=2))
print('Model IDs, aliases, strength 100 and executable-relative cache tests passed:',OUT)
