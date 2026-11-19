#!/usr/bin/env python3
"""Build ../skins/ (the hand-made Plugin Skins) for Binaural Echo.

  MAIN   four 3D tiles from art/mockup.html: Azimuth, Elevation, Distance, Mix.
         Each tile = a 240 x 240 film strip (64 frames, rendered by the mockup's own
         drawing code in headless Chrome) bound to the parameter, the parameter's value
         text, and a swipeable value bar. Tile names, wordmark and frame are baked into
         bg_main.png (also rendered from the mockup).
  MOTION, SPACE  control tabs in the style of Grainbloom's CONTROLS tab: the MAIN
         background with faint rounded column panels and spaced-capital headers (rendered
         from the mockup, so the type matches), kit knobs and toggles in 278 x 107 slots,
         choices as MPC-style choice boxes. Each column is one Q-Link bank (the MPC One's
         four knobs, top to bottom).
  ../ui.json is still run through the SDK generator first: it supplies the kit
  components (knobUni, toggle), the value overlays and the choice names.

Steps (run from anywhere, after one `python3 tools/build.py "plugins/Binaural Echo"` so that
build/params.json exists):
  1. tools/gen_ui.py -> build/gen-skin (both tabs, kit PNGs, overlays)
  2. copy that to skins/
  3. render bg_main.png and tile_*.png with headless Chrome; bar strips with PIL
  4. replace the first tab in skins/TUI.json with the tile tab (text edit: the file has
     duplicate "version" keys the MPC needs, so it is never round-tripped through json)
  5. Q-Links.json tab 1: 13, 9, 5, 1 = Azimuth, Elevation, Distance, Mix (the MPC One's
     column of four)

  python3 "plugins/Binaural Echo/art/make_skin.py"
"""
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJ))
SKINS = os.path.join(PROJ, "skins")
BUILD = os.path.join(PROJ, "build")
MOCKUP = os.path.join(HERE, "mockup.html")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

# Parameter indices (must match src/BinauralEcho.cpp)
(VOLUME, AZIMUTH, ELEVATION, DISTANCE, MIX, MODE, WIDTH, ROOM, AIR,
 MOTION, RATE, SYNC, DIVISION, DEPTH, REVERSE, SHOW_AZ, SHOW_EL, SHOW_DIST, INTENSITY,
 ECHO, ECHO_TIME, ECHO_SYNC, ECHO_DIV, FEEDBACK, STEP, TILT, DAMPING) = range(27)

# Control tabs: (tab name, [(column header, [(param, component)] top to bottom)])
PANEL_TABS = [
    ("MOTION", [("PATH",   [(MOTION, "choice"), (DEPTH, "knobUni"), (REVERSE, "toggle")]),
                ("SPEED",  [(RATE, "knobUni"), (SYNC, "toggle"), (DIVISION, "choice")])]),
    ("ECHO",   [("LEVEL",  [(ECHO, "knobUni"), (FEEDBACK, "knobUni"), (DAMPING, "knobUni")]),
                ("TIME",   [(ECHO_TIME, "knobUni"), (ECHO_SYNC, "toggle"), (ECHO_DIV, "choice")]),
                ("TRAVEL", [(STEP, "knobBi"), (TILT, "knobBi")])]),
    ("SPACE",  [("RENDER", [(MODE, "choice"), (INTENSITY, "knobUni"), (WIDTH, "knobUni")]),
                ("ROOM",   [(ROOM, "knobUni"), (AIR, "knobUni")]),
                ("OUTPUT", [(VOLUME, "knobUni")])]),
]
COL_X = [60, 380, 700]          # slot left edges (slot 278 x 107), as Grainbloom
ROW_Y = [136, 251, 366, 481]    # first row well clear of the column header (centre y 96)
PANEL_TOP, PANEL_BOTTOM, PANEL_PAD = 68, 510, 10   # panels end ~35 px below the 3rd row
BOX = (15, 44, 248, 58)         # choice box inside the 278 x 107 slot

