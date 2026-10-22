# -*- coding: utf-8 -*-
r"""
GLFD: per-function machine-code comparison of the game's translation units between two trees
(2-8 / 2-10 / 2-11 / 2-13 / 2-14 each rebuilt this by hand; kept here since 2-14).

Each Source\*.cpp in a tree's GameLib_conteinar.vcxproj is compiled with the Release flags minus /GL (so the .obj
holds machine code) plus /Gy, disassembled with dumpbin /disasm, split per function and normalised:
  - addresses of relocations (XXXXXXXXh with 8-16 digits) and jump targets inside the function -> placeholders
  - anonymous-namespace names (they carry a hash of the file path)
A TU present in only one tree is listed as such. A function present on one side only, with a body identical to a
function present on the other side only, is RENAMED.

Verdicts for a function that exists in both trees and differs (with --spec; without it, only DIFFER / NUMBERS):
  EXPECTED     the function is in spec "edited" (it had lines removed, or inlines one that did), or is an
               unwind funclet (dtor$N) of such a function
  LAYOUT-OK    same instructions; EVERY changed number is explained by the spec:
                 - [reg+X] with reg not rsp/rbp (a member): the old X lies behind a removed member of a class in
                   spec "classes", and old - new equals the total size removed in front of it (measured offsets)
                 - an immediate: (old, new) is one of spec "sizes" (sizeof before / after)
                 - [rsp+X] / [rbp+X] / the frame set-up (sub rsp, add rsp, mov eax before __chkstk): the function is
                   in spec "stack.functions" and |old - new| is one of spec "stack.deltas"
  UNEXPLAINED  anything else: a different instruction outside "edited", or a number the spec does not explain
               (e.g. an unrelated constant 600 -> 601). This is the "it was used" signal: stop and report.
Exit code: 0 when nothing is UNEXPLAINED, 1 otherwise, 2 on a build failure.

usage: python -I disasm_compare.py <base tree> <new tree> <out dir> [--spec spec.json] [--reuse]
spec.json:
  { "edited":  [ {"match": "?Initialize@DX11Renderer@", "why": "removed the sampler"} , ...],
    "classes": [ {"name": "GameEngine", "removed": [ {"offset": 88, "size": 8, "member": "m_resourceManager"} ]},
                 {"name": "GameEngine", "removed": [ {"offset": 124, "size": 4, "until": 136, "member": "m_totalTime"} ]} ],
               (offsets and shifts are measured with the compiler: offsetof in both trees, never worked out by hand)
    "sizes":   [ [160, 144, "sizeof DX11Renderer"] ],
    "stack":   {"functions": ["main"], "deltas": [16, 8], "why": "GameEngine lives on main's stack"} }
How to use it for a removal (ECS 2-14):
  1. a worktree OUTSIDE the repo at the commit before the change:  git worktree add --detach ..\_base <commit>
  2. measure the layout of every class that lost a member, in both trees, with the compiler (never by hand):
       Tests\DisasmSpecs\measure_layout.py <tree> <tag> measure_layout.cpp [defines]   (edit the .cpp for the members)
     padding matters: removing a 4-byte float in front of a pointer moved the pointer by 8 (GameContext), and a
     float between a bool and a uint32 moved 4 bytes of members and nothing after the next 8-byte boundary (GameEngine)
  3. write the spec (Tests\DisasmSpecs\ECS_2-14_*.json are real ones): "edited" = the functions that had lines
     removed, PLUS the ones that inline them (find them from the report; say why in "why")
  4. python -I Tests\disasm_compare.py ..\_base . <out dir> --spec <spec.json>     -> UNEXPLAINED: 0, or stop
Tooth (2-14): a copy where only kSummaryEverySteps (SurvivorScene.cpp) goes 600 -> 601 is UNEXPLAINED with the
commit-1 spec ("258h -> 259h in 'imul rax,rdx,258h'"); without a spec the same change is only "NUMBERS".
Run the .bat-based Tests tools from PowerShell / cmd, one at a time (they share %TEMP% files).
"""
import json, os, re, subprocess, sys

FLAGS = "/nologo /c /O2 /Oi /Gy /W3 /std:c++20 /permissive- /EHsc /GR- /DNDEBUG /D_CONSOLE /DUNICODE /D_UNICODE /MD"
ADDR = re.compile(r"^\s+[0-9A-F]{16}: (?:[0-9A-F]{2} )+\s*")
HEX = re.compile(r"\b[0-9A-F]{8,16}h\b")
ANON = re.compile(r"`anonymous namespace'|\?A0x[0-9a-f]+")
JMP = re.compile(r"\b[0-9A-F]{16}\b")
NUMTOK = re.compile(r"\b(?:[0-9][0-9A-F]*h|[0-9A-F]+h|\d+)\b")
STACKREG = ("rsp", "rbp")


