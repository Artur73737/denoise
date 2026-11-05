"""Install/uninstall the built MSI in an isolated directory and test the payload.

Requires an interactive Windows user account. Refuses to touch an existing
Photo Denoise MSI registration. Logs and results stay under tests/output.
"""
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import winreg

ROOT = Path(__file__).resolve().parents[1]
MSI = ROOT / 'dist/releases/PhotoDenoise-1.3.1-x64.msi'
OUT = ROOT / 'tests/output' / ('installer-' + str(time.time_ns()))
APP = OUT / 'installed'
OUT.mkdir(parents=True)
msi = C.WinDLL('msi')
msi.MsiEnumRelatedProductsW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, W.LPWSTR]
product = C.create_unicode_buffer(39)
assert msi.MsiEnumRelatedProductsW('{DAD346B7-431F-4244-B5C0-66D3B4E736B6}', 0, 0, product) == 259, 'Existing MSI installation: refusing to modify it'
registry = r'Software\Artur73737\PhotoDenoise'
try:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry):
        raise AssertionError('Existing application registry: refusing to modify it')
except FileNotFoundError:
    pass


def user_path():
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, 'Environment') as key:
        try:
            return winreg.QueryValueEx(key, 'Path')
        except FileNotFoundError:
            return None


def installer(*args, log):
    p = subprocess.run(['msiexec.exe', *args, '/qn', '/norestart', '/l*v', str(OUT / log)], timeout=180)
    assert p.returncode == 0, f'Windows Installer exit {p.returncode}; see {OUT / log}'


def run(*args, success=True):
    p = subprocess.run([str(APP / 'denoise.exe'), *map(str, args)], cwd=OUT, capture_output=True, encoding='utf-8', timeout=120)
    assert (p.returncode == 0) == success, p.stdout + '\n' + p.stderr
    return p


before = user_path()
(OUT / 'path-before.json').write_text(json.dumps(before), encoding='utf-8')
installed = False
report = []
try:
    installer('/i', str(MSI), f'INSTALLFOLDER={APP}', log='install.log')
    installed = True
    assert msi.MsiEnumRelatedProductsW('{DAD346B7-431F-4244-B5C0-66D3B4E736B6}', 0, 0, product) == 0
    for source in (ROOT / 'build/installer/payload').rglob('*'):
        if source.is_file():
            target = APP / source.relative_to(ROOT / 'build/installer/payload')
            assert target.is_file(), str(target)
            assert hashlib.sha256(source.read_bytes()).digest() == hashlib.sha256(target.read_bytes()).digest(), str(target)
    report.append('All installed files match the validated build payload')
    assert str(APP).lower() in user_path()[0].lower()
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry) as key:
        assert Path(winreg.QueryValueEx(key, 'InstallPath')[0]) == APP
    menu = Path(os.environ['APPDATA']) / 'Microsoft/Windows/Start Menu/Programs/Photo Denoise'
    assert (menu / 'Photo Denoise.lnk').exists() and (menu / 'Photo Denoise Models.lnk').exists()
    report.append('Per-user registration, PATH and Start menu shortcuts are installed')
    assert '1.3.1' in run('--help').stdout
    catalog = run('model', 'list', '--offline')
    assert '13 models' in catalog.stdout and str(APP / 'models') in catalog.stdout
    source = ROOT / 'tests/fixtures/rgb.jpg'
    original = source.read_bytes()
    exact = OUT / 'exact.jpg'
    assert run(source, 0, '--output', exact).stdout == str(exact) + '\n'
    assert exact.read_bytes() == original
    report.append('Installed executable runs with bundled DLLs; strength 0 is byte-identical and stdout is one path')
    # Use complete local model files for the install test, without a network dependency.
    for name in ['scunet_color_50.onnx', 'scunet_color_50.onnx.data']:
        shutil.copyfile(ROOT / 'build/bin/models' / name, APP / 'models' / name)
    output = OUT / 'processed.tif'
    p = run(source, 100, '--offline', '--model', 'scunet_color_50', '--output', output)
    assert p.stdout == str(output) + '\n' and output.is_file()
    assert 'Completed in' not in p.stderr and 'Saved:' not in p.stderr
    assert source.read_bytes() == original
    report.append('Installed binary denoises on GPU, exits automatically and preserves the original')
    failed = run(source, 100, '--offline', '--output', output, success=False)
    assert failed.stdout == '' and 'Error:' in failed.stderr
    sentinel = APP / 'models/user-model.keep'
    sentinel.write_bytes(b'Uninstall must preserve user model data')
finally:
    if installed:
        installer('/x', product.value, log='uninstall.log')

assert user_path() == before, 'Uninstall did not restore the user PATH'
assert not (APP / 'denoise.exe').exists()
assert not menu.exists()
assert sentinel.read_bytes() == b'Uninstall must preserve user model data'
assert (APP / 'models/scunet_color_50.onnx.data').exists()
assert not (APP / 'models/catalog.json').exists()
assert msi.MsiEnumRelatedProductsW('{DAD346B7-431F-4244-B5C0-66D3B4E736B6}', 0, 0, product) == 259
report.append('Uninstall removes managed files/registration/shortcuts/PATH and preserves user models')
(OUT / 'result.json').write_text(json.dumps({'passed': True, 'checks': report}, indent=2), encoding='utf-8')
print('MSI install, execution and uninstall checks passed:', OUT)