TILE_FRAMES = 64      # the mockup quantises to 64 steps
BAR_FRAMES = 127
TILE_X = [23, 334, 645, 956]   # tile left edge on the tab, tile width 298
PIC = 240
PIC_Y = 166                     # 66 (tile top) + 100
VALUE_Y = 456                   # 66 + 390
BAR_W, BAR_H = 210, 4
BAR_Y = 518                     # 66 + 452
TOUCH_W, TOUCH_H = 254, 60      # swipe area around the bar
BAR_IN_X, BAR_IN_Y = 22, 18

# (param, mockup tile index, strip file, bipolar, component name, picture's parameter)
# The pictures follow the read-only ~Az / ~El / ~Dist (where the source is now,
# including Motion); taps, the bar and the value text use the parameter itself.
COLUMNS = [
    (AZIMUTH,   0, "tile_azimuth.png",   True,  "tileAzimuth",   SHOW_AZ),
    (ELEVATION, 1, "tile_elevation.png", True,  "tileElevation", SHOW_EL),
    (DISTANCE,  2, "tile_distance.png",  False, "tileDistance",  SHOW_DIST),
    (MIX,       3, "tile_mix.png",       False, "tileMix",       MIX),
]


# ---------------------------------------------------------------------------
# 1-2. Generated skin as the base
# ---------------------------------------------------------------------------
def generate_base():
    params = os.path.join(BUILD, "params.json")
    if not os.path.exists(params):
        sys.exit('build/params.json missing: run python3 tools/build.py "plugins/Binaural Echo" first')
    out = os.path.join(BUILD, "gen-skin")
    shutil.rmtree(out, ignore_errors=True)
    subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"),
                           "--params", params, "--ui", os.path.join(PROJ, "ui.json"),
                           "--out", out, "--kit-cache", BUILD])
    shutil.rmtree(SKINS, ignore_errors=True)
    shutil.copytree(out, SKINS)


# ---------------------------------------------------------------------------
# 3. Art
# ---------------------------------------------------------------------------
RENDER_JS = r"""
<style id="render-mode"></style>
<script>
(function(){
  const mode = location.hash.slice(1);
  const css = document.getElementById('render-mode');
  if (mode === 'bg') {
    css.textContent = `
      html,body{background:#0b0c10!important;padding:0!important;margin:0!important;display:block!important;overflow:hidden}
      .note,.host-top,.host-tabs{display:none!important}
      .wrap{width:1280px!important}
      .stage-box{width:1280px!important;height:628px!important;aspect-ratio:auto!important}
      .stage{transform:none!important;top:-110px!important;border-radius:0!important;box-shadow:none!important}
      .tile canvas,.tile .val{visibility:hidden!important}
      .tile .bar i{display:none!important}`;
  } else if (mode.startsWith('panels|')) {
    // #panels|TAB|HEAD1,HEAD2,...  : MAIN background, no tiles, faint column panels + headers
    const [, tab, heads] = mode.split('|');
    css.textContent = `
      html,body{background:#0b0c10!important;padding:0!important;margin:0!important;display:block!important;overflow:hidden}
      .note,.host-top,.host-tabs,#tiles{display:none!important}
      .wrap{width:1280px!important}
      .stage-box{width:1280px!important;height:628px!important;aspect-ratio:auto!important}
      .stage{transform:none!important;top:-110px!important;border-radius:0!important;box-shadow:none!important}
      .panel{position:absolute;border-radius:14px;background:rgba(255,255,255,.03)}
      .panel .head{position:absolute;left:0;right:0;top:8px;text-align:center;font-size:26px;font-weight:600;letter-spacing:.24em;text-transform:uppercase}`;
    document.querySelector('.brand').innerHTML = '<b>BINAURAL ECHO</b> &nbsp;/&nbsp; ' + tab;
    const content = document.querySelector('.content');
    heads.split(',').forEach((h, i) => {
      const x = %COLX%[i];
      const p = document.createElement('div');
      p.className = 'panel';
      p.style.cssText = `left:${x - %PAD%}px;top:%TOP%px;width:${278 + 2 * %PAD%}px;height:%HEIGHT%px`;
      p.innerHTML = '<div class="head">' + h + '</div>';
      content.appendChild(p);
    });
  } else if (mode === 'strips') {
    const out = {};
    for (let i = 0; i < 4; i++) out[i] = window.renderStrip(i, %FRAMES%, 240);
    document.body.innerHTML = '<pre id="out">' + JSON.stringify(out) + '</pre>';
  }
})();
</script>
</body>"""


