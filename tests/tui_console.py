"""Exercise the real Win32 TUI in a hidden console, not a mocked renderer."""
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import subprocess
import shutil
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/output'/('tui-'+str(time.time_ns()))


def child(output):
    k=C.WinDLL('kernel32',use_last_error=True)
    class Coord(C.Structure):_fields_=[('X',W.SHORT),('Y',W.SHORT)]
    class Rect(C.Structure):_fields_=[('Left',W.SHORT),('Top',W.SHORT),('Right',W.SHORT),('Bottom',W.SHORT)]
    class Info(C.Structure):_fields_=[('size',Coord),('cursor',Coord),('attributes',W.WORD),('window',Rect),('maximum',Coord)]
    class Char(C.Union):_fields_=[('unicode',W.WCHAR),('ascii',C.c_char)]
    class Key(C.Structure):_fields_=[('down',W.BOOL),('repeat',W.WORD),('vk',W.WORD),('scan',W.WORD),('char',Char),('control',W.DWORD)]
    class Event(C.Union):_fields_=[('key',Key),('padding',C.c_byte*16)]
    class Record(C.Structure):_fields_=[('kind',W.WORD),('event',Event)]
    class Startup(C.Structure):
        _fields_=[('cb',W.DWORD),('reserved',W.LPWSTR),('desktop',W.LPWSTR),('title',W.LPWSTR),
                  ('x',W.DWORD),('y',W.DWORD),('width',W.DWORD),('height',W.DWORD),('columns',W.DWORD),('rows',W.DWORD),
                  ('fill',W.DWORD),('flags',W.DWORD),('show',W.WORD),('reserved2size',W.WORD),('reserved2',C.POINTER(C.c_byte)),
                  ('stdin',W.HANDLE),('stdout',W.HANDLE),('stderr',W.HANDLE)]
    class Process(C.Structure):_fields_=[('process',W.HANDLE),('thread',W.HANDLE),('pid',W.DWORD),('tid',W.DWORD)]
    k.CreateFileW.argtypes=[W.LPCWSTR,W.DWORD,W.DWORD,C.c_void_p,W.DWORD,W.DWORD,W.HANDLE];k.CreateFileW.restype=W.HANDLE
    k.CloseHandle.argtypes=[W.HANDLE]
    k.GetConsoleMode.argtypes=[W.HANDLE,C.POINTER(W.DWORD)]
    k.GetConsoleScreenBufferInfo.argtypes=[W.HANDLE,C.POINTER(Info)]
    k.SetConsoleScreenBufferSize.argtypes=[W.HANDLE,Coord]
    k.SetConsoleWindowInfo.argtypes=[W.HANDLE,W.BOOL,C.POINTER(Rect)]
    k.SetConsoleCursorPosition.argtypes=[W.HANDLE,Coord]
    k.ReadConsoleOutputCharacterW.argtypes=[W.HANDLE,W.LPWSTR,W.DWORD,Coord,C.POINTER(W.DWORD)]
    k.WriteConsoleOutputCharacterW.argtypes=[W.HANDLE,W.LPCWSTR,W.DWORD,Coord,C.POINTER(W.DWORD)]
    k.WriteConsoleInputW.argtypes=[W.HANDLE,C.POINTER(Record),W.DWORD,C.POINTER(W.DWORD)]
    k.SetHandleInformation.argtypes=[W.HANDLE,W.DWORD,W.DWORD]
    k.CreateProcessW.argtypes=[W.LPCWSTR,W.LPWSTR,C.c_void_p,C.c_void_p,W.BOOL,W.DWORD,C.c_void_p,W.LPCWSTR,C.POINTER(Startup),C.POINTER(Process)]
    k.WaitForSingleObject.argtypes=[W.HANDLE,W.DWORD]
    k.TerminateProcess.argtypes=[W.HANDLE,W.UINT]
    k.GetExitCodeProcess.argtypes=[W.HANDLE,C.POINTER(W.DWORD)]

    def handle(name):
        h=k.CreateFileW(name,0xc0000000,3,None,3,0,None)
        assert h and h!=C.c_void_p(-1).value,C.get_last_error()
        return h
    input_handle=handle('CONIN$');original=handle('CONOUT$')
    original_mode=W.DWORD();assert k.GetConsoleMode(input_handle,C.byref(original_mode))
    for h in [input_handle,original]:assert k.SetHandleInformation(h,1,1)
    marker='RESTORED ORIGINAL TERMINAL'
    written=W.DWORD();assert k.WriteConsoleOutputCharacterW(original,marker,len(marker),Coord(0,0),C.byref(written))
    assert k.SetConsoleCursorPosition(original,Coord(0,1))

    def resize(h,w,hgt):
        info=Info();assert k.GetConsoleScreenBufferInfo(h,C.byref(info))
        small=Rect(0,0,min(w,info.window.Right-info.window.Left+1)-1,min(hgt,info.window.Bottom-info.window.Top+1)-1)
        assert k.SetConsoleWindowInfo(h,True,C.byref(small)),C.get_last_error()
        assert k.SetConsoleScreenBufferSize(h,Coord(w,hgt)),C.get_last_error()
        rect=Rect(0,0,w-1,hgt-1);assert k.SetConsoleWindowInfo(h,True,C.byref(rect)),C.get_last_error()
    resize(original,100,32)
    start=Startup();start.cb=C.sizeof(start);start.flags=0x100|1;start.show=0
    start.stdin=input_handle;start.stdout=original;start.stderr=original
    process=Process()
    command=C.create_unicode_buffer(subprocess.list2cmdline([str(ROOT/'build/bin/denoise.exe'),'model','.']))
    assert k.CreateProcessW(None,command,None,None,True,0,None,str(ROOT),C.byref(start),C.byref(process)),C.get_last_error()

    def snapshot():
        h=handle('CONOUT$')
        try:
            info=Info();assert k.GetConsoleScreenBufferInfo(h,C.byref(info))
            width=info.window.Right-info.window.Left+1;height=info.window.Bottom-info.window.Top+1
            lines=[]
            for y in range(height):
                line=C.create_unicode_buffer(width+1);read=W.DWORD()
                assert k.ReadConsoleOutputCharacterW(h,line,width,Coord(info.window.Left,info.window.Top+y),C.byref(read))
                lines.append(line.value)
            return '\n'.join(lines)
        finally:k.CloseHandle(h)
    def wait_text(needle,timeout=15):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            view=snapshot()
            if needle in view:return view
            time.sleep(.1)
        raise AssertionError('Did not see '+needle+'\n'+snapshot())
    def press(vk,char='\0'):
        records=(Record*2)()
        for i in range(2):
            records[i].kind=1;records[i].event.key=Key(i==0,1,vk,0,Char(unicode=char),0)
        count=W.DWORD();assert k.WriteConsoleInputW(input_handle,records,2,C.byref(count)) and count.value==2
    try:
        first=wait_text('MODEL ID',45);assert 'scunet_color_50' in first and 'MiB' in first and 'STATUS' in first
        assert 'Online catalog' in first
        (output/'library.txt').write_text(first,encoding='utf-8')
        press(0x28);press(0x28)
        heavy=wait_text('Color / Heavy noise');assert '--model scunet_color_50' in heavy
        (output/'selected.txt').write_text(heavy,encoding='utf-8')
        h=handle('CONOUT$');resize(h,100,24);k.CloseHandle(h)
        press(0x23)  # End: scroll beyond the first seven visible entries.
        scrolled=wait_text('DRUNet / Noise-level control')
        assert '--model drunet_color' in scrolled
        (output/'scrolled.txt').write_text(scrolled,encoding='utf-8')
        press(0x24);press(0x28);press(0x28);wait_text('Color / Heavy noise')
        h=handle('CONOUT$');resize(h,60,18);k.CloseHandle(h)
        wait_text('Resize the terminal')
        h=handle('CONOUT$');resize(h,100,32);k.CloseHandle(h)
        wait_text('Color / Heavy noise')
        cached=ROOT/'build/bin/models/catalog.json'
        before=cached.stat().st_mtime_ns
        press(ord('R'),'r')
        deadline=time.monotonic()+45
        while cached.stat().st_mtime_ns==before and time.monotonic()<deadline:time.sleep(.1)
        assert cached.stat().st_mtime_ns>before,'R did not fetch/replace the online catalog cache'
        wait_text('MODEL ID',45)
        press(0x0d,'\r')
        completed=wait_text('Completed',20);assert 'Model installed and SHA-256 verified' in completed
        (output/'installed.txt').write_text(completed,encoding='utf-8')
        press(0x0d,'\r');wait_text('MODEL ID')
        press(0x24)  # Home: first model, deliberately not installed in this test cache.
        wait_text('Color / Light noise')
        if not (ROOT/'build/bin/models/scunet_color_15.onnx.data').exists():
            press(0x0d,'\r')
            transfer=wait_text('Downloading model',30)
            assert 'Connecting to model server' in transfer or 'Downloading model file' in transfer
            (output/'download.txt').write_text(transfer,encoding='utf-8')
            press(0x1b)
            canceled=wait_text('Canceled',60)
            assert 'partial' in canceled.lower()
            (output/'canceled.txt').write_text(canceled,encoding='utf-8')
            press(0x0d,'\r');wait_text('MODEL ID')
        press(ord('Q'),'q')
        assert k.WaitForSingleObject(process.process,10000)==0
        result=W.DWORD();assert k.GetExitCodeProcess(process.process,C.byref(result)) and result.value==0
        restored=snapshot();assert marker in restored
        mode=W.DWORD();assert k.GetConsoleMode(input_handle,C.byref(mode)) and mode.value==original_mode.value
        k.CloseHandle(process.thread);k.CloseHandle(process.process)
        process=Process()
        command=C.create_unicode_buffer(subprocess.list2cmdline([str(ROOT/'build/bin/denoise.exe')]))
        assert k.CreateProcessW(None,command,None,None,True,0,None,str(ROOT),C.byref(start),C.byref(process)),C.get_last_error()
        form=wait_text('IMAGE PATH')
        (output/'form.txt').write_text(form,encoding='utf-8')
        source=output/'TUI input \u00e0.jpg';shutil.copyfile(ROOT/'tests/fixtures/rgb.jpg',source)
        original_bytes=source.read_bytes()
        for char in str(source):press(0,char)
        press(0x09,'\t');press(0x24);press(0x2e);press(0x2e)
        for char in '100':press(0,char)
        press(0x0d,'\r')
        # No completion keystroke: processing must restore the terminal and exit.
        assert k.WaitForSingleObject(process.process,45000)==0,'Denoise did not exit automatically'
        job_done=snapshot()
        (output/'image_done.txt').write_text(job_done,encoding='utf-8')
        assert 'Saved:' not in job_done and 'Completed in' not in job_done
        assert source.stem+'_denoised_100.tif' in job_done
        assert k.GetExitCodeProcess(process.process,C.byref(result)) and result.value==0
        assert source.with_stem(source.stem+'_denoised_100').with_suffix('.tif').exists()
        assert source.read_bytes()==original_bytes
        assert marker in snapshot()
        assert k.GetConsoleMode(input_handle,C.byref(mode)) and mode.value==original_mode.value
        (output/'result.json').write_text(json.dumps({'passed':True,'checks':['real console buffer rendering','model . command','online catalog fetch on entry and R','catalog scrolling beyond seven entries','keyboard selection','resize down/up','installed-model verification','download view and cancellation when missing','back navigation','quit','interactive image form','Unicode path input','strength 100 from form','worker-thread GPU processing','original terminal and input mode restored']}))
    finally:
        if k.WaitForSingleObject(process.process,0)!=0:k.TerminateProcess(process.process,1)
        for h in [process.thread,process.process,input_handle,original]:k.CloseHandle(h)


if __name__=='__main__':
    if len(sys.argv)>1:child(Path(sys.argv[1]))
    else:
        OUT.mkdir(parents=True)
        startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
        p=subprocess.run([sys.executable,__file__,str(OUT)],creationflags=subprocess.CREATE_NEW_CONSOLE,startupinfo=startup,capture_output=True,text=True,timeout=180)
        if p.returncode:raise AssertionError(p.stdout+'\n'+p.stderr)
        assert (OUT/'result.json').exists()
        print('Real hidden-console TUI tests passed:',OUT)