def tus(tree):
    s = open(os.path.join(tree, "GameLib_conteinar.vcxproj"), "rb").read().decode("utf-8-sig", "replace")
    return [m for m in re.findall(r'<ClCompile Include="([^"]+)"', s) if m.startswith("Source\\")]


def build(tree, out, tag, units):
    d = os.path.join(out, tag)
    os.makedirs(d, exist_ok=True)
    lines = ["@echo off",
             'set "VSWHERE=%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe"',
             'for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSPATH=%%i"',
             'call "%VSPATH%\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul 2>&1']
    for tu in units:
        name = os.path.splitext(os.path.basename(tu))[0]
        lines.append('cl %s /I "%s\\Source" /Fo"%s\\%s.obj" "%s\\%s" >"%s\\%s.cl.log" 2>&1 || echo CLFAIL %s'
                     % (FLAGS, tree, d, name, tree, tu, d, name, name))
        lines.append('dumpbin /nologo /disasm "%s\\%s.obj" >"%s\\%s.asm"' % (d, name, d, name))
    bat = os.path.join(d, "run.bat")
    open(bat, "w", encoding="ascii").write("\r\n".join(lines) + "\r\n")
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, timeout=3000)
    txt = r.stdout.decode("cp932", "replace")
    if "CLFAIL" in txt:
        print("compile failed in %s: %s" % (tag, txt))
        sys.exit(2)
    return d


def functions(asm_path):
    funcs, cur, body = {}, None, []
    for line in open(asm_path, "rb").read().decode("cp932", "replace").splitlines():
        if line and not line.startswith(" ") and line.endswith(":"):
            if cur is not None:
                funcs[cur] = body
            cur, body = ANON.sub("ANON", line[:-1]), []
            continue
        if cur is None:
            continue
        m = ADDR.match(line)
        if not m:
            continue
        ins = HEX.sub("ADDR", ANON.sub("ANON", line[m.end():]))
        ins = JMP.sub("TARGET", ins)
        body.append(re.sub(r"\s+", " ", ins).strip())
    if cur is not None:
        funcs[cur] = body
    return funcs


def value(tok):
    return int(tok[:-1], 16) if tok.endswith("h") else int(tok)


def numbers(ins):
    """[(kind, base_register, signed value, token)] for every number in an instruction."""
    out = []
    spans = [(m.start(), m.end(), m.group(1)) for m in re.finditer(r"\[([^\]]*)\]", ins)]
    for m in NUMTOK.finditer(ins):
        inside = [s for s in spans if s[0] < m.start() < s[1]]
        if inside:
            body = inside[0][2]
            reg = re.match(r"\s*([a-z0-9]+)", body)
            base = reg.group(1) if reg else ""
            sign = -1 if ins[m.start() - 1] == "-" else 1
            out.append(("mem", base, sign * value(m.group()), m.group()))
        else:
            out.append(("imm", "", value(m.group()), m.group()))
    return out


def strip_nops(body):
    return [i for i in body if not i.startswith("nop")]


def shape(i):
    return NUMTOK.sub("N", i)


def parent_of(label):
    m = re.match(r"\?dtor\$\d+@\?0\?\?(.*)", label)
    return "?" + m.group(1) if m else None


