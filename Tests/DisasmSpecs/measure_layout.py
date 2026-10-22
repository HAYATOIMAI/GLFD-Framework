# -*- coding: utf-8 -*-
"""Compile a layout TU (measure_layout.cpp by default) against a tree with the Release flags and print what it prints.
For disasm_compare.py specs: offsets and sizes are measured, never worked out by hand.
usage: python -I measure_layout.py <tree> <tag> [<file.cpp> [DEFINE ...]]   (output under this folder\\<tag>)"""
import os, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
tree, tag = sys.argv[1], sys.argv[2]
src = sys.argv[3] if len(sys.argv) > 3 else "measure_layout.cpp"
defs = " ".join("/D" + d for d in sys.argv[4:])
d = os.path.join(here, tag)
os.makedirs(d, exist_ok=True)
bat = os.path.join(d, "b.bat")
open(bat, "w", encoding="ascii").write(
    "@echo off\r\n"
    "set \"VSWHERE=%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"\r\n"
    "for /f \"usebackq delims=\" %%i in (`\"%VSWHERE%\" -latest -products * -property installationPath`) do set \"VSPATH=%%i\"\r\n"
    "call \"%VSPATH%\\VC\\Auxiliary\\Build\\vcvars64.bat\" >nul 2>&1\r\n"
    "cl /nologo /std:c++20 /permissive- /EHsc /GR- /O2 /MD /DNDEBUG /DUNICODE /D_UNICODE " + defs + " /I \"" + tree + "\\Source\" "
    "/Fo\"" + d + "\\\\\" \"" + os.path.join(here, src) + "\" /Fe\"" + d + "\\s.exe\" >\"" + d + "\\build.log\" 2>&1\r\n"
    "if errorlevel 1 exit /b 90\r\n"
    "\"" + d + "\\s.exe\"\r\n")
r = subprocess.run(["cmd", "/c", bat], capture_output=True, timeout=600)
print(tag, r.returncode)
print(r.stdout.decode("cp932", "replace"))
if r.returncode == 90:
    print(open(os.path.join(d, "build.log"), "rb").read().decode("cp932", "replace")[-1500:])
