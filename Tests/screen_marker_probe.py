# -*- coding: utf-8 -*-
"""2-14 screen comparison probe (NOT for the repo). In BOTH scenes, just before DrawPoints, append 4 markers at fixed
NDC positions with fixed radii (Pos.z) and colours, one of them half transparent. They go through the same VS / GS /
PS / constant buffer (aspect) / blend as everything else, so a change in any of those changes their pixels.
Positions are in the corners, where neither scene draws anything early on.
Apply it to a worktree OUTSIDE the repo (it edits RenderSystem.h and SurvivorScene.cpp), build Release there,
then shoot with screen_marker_shots.ps1 and compare two shot folders with screen_marker_compare.py.
Tooth (2-14): the shader without the aspect ratio, or the constant buffer's fields swapped, changes every marker.
usage: python -I screen_marker_probe.py <tree>"""
import sys
root = sys.argv[1].rstrip("\\/") + "\\"
BLOCK = (
    "{   // [2-14 PROBE] 4 fixed markers, drawn last\n"
    "  const std::size_t probeN0 = vertices.GetSize();\n"
    "  if (vertices.TryResize(probeN0 + 4u)) {\n"
    "    const float probeM[4][8] = {\n"
    "      { -0.85f,  0.80f, 0.04f, 1.0f,  0.0f, 1.0f, 1.0f, 1.0f },\n"
    "      {  0.85f,  0.80f, 0.03f, 1.0f,  1.0f, 0.0f, 1.0f, 1.0f },\n"
    "      { -0.85f, -0.80f, 0.02f, 1.0f,  1.0f, 1.0f, 0.0f, 1.0f },\n"
    "      {  0.85f, -0.80f, 0.05f, 1.0f,  1.0f, 0.5f, 0.0f, 0.5f } };\n"
    "    for (std::size_t j = 0; j < 4u; ++j) {\n"
    "      vertices[probeN0 + j].Pos   = DirectX::XMFLOAT4(probeM[j][0], probeM[j][1], probeM[j][2], probeM[j][3]);\n"
    "      vertices[probeN0 + j].Color = DirectX::XMFLOAT4(probeM[j][4], probeM[j][5], probeM[j][6], probeM[j][7]);\n"
    "    }\n"
    "  }\n"
    "}\n")
for rel, indent in ((r"Source\Graphics\RenderSystem.h", "      "), (r"Source\Game\SurvivorScene.cpp", "    ")):
    p = root + rel
    raw = open(p, "rb").read()
    try:
        s = raw.decode("utf-8-sig"); enc = "utf-8"
    except UnicodeDecodeError:
        s = raw.decode("cp932"); enc = "cp932"
    bom = raw.startswith(b"\xef\xbb\xbf")
    s = s.replace("\r\n", "\n")
    anchor = indent + "renderer.DrawPoints(vertices.GetData(), vertices.GetSize());\n"
    if s.count(anchor) != 1:
        sys.exit("%s anchor %d" % (rel, s.count(anchor)))
    block = "".join(indent + l + "\n" for l in BLOCK.rstrip("\n").split("\n"))
    s = s.replace(anchor, block + anchor)
    open(p, "wb").write((b"\xef\xbb\xbf" if bom else b"") + s.replace("\n", "\r\n").encode(enc))
    print("patched", rel)
