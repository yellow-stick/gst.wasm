# ys_gst_web

Minimal Flutter Web page driving the gst.wasm GES timeline sample
(`ges-timeline-example`). Flutter and the Emscripten module share no memory:
`lib/main.dart` calls `window.ysGst` from `web/ys_gst_adapter.js`
(`start`, `load`, `play`, `pause`, `seekFrame`, `onEvent`), which sets up
`window.Module` like `ges-timeline-example.html` and forwards `ysOnEvent`
events back. `window.ysEvents` / `window.ysWaitFor` are kept for automation.

## Build

```sh
cd flutter/ys_gst_web
flutter build web --release --no-web-resources-cdn
```

`--no-web-resources-cdn` bundles CanvasKit locally: the page is served with
`Cross-Origin-Embedder-Policy: require-corp`, which blocks the default CDN.

## Serve

From the repository root, place the installed sample (plus the test clips
from `tools/make_test_clips.sh build/ys-media`) under `build/web/gst/`:

```sh
GST=flutter/ys_gst_web/build/web/gst
mkdir -p "$GST/media"
cp build/gst.wasm_web_wasm32/dist/web_wasm32/share/gst.wasm/samples/ges-timeline-example/* "$GST/"
cp build/ys-media/clip-a.mp4 build/ys-media/clip-b.mp4 "$GST/media/"
python3 tools/serve_samples.py flutter/ys_gst_web/build/web --port 8090
```

Open <http://127.0.0.1:8090/>, wait for `ready`, then press Load. Another
sample directory can be used with `?gst=<url ending in />`.
