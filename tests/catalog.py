"""Validate remote catalog boundaries and load a new ID without rebuilding."""
from pathlib import Path
import copy
import json
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
BIN=ROOT/'build/bin'
OUT=ROOT/'tests/output'/('catalog-'+str(time.time_ns()))
OUT.mkdir(parents=True)
BASE=json.loads((ROOT/'catalog/models.json').read_text())
checks=[]


def validate(name,document,valid=False):
    target=OUT/(name+'.json')
    target.write_text(document if isinstance(document,str) else json.dumps(document),encoding='utf-8')
    p=subprocess.run([str(BIN/'catalog_validate.exe'),str(target)],capture_output=True,text=True,timeout=10)
    assert (p.returncode==0)==valid,(name,p.returncode,p.stderr)
    checks.append(name)


validate('valid-snapshot',BASE,True)
for name,edit in [
 ('schema',lambda d:d.update(schema=999)),
 ('adapter',lambda d:d['models'][0].update(adapter='unimplemented')),
 ('channels',lambda d:d['models'][0].update(input_channels=4)),
 ('traversal',lambda d:d['models'][0]['files'][0].update(name='../model.onnx')),
 ('device-name',lambda d:d['models'][0]['files'][0].update(name='CON.onnx')),
 ('url-http',lambda d:d['models'][0]['files'][0].update(url='http://huggingface.co/model')),
 ('url-host',lambda d:d['models'][0]['files'][0].update(url='https://huggingface.co.evil.example/model')),
 ('hash',lambda d:d['models'][0]['files'][0].update(sha256='bad')),
 ('negative-bytes',lambda d:d['models'][0]['files'][0].update(bytes=-1)),
 ('fractional-bytes',lambda d:d['models'][0]['files'][0].update(bytes=1.5)),
 ('oversize-artifact',lambda d:d['models'][0]['files'][0].update(bytes=10**20)),
 ('duplicate-id',lambda d:d['models'][1].update(id=d['models'][0]['id'])),
 ('filename-collision',lambda d:d['models'][1]['files'][0].update(name=d['models'][0]['files'][0]['name'])),
 ('fixed-tile',lambda d:d['models'][0].update(fixed_tile=17)),
 ('missing-core',lambda d:d['models'].pop(0)),
]:
    value=copy.deepcopy(BASE);edit(value);validate(name,value)
validate('trailing-garbage',json.dumps(BASE)+'garbage')
validate('duplicate-key',json.dumps(BASE).replace('"schema": 1','"schema": 1, "schema": 2',1))
validate('embedded-nul',json.dumps(BASE)+'\0')
validate('oversize-catalog',' '*524289)

# A genuinely new catalog row works from the executable-adjacent cache.
portable=OUT/'portable';portable.mkdir();cache=portable/'models';cache.mkdir()
for name in ['denoise.exe','onnxruntime.dll','DirectML.dll','libraw.dll']:shutil.copyfile(BIN/name,portable/name)
dynamic=copy.deepcopy(BASE)
model=copy.deepcopy(next(m for m in BASE['models'] if m['id']=='restormer_real'))
model['id']='new_remote_catalog_model';model['files'][0]['name']='new_remote_catalog_model.onnx'
dynamic['models'].append(model)
validate('dynamic-catalog',dynamic,True)
(cache/'catalog.json').write_text(json.dumps(dynamic))
shutil.copyfile(BIN/'models/restormer_real_denoise.onnx',cache/'new_remote_catalog_model.onnx')
p=subprocess.run([str(portable/'denoise.exe'),'model','list','--offline'],cwd=OUT,capture_output=True,text=True,timeout=30)
assert p.returncode==0 and f'Cached catalog | {len(dynamic["models"])} models' in p.stdout and 'new_remote_catalog_model' in p.stdout
result=OUT/'dynamic_result.tif'
p=subprocess.run([str(portable/'denoise.exe'),str(ROOT/'tests/fixtures/rgb.jpg'),'50','--model',model['id'],'--offline','--tile','128','--no-tui','--output',str(result)],cwd=OUT,capture_output=True,text=True,timeout=60)
assert p.returncode==0,p.stderr
assert 'Model: new_remote_catalog_model' in p.stderr and result.is_file()
checks.append('new-id-loaded-and-run-without-rebuild')
# Corrupt cache is ignored in favor of the embedded validated snapshot.
(cache/'catalog.json').write_text('{broken')
p=subprocess.run([str(portable/'denoise.exe'),'model','list','--offline'],cwd=OUT,capture_output=True,text=True,timeout=30)
assert p.returncode==0 and f'Bundled catalog | {len(BASE["models"])} models' in p.stdout
assert 'new_remote_catalog_model' not in p.stdout
checks.append('invalid-cache-fallback')
(OUT/'result.json').write_text(json.dumps({'passed':True,'checks':checks,'runtime_stderr':p.stderr},indent=2))
print('Catalog validation and dynamic model loading tests passed:',OUT)