def chrome(args):
    if not os.path.exists(CHROME):
        sys.exit("Google Chrome not found at " + CHROME)
    base = [CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
            "--force-device-scale-factor=1", "--allow-file-access-from-files"]
    return subprocess.run(base + args, check=True, capture_output=True, text=True).stdout


def render_art():
    html = open(MOCKUP, encoding="utf-8").read()
    js = (RENDER_JS.replace("%FRAMES%", str(TILE_FRAMES)).replace("%COLX%", json.dumps(COL_X))
          .replace("%PAD%", str(PANEL_PAD)).replace("%TOP%", str(PANEL_TOP))
          .replace("%HEIGHT%", str(PANEL_BOTTOM - PANEL_TOP)))
    html = html.replace("</body>", js, 1)
    tmp = tempfile.mkdtemp(prefix="binaural-art-")
    page = os.path.join(tmp, "render.html")
    open(page, "w", encoding="utf-8").write(html)
    url = "file://" + page

    # Background: the tab area (1280 x 628) without pictures, values or bar fills.
    shot = os.path.join(tmp, "bg.png")
    chrome(["--window-size=1280,628", "--virtual-time-budget=4000",
            "--screenshot=" + shot, url + "#bg"])
    from PIL import Image
    Image.open(shot).convert("RGB").crop((0, 0, 1280, 628)).save(os.path.join(SKINS, "bg_main.png"), optimize=True)
    for tab, columns in PANEL_TABS:
        chrome(["--window-size=1280,628", "--virtual-time-budget=4000", "--screenshot=" + shot,
                url + "#panels|%s|%s" % (tab, ",".join(h for h, _ in columns))])
        Image.open(shot).convert("RGB").crop((0, 0, 1280, 628)).save(
            os.path.join(SKINS, "bg_%s.png" % tab.lower()), optimize=True)

    # Film strips, straight from the mockup's renderStrip().
    dom = chrome(["--virtual-time-budget=60000", "--dump-dom", url + "#strips"])
    m = re.search(r'<pre id="out">(.*?)</pre>', dom, re.S)
    if not m:
        sys.exit("strip render failed")
    strips = json.loads(m.group(1).replace("&quot;", '"'))
    for _, idx, name, _, _, _ in COLUMNS:
        data = strips[str(idx)].split(",", 1)[1]
        path = os.path.join(SKINS, name)
        open(path, "wb").write(base64.b64decode(data))
        img = Image.open(path)
        assert img.size == (PIC, PIC * TILE_FRAMES), (name, img.size)
        img.save(path, optimize=True)
    shutil.rmtree(tmp, ignore_errors=True)


def render_bars():
    """bar_touch_uni.png / bar_touch_bi.png: 127 frames, the bar fill (cyan gradient)
    on a transparent 254 x 60 swipe frame. The track and centre mark are in bg_main.png."""
    from PIL import Image
    lo, hi = (0x1f, 0x7c, 0x95), (0x46, 0xe3, 0xff)

    def grad(t):
        return tuple(int(lo[k] + (hi[k] - lo[k]) * t) for k in range(3)) + (255,)

    for name, bipolar in (("bar_touch_uni.png", False), ("bar_touch_bi.png", True)):
        out = Image.new("RGBA", (TOUCH_W, TOUCH_H * BAR_FRAMES), (0, 0, 0, 0))
        px = out.load()
        for f in range(BAR_FRAMES):
            v = f / (BAR_FRAMES - 1)
            a, b = (min(v, 0.5), max(v, 0.5)) if bipolar else (0.0, v)
            x0, x1 = int(round(a * BAR_W)), int(round(b * BAR_W))
            if x1 - x0 < 1 and bipolar:
                continue
            w = max(1, x1 - x0)
            for x in range(x0, x0 + w):
                c = grad((x - x0) / max(1, w - 1))
                for y in range(BAR_H):
                    # rounded 2 px ends
                    if (x == x0 or x == x0 + w - 1) and y in (0, BAR_H - 1):
                        continue
                    px[BAR_IN_X + x, f * TOUCH_H + BAR_IN_Y + y] = c
        out.save(os.path.join(SKINS, name), optimize=True)


