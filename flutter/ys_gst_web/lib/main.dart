import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:ui_web' as ui_web;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:web/web.dart' as web;

/// `window.ysGst`, defined by web/ys_gst_adapter.js. Flutter and the
/// Emscripten module share no memory; everything goes through these calls
/// and the event objects passed to [onEvent].
@JS('ysGst')
external YsGst get ysGst;

extension type YsGst._(JSObject _) implements JSObject {
  external void start(web.HTMLCanvasElement canvas, String baseUrl);
  external void load();
  external void play();
  external void pause();
  external void seekFrame(int frame);
  external void onEvent(JSFunction callback);
}

@JS('JSON.stringify')
external JSString _jsonStringify(JSAny? value);

const _canvasViewType = 'ys-gst-canvas';

/// The `<canvas id="canvas">` handed to Emscripten as `Module.canvas`.
final web.HTMLCanvasElement _canvas = web.HTMLCanvasElement()
  ..id = 'canvas'
  ..width = 320
  ..height = 240;

void main() {
  _canvas.style
    ..width = '100%'
    ..height = '100%'
    ..background = '#000';
  ui_web.platformViewRegistry.registerViewFactory(
    _canvasViewType,
    (int viewId) => _canvas,
  );
  runApp(const MaterialApp(home: GstPage()));
}

class GstPage extends StatefulWidget {
  const GstPage({super.key});

  @override
  State<GstPage> createState() => _GstPageState();
}

class _GstPageState extends State<GstPage> {
  final _frameField = TextEditingController(text: '0');
  String _lastEvent = 'starting…';
  String _frameInfo = '-';
  bool _started = false;

  @override
  void initState() {
    super.initState();
    ysGst.onEvent(_onEvent.toJS);
  }

  @override
  void dispose() {
    _frameField.dispose();
    super.dispose();
  }

  // Emscripten transfers Module.canvas to the GStreamer thread at startup,
  // so start only once the platform view element is in the document.
  void _startWhenAttached() {
    if (_started) return;
    if (!_canvas.isConnected) {
      SchedulerBinding.instance.addPostFrameCallback((_) => _startWhenAttached());
      SchedulerBinding.instance.scheduleFrame();
      return;
    }
    _started = true;
    final baseUrl = Uri.base.queryParameters['gst'] ?? 'gst/';
    ysGst.start(_canvas, baseUrl.endsWith('/') ? baseUrl : '$baseUrl/');
  }

  void _onEvent(JSObject event) {
    final name = event.getProperty<JSString?>('event'.toJS)?.toDart;
    if (name == 'frame') {
      final timelineFrame = event.getProperty<JSAny?>('timelineFrame'.toJS);
      setState(() {
        _frameInfo = 'timelineFrame=${timelineFrame.dartify()} '
            'a=${_layerFrame(event, 'a')} b=${_layerFrame(event, 'b')}';
      });
    } else {
      final text = _jsonStringify(event).toDart;
      setState(() => _lastEvent = text);
    }
  }

  static String _layerFrame(JSObject event, String layer) {
    final value = event.getProperty<JSAny?>(layer.toJS);
    if (value == null || value.isUndefinedOrNull) return 'null';
    return '${(value as JSObject).getProperty<JSAny?>('frame'.toJS).dartify()}';
  }

  void _run(void Function() command) {
    try {
      command();
    } catch (error) {
      setState(() => _lastEvent = 'command failed: $error');
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            SizedBox(
              width: 320,
              height: 240,
              child: HtmlElementView(
                viewType: _canvasViewType,
                onPlatformViewCreated: (_) => _startWhenAttached(),
              ),
            ),
            const SizedBox(height: 8),
            Row(
              children: [
                ElevatedButton(
                  onPressed: () => _run(() => ysGst.load()),
                  child: const Text('Load'),
                ),
                const SizedBox(width: 8),
                ElevatedButton(
                  onPressed: () => _run(() => ysGst.play()),
                  child: const Text('Play'),
                ),
                const SizedBox(width: 8),
                ElevatedButton(
                  onPressed: () => _run(() => ysGst.pause()),
                  child: const Text('Pause'),
                ),
                const SizedBox(width: 8),
                SizedBox(
                  width: 80,
                  child: TextField(
                    controller: _frameField,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(labelText: 'Frame'),
                  ),
                ),
                const SizedBox(width: 8),
                ElevatedButton(
                  onPressed: () => _run(() {
                    ysGst.seekFrame(int.tryParse(_frameField.text) ?? 0);
                  }),
                  child: const Text('Seek'),
                ),
              ],
            ),
            const SizedBox(height: 8),
            Text('Event: $_lastEvent'),
            Text('Frame: $_frameInfo'),
          ],
        ),
      ),
    );
  }
}
