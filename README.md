# Binaural Echo

A 3D headphone panner for Akai MPC standalone hardware, by Lightlike.

Put it on a track's insert and move the sound around your head with the Q-Links: left and right, all the way behind you, above and below, near and far. It is made for **headphones**. On speakers you still get the panning, distance and room, but the sense of height and of sounds behind you mostly disappears.

The MPC only ever sees stereo in and stereo out. All of the 3D work, including the 4-channel ambisonic sound field in Field mode, happens inside the plugin.

## How it works

Three render modes, from most detailed to cheapest:

| Mode | What it does |
|---|---|
| **Head** | Each input channel is a point source rendered through a model of a listener's head. The far ear gets the sound slightly later (interaural time difference, up to about 0.65 ms). A shadow filter darkens the far ear and brightens the near one. Five short echoes model the outer ear, and their timing moves with elevation. A shoulder echo is added for sources above, and the treble is softened for sources behind. Closer than 1 m, the level difference between the ears grows the way it does with a real nearby source. |
| **Field** | First-order ambisonics. The left and right inputs are encoded into a 4-channel sound field (W, X, Y, Z) at ± Width. The whole field is rotated by Azimuth and Elevation, then decoded to eight virtual speakers on a cube, each heard through the head model. Sounds are a little more diffuse than Head mode, and wide stereo material holds together well. |
| **Lite** | Only time and level differences between the ears, a simple shadow filter on the far ear, and the rear treble cue. It uses the least CPU and has no elevation cue. |

### Intensity

A natural head model is subtle: real binaural cues are small, and on a mix they are easy to miss. **Intensity** (70 % by default) exaggerates every cue:

- a wider time gap between the ears (up to 1.8 times a real head)
- a deeper head shadow and an extra level difference between the ears (about 20 dB at the side instead of 14)
- stronger outer-ear echoes and a darker sound behind you
- direction colour: a presence lift around 3 kHz in front, a lift around 8 kHz above and a dip there below
- a steeper fall-off with distance (+6 dB at 0.2 m, −21 dB at 10 m)
- more air absorption

At 0 % it is the plain head model. Overall loudness at the front stays the same at any setting.

All three modes share the distance cues:

- **Level** falls with distance (+5 dB at 0.2 m, 0 dB at 1 m, −14 dB at 10 m).
- **Air absorption** dulls distant sources (set by Air).
- **Early reflections** arrive closer behind the direct sound as the source moves away.
- **Late reverb** (a small 4-line feedback delay network) stays at a constant level, so far sources sound wetter, as in a real room.

The head model is a structural HRTF model based on Brown & Duda, *A Structural Model for Binaural Echo Sound Synthesis* (IEEE Trans. Speech and Audio Processing, 1998): a spherical-head shadow filter, Woodworth ITD, and pinna and shoulder echoes. It was written from the paper's equations. No measured HRTF data is used, so the plugin needs no data files.

## Motion

Binaural Echo can move the sound by itself, for example circling your head continuously. Motion adds a path on top of the Azimuth, Elevation and Distance you set, so those still decide where the movement is centred. Pick a path on the MOTION tab:

| Motion | Path |
|---|---|
| **Off** | Stays where you put it |
| **Orbit** | Circles the head. Depth tilts the circle, so the sound is higher on one side than the other. |
| **Swing** | Sweeps side to side, up to ±120° at Depth 100 % |
| **Figure 8** | Sweeps side to side while going up and down twice as fast |
| **Spiral** | Circles while slowly rising and falling (one rise and fall every 4 turns) |
| **Fly-by** | Passes by on a straight line, closest at Distance, then comes back. Depth sets how far it travels (2–10 m each way). |

**Rate** sets the speed in cycles per second (0.02–4 Hz). Turn **Sync** on to lock the cycle to the MPC's tempo instead, with **Division** setting the cycle length (8 bars down to 1/8). While the sequence plays, a synced motion follows the song position, so it lands the same way every time through. **Reverse** goes the other way round.

The 3D pictures on the MAIN tab follow the moving sound. The value text and bars show the position you set.

## Echo