def render_choice_art():
    """choice_box.png: dark rounded field; choice_focus.png: cyan outline shown while selected."""
    from PIL import Image, ImageDraw
    ss, (_, _, w, h) = 4, BOX

    def draw(fn, paint):
        im = Image.new("RGBA", (w * ss, h * ss), (0, 0, 0, 0))
        paint(ImageDraw.Draw(im))
        im.resize((w, h), Image.LANCZOS).save(os.path.join(SKINS, fn), optimize=True)
    inset = 5
    draw("choice_box.png", lambda d: d.rounded_rectangle(
        [inset * ss, inset * ss, (w - inset) * ss, (h - inset) * ss], radius=8 * ss,
        fill=(32, 36, 46, 255), outline=(44, 48, 57, 255), width=ss))
    draw("choice_focus.png", lambda d: d.rounded_rectangle(
        [ss, ss, (w - 1) * ss, (h - 1) * ss], radius=11 * ss,
        outline=(70, 227, 255, 255), width=int(1.6 * ss)))


# ---------------------------------------------------------------------------
# 4. TUI.json
# ---------------------------------------------------------------------------
def bounds(rect, focus="No", show="Show"):
    return {"version": 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": show,
            "whenVisible": "Always", "boundsType": "Absolute", "bounds": rect}


def entry(name, wtype, data, rect, remap=None, **kw):
    return {"version": 2,
            "componentData": {"version": 1, "name": name, "type": wtype, "data": data},
            "handle remapping": {"version": 1, "map": remap or []},
            "bounds": bounds(rect, **kw)}


def text_style(height, colour, style="Regular", just="horizontallyCentred verticallyCentred"):
    return {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": style, "height": height},
            "colour": colour, "justification": just, "case": "Original"}


def touch_definition(overlay, components):
    """Tap = bank the Q-Links to this control; double-tap / Enter = the MPC's value editor."""
    clear = {"version": 1, "colour": "0", "image": ""}
    return {"version": 2,
            "actions": [
                {"version": 1, "onAction": "Mouse Down", "handler": "Q-Link", "handleName": "Data", "additionalData": ""},
                {"version": 1, "onAction": "Double Click", "handler": "Show Overlay", "handleName": "Data", "additionalData": overlay},
                {"version": 1, "onAction": "Enter Pressed", "handler": "Show Overlay", "handleName": "Data", "additionalData": overlay}],
            "backgroundData": {"version": 1, "focussed": clear, "unfocussed": dict(clear)},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False,
            "componentsData": components}


def picture_definition(bipolar):
    """An invisible touch area over the picture (the picture itself is a plain strip)."""
    return touch_definition("knob overlay bipolar" if bipolar else "knob overlay", [])


def bar_definition(bipolar):
    knob = {"version": 3, "knobType": "FilmStrip",
            "filmStrip": "bar_touch_bi.png" if bipolar else "bar_touch_uni.png",
            "numFrames": BAR_FRAMES, "invert": False, "dragOrientation": "Horizontal",
            "handleName": "Data"}
    overlay = "knob overlay bipolar" if bipolar else "knob overlay"
    return touch_definition(overlay, [entry("knob", "Knob", knob, "0 0 %d %d" % (TOUCH_W, TOUCH_H))])