def classify(label, a, b, spec):
    """-> (verdict, detail)"""
    if spec is None:
        xa, xb = strip_nops(a), strip_nops(b)
        same_shape = len(xa) == len(xb) and all(shape(x) == shape(y) for x, y in zip(xa, xb))
        return ("NUMBERS" if same_shape else "DIFFER"), ""
    for e in spec.get("edited", []):
        if e["match"] in label or (parent_of(label) and e["match"] in parent_of(label)):
            return "EXPECTED", e.get("why", "")
    xa, xb = strip_nops(a), strip_nops(b)
    if len(xa) != len(xb) or any(shape(x) != shape(y) for x, y in zip(xa, xb)):
        return "UNEXPLAINED", "the instructions differ (not only numbers)"
    stack = spec.get("stack", {})
    in_stack = any(f == label or f in label for f in stack.get("functions", []))
    deltas = set(stack.get("deltas", []))
    sizes = {(s[0], s[1]): (s[2] if len(s) > 2 else "sizeof") for s in spec.get("sizes", [])}
    explained, unexplained = [], []
    for idx, (x, y) in enumerate(zip(xa, xb)):
        if x == y:
            continue
        nx, ny = numbers(x), numbers(y)
        for (k1, r1, v1, t1), (k2, r2, v2, t2) in zip(nx, ny):
            if v1 == v2:
                continue
            why = None
            if k1 == "mem" and r1 not in STACKREG:
                for c in spec.get("classes", []):
                    removed = c["removed"]
                    # "size" is the measured shift of what follows (the member plus any padding it took with it);
                    # "until" (optional) is the old offset from which nothing moves any more (padding absorbed it)
                    shift = sum(r["size"] for r in removed
                                if r["offset"] + r["size"] <= v1 and v1 < r.get("until", 1 << 62))
                    if shift > 0 and v1 - v2 == shift:
                        why = "%s: %d -> %d is behind %s (-%d)" % (c["name"], v1, v2,
                              "/".join(r.get("member", "?") for r in removed if r["offset"] + r["size"] <= v1), shift)
                        break
            elif k1 == "mem" and r1 in STACKREG:
                if in_stack and abs(v1 - v2) in deltas:
                    why = "stack %s%+d -> %+d (%s)" % (r1, v1, v2, stack.get("why", ""))
                # a member of an object that lives on this function's stack: [rbp + base + member offset]
                for so in spec.get("stack_objects", []):
                    if why or so["function"] not in label or so.get("register", "rbp") != r1:
                        continue
                    off = v1 - so["base"]
                    for c in spec.get("classes", []):
                        if c["name"] != so["class"]:
                            continue
                        shift = sum(r["size"] for r in c["removed"]
                                    if r["offset"] + r["size"] <= off and off < r.get("until", 1 << 62))
                        if shift > 0 and v1 - v2 == shift:
                            why = "%s on the stack at %s%+d: member %d -> %d (-%d) (%s)" % (
                                so["class"], r1, so["base"], off, off - shift, shift, so.get("why", ""))
                            break
            else:
                if (v1, v2) in sizes:
                    why = "%s %d -> %d" % (sizes[(v1, v2)], v1, v2)
                elif in_stack and abs(v1 - v2) in deltas and (re.match(r"(sub|add) rsp,", x) or
                        (x.startswith("mov eax,") and idx + 1 < len(xa) and "__chkstk" in xa[idx + 1])):
                    why = "stack frame %d -> %d (%s)" % (v1, v2, stack.get("why", ""))
            (explained if why else unexplained).append(why or ("%s -> %s in '%s'" % (t1, t2, x)))
    if unexplained:
        return "UNEXPLAINED", "; ".join(unexplained[:4])
    return "LAYOUT-OK", "; ".join(sorted(set(explained)))


def main():
    base, new, out = sys.argv[1], sys.argv[2], sys.argv[3]
    spec = None
    if "--spec" in sys.argv:
        spec = json.load(open(sys.argv[sys.argv.index("--spec") + 1], encoding="utf-8-sig"))   # specs are saved with a BOM
    tb, tn = tus(base), tus(new)
    if "--reuse" in sys.argv:
        b, n = os.path.join(out, "base"), os.path.join(out, "new")
    else:
        b, n = build(base, out, "base", tb), build(new, out, "new", tn)
    report, bad = [], 0
    nb = {os.path.splitext(os.path.basename(t))[0] for t in tb}
    nn = {os.path.splitext(os.path.basename(t))[0] for t in tn}
    for name in sorted(nb - nn):
        report.append("%-20s TU ONLY IN BASE (removed)" % name)
    for name in sorted(nn - nb):
        report.append("%-20s TU ONLY IN NEW" % name)
    for name in sorted(nb & nn):
        fb, fn = functions(os.path.join(b, name + ".asm")), functions(os.path.join(n, name + ".asm"))
        same = [k for k in fb if k in fn and fb[k] == fn[k]]
        diff = [k for k in fb if k in fn and fb[k] != fn[k]]
        only_b = [k for k in fb if k not in fn]
        only_n = [k for k in fn if k not in fb]
        bodies_n = {tuple(fn[k]): k for k in only_n}
        renamed = {k: bodies_n[tuple(fb[k])] for k in only_b if tuple(fb[k]) in bodies_n}
        report.append("%-20s functions base=%d new=%d identical=%d differ=%d only_base=%d (renamed %d) only_new=%d"
                      % (name, len(fb), len(fn), len(same), len(diff), len(only_b), len(renamed), len(only_n)))
        for k in diff:
            verdict, detail = classify(k, fb[k], fn[k], spec)
            if verdict == "UNEXPLAINED":
                bad += 1
            report.append("    %-11s %s" % (verdict, k[:200]))
            if detail:
                report.append("                %s" % detail[:600])
        for k in only_b:
            report.append("    %-11s %s" % ("RENAMED" if k in renamed else "ONLYBASE", k[:200]))
        for k in only_n:
            if k in renamed.values():
                continue
            verdict = "ONLYNEW"
            if spec is not None:
                p = parent_of(k)
                if p and any(e["match"] in p for e in spec.get("edited", [])):
                    verdict = "ONLYNEW-OK"   # an unwind funclet of an edited function, renumbered
                elif any(e["match"] in k for e in spec.get("edited", [])):
                    verdict = "ONLYNEW-OK"   # an edited function whose signature (so its decorated name) changed
                else:
                    verdict = "UNEXPLAINED"; bad += 1
            report.append("    %-11s %s" % (verdict, k[:200]))
    report.append("UNEXPLAINED: %d" % bad)
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "report.txt"), "w", encoding="utf-8").write("\n".join(report) + "\n")
    print("\n".join(report))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