A delay whose repeats travel around your head. The delay runs on the 4-channel sound field. On every trip round the feedback loop the field turns by **Step** (and **Tilt**), so each repeat comes back from a new direction. With Step at R 90, the first echo is on your right, the next behind you, then on your left, then in front again. A lowpass inside the loop (**Damping**) makes each repeat darker, so later echoes also sound further away.

- **Echo**: level of the echoes (0 % = off).
- **Echo Time**: 20–2000 ms, or with **Echo Sync** on, **Echo Div** from 1/32 to 1 bar (dotted values included).
- **Feedback**: how many repeats (up to 95 %).
- **Step**: how far each repeat turns. R 90 / L 90 circles you, Back (180) bounces front to back, and small values creep round slowly. None keeps every repeat where it started.
- **Tilt**: how far each repeat tips up or down as it goes round. Combined with Step, the echoes trace a tilted circle, rising on one side and falling on the other.
- **Damping**: how quickly repeats darken.

Each echo starts where the sound was when it was played. With Motion running and Step at None, the echoes stay behind the moving source like a trail (the Comet Orbit preset).

## Parameters

| # | Name | Range | Default | What it does |
|---|---|---|---|---|
| 0 | Volume | −24 … +12 dB | 0 dB | Output level |
| 1 | Azimuth | L 180 … Front … R 180 | Front | Direction round the head; ±180 is behind |
| 2 | Elevation | Down 90 … Level … Up 90 | Level | Height |
| 3 | Distance | 0.20 … 10 m | 1.0 m | How far away the source is |
| 4 | Mix | 0 … 100 % | 100 % | Dry / processed |
| 5 | Mode | Head / Field / Lite | Head | Render mode (switching crossfades in about 20 ms) |
| 6 | Width | 0 … 100 % | 40 % | Spread of the input's left and right around the target direction (100 % = ±45°). 0 % makes a mono point. |
| 7 | Room | 0 … 100 % | 35 % | Early reflections and reverb |
| 8 | Air | 0 … 100 % | 50 % | How much distance dulls the treble |
| 9 | Motion | Off / Orbit / Swing / Figure 8 / Spiral / Fly-by | Off | Built-in movement (see above) |
| 10 | Rate | 0.02 … 4 Hz | 0.25 Hz | Cycles per second when Sync is off |
| 11 | Sync | Off / On | Off | Lock the cycle to the tempo |
| 12 | Division | 8 Bars … 1/8 | 1 Bar | Cycle length when synced |
| 13 | Depth | 0 … 100 % | 50 % | Size of the movement |
| 14 | Reverse | Off / On | Off | Move the other way |
| 15–17 | ~Az, ~El, ~Dist | | | Read-only: where the sound is right now (drives the screen pictures; not automatable) |
| 18 | Intensity | 0 … 100 % | 70 % | How strongly the 3D cues are exaggerated (0 % = natural head) |
| 19 | Echo | 0 … 100 % | 0 % | Echo level |
| 20 | Echo Time | 20 … 2000 ms | 375 ms | Time between repeats (Echo Sync off) |
| 21 | Echo Sync | Off / On | Off | Lock the echo time to the tempo |
| 22 | Echo Div | 1/32 … 1 Bar | 1/4 | Echo time when synced |
| 23 | Feedback | 0 … 95 % | 55 % | Repeats |
| 24 | Step | L 180 … None … R 180 | R 90 | How far each repeat turns |
| 25 | Tilt | Down 45 … Level … Up 45 | Level | How far each repeat tips |
| 26 | Damping | 0 … 100 % | 40 % | How fast repeats darken |

Direction and distance glide (about 30 ms) so sweeping a Q-Link is smooth. Azimuth glides the short way round: going from R 170 to L 170 passes behind you, not through the front.

## Screen

