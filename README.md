# Binaural

An insert effect plugin for Akai MPC standalone hardware, by Lightlike.

_Describe what the plugin does here. This file is included in the distributable zip._

## Build

```
python tools/build.py plugins/Binaural
python tools/package.py plugins/Binaural
```

## Project layout

- `plugin.json` - manifest (name, manufacturer, uid). The `uid` must match the C++ `uniqueId`.
- `src/Binaural.cpp` - the plugin source (parameters and DSP).
- `ui.json` - which parameters appear on the MPC screen, in which order.
- `build/` - build output (the deployable `Lightlike - VST - Binaural` folder lives here).
- `dist/` - zips produced by `package.py`.