def main_tab():
    value_style = text_style(40.0, "ffe9edf5", style="Light")
    comps = []
    for (param, _, strip, bipolar, name, shown), x in zip(COLUMNS, TILE_X):
        remap = [{"key": "Data", "value": "Parameter %d" % param}]
        comps.append(entry("picture %d" % param, "Image",
                           {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": strip,
                            "numFrames": TILE_FRAMES, "handleName": "Parameter %d" % shown},
                           "%d %d %d %d" % (x + 29, PIC_Y, PIC, PIC)))
        comps.append(entry("tile %d" % param, name, {"version": 1, "handleName": "Data"},
                           "%d %d %d %d" % (x + 29, PIC_Y, PIC, PIC), remap=remap, focus="Yes", show="Hide"))
        comps.append(entry("value p%d" % param, "Label",
                           {"version": 1, "textStyle": value_style, "type": "Value",
                            "handleName": "Parameter %d" % param},
                           "%d %d 298 56" % (x, VALUE_Y)))
        comps.append(entry("bar %d" % param, name + "Bar", {"version": 1, "handleName": "Data"},
                           "%d %d %d %d" % (x + 44 - BAR_IN_X, BAR_Y - BAR_IN_Y, TOUCH_W, TOUCH_H),
                           remap=remap, focus="Yes", show="Hide"))
    bg = {"version": 1, "colour": "ff0b0c10", "image": "bg_main.png"}
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": bg, "unfocussed": dict(bg)},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False,
            "componentsData": comps}
    # No Q-Link highlight rectangle: the frame round all four tiles is in bg_main.png.
    return {"version": 1, "tabName": "MAIN", "fnKeyIndex": 0, "fnKeySubIndex": 0,
            "componentDefinition": root, "qlinkBoundsData": []}


def choice_definition():
    """278 x 107 slot: grey capitalised name, then a rounded box with the choice in cyan.
    Tap banks the Q-Links; the Q-Link / data wheel step through the choices. No list
    overlay: the MPC's MenuOverlay opens empty for plugin choices (seen with Grainbloom)."""
    clear = {"version": 1, "colour": "0", "image": ""}
    x, y, w, h = BOX
    box = "%d %d %d %d" % BOX
    img = lambda n, f: entry(n, "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": f}, box)
    focus = img("focus", "choice_focus.png")
    focus["bounds"]["whenVisible"] = "WhenFocussed"
    name = text_style(28.0, "ff7d8496", style="Bold")
    name["case"] = "Upper Case"
    value = text_style(34.0, "ff46e3ff")
    return {"version": 2,
            "actions": [{"version": 1, "onAction": "Mouse Down", "handler": "Q-Link",
                         "handleName": "Data", "additionalData": ""}],
            "backgroundData": {"version": 1, "focussed": clear, "unfocussed": dict(clear)},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False,
            "componentsData": [
                entry("name", "Label", {"version": 1, "textStyle": name, "type": "Name",
                                        "handleName": "Data"}, "15 2 248 40"),
                img("box", "choice_box.png"),
                focus,
                entry("value", "Label", {"version": 1, "textStyle": value, "type": "Value",
                                         "handleName": "Data"}, "%d %d %d %d" % (x + 10, y, w - 20, h)),
            ]}


def panel_tab(index, tab, columns):
    comps = []
    for (_, controls), x in zip(columns, COL_X):
        for (param, comp), y in zip(controls, ROW_Y):
            comps.append(entry("%s %d" % (comp, param), "choiceBox" if comp == "choice" else comp,
                               {"version": 1, "handleName": "Data"}, "%d %d 278 107" % (x, y),
                               remap=[{"key": "Data", "value": "Parameter %d" % param}], focus="Yes"))
    bg = {"version": 1, "colour": "ff0b0c10", "image": "bg_%s.png" % tab.lower()}
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": bg, "unfocussed": dict(bg)},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False,
            "componentsData": comps}
    rows = max(len(c) for _, c in columns)
    top = ROW_Y[0] - 10
    q = ["%d %d 298 %d" % (x - 10, top, ROW_Y[rows - 1] + 107 + 10 - top) for x, _ in zip(COL_X, columns)]
    return {"version": 1, "tabName": tab, "fnKeyIndex": index, "fnKeySubIndex": 0,
            "componentDefinition": root, "qlinkBoundsData": q}