- **MAIN**: four tiles that match the four Q-Links of an MPC One, top to bottom: Azimuth, Elevation, Distance, Mix. Each tile shows a 3D wireframe of the sound around a head, the value, and a bar. Tap a tile to bank the Q-Links to it, double-tap for the value editor, or swipe left and right on the bar.
- **MOTION**: two columns in the style of the MAIN tab. **PATH** has Motion, Depth and Reverse, and **SPEED** has Rate, Sync and Division. Each column is one Q-Link bank: tap a control to bank the Q-Links to its column. Choices (Motion, Division) step with the Q-Link or data wheel.
- **ECHO**: **LEVEL** has Echo, Feedback and Damping, **TIME** has Echo Time, Echo Sync and Echo Div, and **TRAVEL** has Step and Tilt.
- **SPACE**: **RENDER** has Mode, Intensity and Width, **ROOM** has Room and Air, and **OUTPUT** has Volume.

## Presets

In the order the MPC lists them:

**Moving, with echoes**
- **Comet Orbit**: circles your head, trailing 1/16 echoes that stay where they were played
- **Swirl Trail**: orbits once a bar while the 1/8 echoes turn the other way
- **Pendulum Echo**: swings side to side over 2 bars; dotted-1/8 echoes bounce front to back
- **Figure 8 Trails**: a figure of eight with a tight trail of 1/16 echoes
- **Spiral Galaxy**: Field mode; spirals up and down while quarter-note echoes turn and tip
- **Fly-by Wake**: passes by and comes back, leaving a darkening wake of 1/8 echoes
- **Counter Spin**: fast half-note spin backwards; the echoes walk forwards in 1/8s
- **Halo**: a tilted orbit above your head with a quick 120 ms shimmer of echoes
- **Orbit Ping-Pong**: slow orbit; quarter-note echoes jump to the opposite side

**Moving**: Slow Orbit, Orbit 1 Bar, Tilted Orbit, Spin 1/4, Swing 2 Bars, Figure 8, Spiral, Fly-by

**Static**: Front & Centre, Hard Right, Behind You, Overhead, Whisper (close, left; Volume −10 dB because the near ear gets nearly all the sound), Far Field, Wide Field, Light Pan

**Static, with echoes**: Circle 1/4 (repeats walk round you), Front-Back Bounce, Tilted Circle, Slow Creep

You can still automate any parameter the usual way on the MPC, for example recording Azimuth moves from a Q-Link. Motion is for steady, repeating movement without automation.

## CPU

Measured on a desktop at 44.1 kHz / 128 frames: Lite ≈ 0.1 %, Head ≈ 0.3 %, Field ≈ 0.5 % of one core. The echoes add about 0.1–0.2 % (in Field mode they share the main decode; in Head and Lite they use a simpler head model without the ear echoes). Expect roughly 10–20 times that on an MPC One. Field is the heaviest (eight virtual speakers) but still light enough to run several instances.

## Limitations

- The head model is generic, not your own ears. Front/back confusion is normal for any non-personalised binaural render, especially with Width at 0 %. Moving the source helps.
- Elevation cues are subtle in all modes and absent in Lite.
- There is no Doppler effect. Distance changes do not shift pitch.

## Build

Builds with the [MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk) from `plugins/Binaural Echo`:

```
python3 tools/build.py "plugins/Binaural Echo"
python3 tools/package.py "plugins/Binaural Echo"
```

The MPC needs MPC OS 3.10.1 or later, with Preferences → Enhancements (Beta) → Enable Enhancements and External Plugin Support turned on.

The screen art is rendered from `art/mockup.html` (the design mockup) by `art/make_skin.py`, which writes `skins/`. Re-run it after changing `ui.json` or the mockup, then rebuild. It needs Google Chrome (headless) and Pillow.

## Project layout

- `plugin.json`: manifest (name, manufacturer, uid). The `uid` must match the C++ `uniqueId`.
- `src/BinauralEcho.cpp`: parameters and DSP.
- `ui.json`: layout of the SPACE tab (and the base the skin script starts from).
- `skins/`: the hand-built Plugin Skins (generated by `art/make_skin.py`).
- `art/`: mockup and skin script.

## Licence

MIT, © Lightlike. See `LICENSE`. The embedded Titillium Web font in `art/mockup.html` is under the SIL Open Font License. The head model follows the equations published by Brown & Duda (1998). No code or data is taken from elsewhere.
