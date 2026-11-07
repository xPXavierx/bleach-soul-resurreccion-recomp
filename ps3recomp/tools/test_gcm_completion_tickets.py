#!/usr/bin/env python3
"""Build the production GCM completion fixture without a game or graphics API."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='gcm-completions-') as tmp:
    binary=Path(tmp)/('test.exe' if os.name=='nt' else 'test')
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections']
    if os.name!='nt':linker+=['-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',
      '-ffunction-sections','-fdata-sections','-I'+str(root/'include'),
      '-I'+str(root/'libs/video'),str(root/'libs/video/tests/test_gcm_completion_tickets.c'),
      *linker,'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],timeout=30,capture_output=True,text=True)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-2000:]);raise SystemExit(result.returncode)