def indent(text, spaces):
    pad = " " * spaces
    return "\n".join(pad + line if line else line for line in text.splitlines())


def match_close(t, start, open_ch, close_ch):
    """Index just past the bracket matching t[start], skipping JSON strings."""
    depth, i, in_str = 0, start, False
    while i < len(t):
        ch = t[i]
        if in_str:
            if ch == "\\":
                i += 1
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
        elif ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced")


def patch_tui():
    path = os.path.join(SKINS, "TUI.json")
    t = open(path, encoding="utf-8").read()
    # Replace all tabs: MAIN tiles, then the control tabs.
    a = t.index("[", t.index('"tabs":'))
    end = match_close(t, a, "[", "]")
    tabs = [main_tab()] + [panel_tab(i + 1, tab, cols) for i, (tab, cols) in enumerate(PANEL_TABS)]
    t = t[:a] + "[\n" + ",\n".join(indent(json.dumps(x, indent=4), 12) for x in tabs) + "\n        ]" + t[end:]
    # Component definitions for the tiles, bars and choice boxes.
    defs = [("choiceBox", choice_definition())]
    for _, _, strip, bipolar, name, _ in COLUMNS:
        defs.append((name, picture_definition(bipolar)))
        defs.append((name + "Bar", bar_definition(bipolar)))
    key = '"localComponentDefinitions": ['
    i = t.index(key) + len(key)
    items = "".join("\n" + indent(json.dumps({"key": k, "value": v}, indent=4), 16) + ","
                    for k, v in defs)
    t = t[:i] + items + t[i:]
    open(path, "w", encoding="utf-8").write(t)

    # The generated MAIN tab used kit knobs and a fader; drop images nothing uses now.
    used = set(re.findall(r'"[A-Za-z]*(?:[iI]mage|filmStrip)": "([^"]+\.png)"', t))
    for f in os.listdir(SKINS):
        if f.endswith(".png") and f not in used:
            os.remove(os.path.join(SKINS, f))


# ---------------------------------------------------------------------------
# 5. Q-Links.json
# ---------------------------------------------------------------------------
def patch_qlinks():
    path = os.path.join(SKINS, "Q-Links.json")
    q = json.load(open(path, encoding="utf-8"))
    maps = []
    screen = {"Q-Link %d" % i: -1 for i in range(1, 17)}
    for (p, _, _, _, _, _), ql in zip(COLUMNS, (13, 9, 5, 1)):
        screen["Q-Link %d" % ql] = p
    maps.append({"Tab": 1, "SubTab": 1, "Bank Direction": "Column", "Q-Links": screen})
    order = [c[0] for c in COLUMNS]
    for i, (_, columns) in enumerate(PANEL_TABS):
        screen = {"Q-Link %d" % n: -1 for n in range(1, 17)}
        for c, (_, controls) in enumerate(columns):
            for r, (param, _) in enumerate(controls):
                screen["Q-Link %d" % (13 + c - 4 * r)] = param   # column c, row r
                order.append(param)
        maps.append({"Tab": i + 2, "SubTab": 1, "Bank Direction": "Column", "Q-Links": screen})
    q["Screen Mode Q-Links"]["map"] = maps
    # Program mode: MAIN's four first, then the control tabs in order.
    # The display copies (~Az, ~El, ~Dist) are read-only: never on a Q-Link.
    q["Program Mode Q-Links"] = {"Q-Link %d" % (i + 1): (order[i] if i < len(order) else -1)
                                 for i in range(16)}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(q, f, indent=4)
        f.write("\n")


if __name__ == "__main__":
    generate_base()
    render_art()
    render_bars()
    render_choice_art()
    patch_tui()
    patch_qlinks()
    print("skins/ written:", ", ".join(sorted(os.listdir(SKINS))))
