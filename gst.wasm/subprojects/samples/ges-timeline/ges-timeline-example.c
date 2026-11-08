/*
 * GStreamer - gst.wasm GES timeline example
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Loads a GES project (.xges), plays it into a canvas and seeks it frame by
 * frame. The page drives it through the exported ys_* functions and gets
 * JSON events through Module.ysOnEvent.
 *
 * Threads: the GLib main loop runs on the proxied main thread, which owns
 * the #canvas. Every GES call happens there; the exported functions only
 * hand their arguments over with g_main_context_invoke ().
 *
 * Only WebCodecs decoders are registered (no libav), so decodebin can only
 * pick the browser decoders.
 */

#include <stdarg.h>
#include <string.h>

#include <emscripten.h>
#include <emscripten/threading.h>
#include <ges/ges.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/web/gstwebcanvas.h>
#include <gst/web/gstwebutils.h>

GST_DEBUG_CATEGORY_STATIC (example_dbg);
#define GST_CAT_DEFAULT example_dbg

/* Test clips strip layout, see tools/make_test_clips.sh */
#define STRIP_WIDTH 320
#define STRIP_HEIGHT 240
#define STRIP_CELL 20
#define STRIP_BITS 12
#define STRIP_MIN_REFERENCE 40

static GMainLoop *loop;
static GESProject *project;
static GESTimeline *timeline;
static GESPipeline *pipeline;
static gint seek_serial;

static void
register_elements (void)
{
  GST_PLUGIN_STATIC_DECLARE (coreelements);
  GST_PLUGIN_STATIC_DECLARE (playback);
  GST_PLUGIN_STATIC_DECLARE (typefindfunctions);
  GST_PLUGIN_STATIC_DECLARE (isomp4);
  GST_PLUGIN_STATIC_DECLARE (videoparsersbad);
  GST_PLUGIN_STATIC_DECLARE (web);
  GST_PLUGIN_STATIC_DECLARE (compositor);
  GST_PLUGIN_STATIC_DECLARE (videoconvertscale);
  GST_PLUGIN_STATIC_DECLARE (videorate);
  GST_PLUGIN_STATIC_DECLARE (videotestsrc);
  GST_PLUGIN_STATIC_DECLARE (nle);
  GST_PLUGIN_STATIC_DECLARE (ges);
  /* FIXME: Should not be needed. (RDI-2866), as in the codecs example */
  GST_ELEMENT_REGISTER_DECLARE (glimagesink);

  GST_PLUGIN_STATIC_REGISTER (coreelements);
  GST_PLUGIN_STATIC_REGISTER (playback);
  GST_PLUGIN_STATIC_REGISTER (typefindfunctions);
  GST_PLUGIN_STATIC_REGISTER (isomp4);
  GST_PLUGIN_STATIC_REGISTER (videoparsersbad);
  GST_PLUGIN_STATIC_REGISTER (web);
  GST_PLUGIN_STATIC_REGISTER (compositor);
  GST_PLUGIN_STATIC_REGISTER (videoconvertscale);
  GST_PLUGIN_STATIC_REGISTER (videorate);
  GST_PLUGIN_STATIC_REGISTER (videotestsrc);
  GST_PLUGIN_STATIC_REGISTER (nle);
  GST_PLUGIN_STATIC_REGISTER (ges);
  GST_ELEMENT_REGISTER (glimagesink, NULL);
}

/* JSON helpers */

static void
json_append_string (GString *json, const gchar *value)
{
  const gchar *p;

  if (!value) {
    g_string_append (json, "null");
    return;
  }

  g_string_append_c (json, '"');
  for (p = value; *p; p++) {
    switch (*p) {
      case '"':
        g_string_append (json, "\\\"");
        break;
      case '\\':
        g_string_append (json, "\\\\");
        break;
      case '\n':
        g_string_append (json, "\\n");
        break;
      default:
        if ((guchar) *p < 0x20)
          g_string_append_printf (json, "\\u%04x", (guchar) *p);
        else
          g_string_append_c (json, *p);
    }
  }
  g_string_append_c (json, '"');
}

/* Takes ownership of @json */
static void
emit (GString *json)
{
  gchar *text = g_string_free (json, FALSE);

  GST_DEBUG ("event %s", text);
  /* clang-format off */
  MAIN_THREAD_ASYNC_EM_ASM ({
    const text = UTF8ToString ($0);
    _free ($0);
    if (Module.ysOnEvent)
      Module.ysOnEvent (JSON.parse (text));
  }, text);
  /* clang-format on */
}

static GString *
event_new (const gchar *name)
{
  GString *json = g_string_new ("{\"event\":");

  json_append_string (json, name);
  return json;
}

static void
emit_error (const gchar *where, const gchar *message)
{
  GString *json = event_new ("error");

  g_string_append (json, ",\"where\":");
  json_append_string (json, where);
  g_string_append (json, ",\"message\":");
  json_append_string (json, message);
  g_string_append_c (json, '}');
  emit (json);
}

static guint64
timeline_frame_from_time (GstClockTime time)
{
  gint fps_n = 0, fps_d = 1;
  GESTrack *track = NULL;
  GList *tracks = ges_timeline_get_tracks (timeline);
  GstCaps *caps;

  if (tracks)
    track = GES_TRACK (tracks->data);
  caps = track ? (GstCaps *) ges_track_get_restriction_caps (track) : NULL;
  if (caps)
    gst_structure_get_fraction (
        gst_caps_get_structure (caps, 0), "framerate", &fps_n, &fps_d);
  gst_clear_caps (&caps);
  g_list_free_full (tracks, gst_object_unref);

  if (fps_n <= 0)
    return G_MAXUINT64;
  return gst_util_uint64_scale_round (time, fps_n, (guint64) fps_d * GST_SECOND);
}

/* Frame code reader, same algorithm as tools/framecode.py */

static gdouble
strip_cell_luma (GstVideoFrame *frame, gint cell, gint y0, gint y1)
{
  gint width = GST_VIDEO_FRAME_WIDTH (frame);
  gint height = GST_VIDEO_FRAME_HEIGHT (frame);
  const guint8 *data = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  gint stride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0);
  gdouble sum = 0;
  gint count = 0;
  gint x, y;

  for (y = y0; y < y1; y += 4) {
    gint sy = y * height / STRIP_HEIGHT;

    for (x = cell * STRIP_CELL + 5; x < cell * STRIP_CELL + 15; x++) {
      gint sx = x * width / STRIP_WIDTH;
      const guint8 *px = data + sy * stride + sx * 4;

      sum += (px[0] * 299 + px[1] * 587 + px[2] * 114) / 1000;
      count++;
    }
  }
  return sum / count;
}

static void
strip_append (GString *json, const gchar *name, GstVideoFrame *frame,
    gint y0, gint y1)
{
  gdouble reference = strip_cell_luma (frame, 0, y0, y1);
  guint number = 0;
  gint bit;

  g_string_append_printf (json, ",\"%s\":", name);
  if (reference < STRIP_MIN_REFERENCE) {
    g_string_append (json, "null");
    return;
  }

  for (bit = 0; bit < STRIP_BITS; bit++)
    if (strip_cell_luma (frame, bit + 1, y0, y1) > reference / 2)
      number |= 1 << bit;

  g_string_append_printf (json, "{\"frame\":%u,\"alpha\":%.3f}", number,
      reference / 255.0);
}

static GstPadProbeReturn
rendered_frame_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
  GstEvent *segment_event;
  const GstSegment *segment = NULL;
  GstCaps *caps;
  GstVideoInfo vinfo;
  GstVideoFrame frame;
  GstClockTime stream_time = GST_CLOCK_TIME_NONE;
  GString *json;

  caps = gst_pad_get_current_caps (pad);
  if (!caps || !gst_video_info_from_caps (&vinfo, caps) ||
      GST_VIDEO_INFO_FORMAT (&vinfo) != GST_VIDEO_FORMAT_RGBA) {
    gst_clear_caps (&caps);
    return GST_PAD_PROBE_OK;
  }
  gst_caps_unref (caps);

  segment_event = gst_pad_get_sticky_event (pad, GST_EVENT_SEGMENT, 0);
  if (segment_event)
    gst_event_parse_segment (segment_event, &segment);
  if (segment && segment->format == GST_FORMAT_TIME)
    stream_time = gst_segment_to_stream_time (
        segment, GST_FORMAT_TIME, GST_BUFFER_PTS (buffer));

  json = event_new ("frame");
  g_string_append_printf (json,
      ",\"seek\":%d,\"pts\":%" G_GUINT64_FORMAT ",\"streamTime\":%" G_GINT64_FORMAT,
      g_atomic_int_get (&seek_serial), GST_BUFFER_PTS (buffer),
      GST_CLOCK_TIME_IS_VALID (stream_time) ? (gint64) stream_time : -1);
  if (GST_CLOCK_TIME_IS_VALID (stream_time))
    g_string_append_printf (json, ",\"timelineFrame\":%" G_GUINT64_FORMAT,
        timeline_frame_from_time (stream_time));
  if (gst_video_frame_map (&frame, &vinfo, buffer, GST_MAP_READ)) {
    strip_append (json, "a", &frame, 4, 28);
    strip_append (json, "b", &frame, 212, 236);
    gst_video_frame_unmap (&frame);
  }
  g_string_append_c (json, '}');
  emit (json);

  if (segment_event)
    gst_event_unref (segment_event);

  return GST_PAD_PROBE_OK;
}

/* Reports which decoders the timeline instantiated */
static void
on_deep_element_added (GstBin *bin, GstBin *sub_bin, GstElement *element,
    gpointer user_data)
{
  GstElementFactory *factory = gst_element_get_factory (element);
  const gchar *klass;
  GString *json;

  if (!factory)
    return;
  klass = gst_element_factory_get_metadata (
      factory, GST_ELEMENT_METADATA_KLASS);
  if (!klass || !strstr (klass, "Decoder"))
    return;

  json = event_new ("decoder");
  g_string_append (json, ",\"factory\":");
  json_append_string (json, GST_OBJECT_NAME (factory));
  g_string_append (json, ",\"klass\":");
  json_append_string (json, klass);
  g_string_append_c (json, '}');
  emit (json);
}

static gboolean
on_bus_message (GstBus *bus, GstMessage *message, gpointer user_data)
{
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:{
      GError *err = NULL;
      gchar *debug = NULL;
      gchar *text;

      gst_message_parse_error (message, &err, &debug);
      text = g_strdup_printf ("%s (%s): %s", err->message,
          GST_OBJECT_NAME (GST_MESSAGE_SRC (message)), debug ? debug : "");
      emit_error ("pipeline", text);
      g_free (text);
      g_clear_error (&err);
      g_free (debug);
      break;
    }
    case GST_MESSAGE_ASYNC_DONE:{
      GString *json = event_new ("async-done");
      gint64 position = -1;

      gst_element_query_position (
          GST_ELEMENT (pipeline), GST_FORMAT_TIME, &position);
      g_string_append_printf (json,
          ",\"seek\":%d,\"position\":%" G_GINT64_FORMAT "}",
          g_atomic_int_get (&seek_serial), position);
      emit (json);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (message) == GST_OBJECT (pipeline)) {
        GstState state;
        GString *json = event_new ("state");

        gst_message_parse_state_changed (message, NULL, &state, NULL);
        g_string_append (json, ",\"state\":");
        json_append_string (json, gst_element_state_get_name (state));
        g_string_append_c (json, '}');
        emit (json);
      }
      break;
    case GST_MESSAGE_EOS:
      emit (g_string_append (event_new ("eos"), "}"));
      break;
    default:
      break;
  }

  return G_SOURCE_CONTINUE;
}

static void
append_timeline_description (GString *json)
{
  GList *layers, *l;
  gboolean first = TRUE;

  g_string_append_printf (json,
      ",\"duration\":%" G_GUINT64_FORMAT ",\"clips\":[",
      ges_timeline_get_duration (timeline));

  layers = ges_timeline_get_layers (timeline);
  for (l = layers; l; l = l->next) {
    GList *clips = ges_layer_get_clips (GES_LAYER (l->data)), *c;

    for (c = clips; c; c = c->next) {
      GESTimelineElement *element = GES_TIMELINE_ELEMENT (c->data);
      GESAsset *asset = ges_extractable_get_asset (GES_EXTRACTABLE (element));

      if (!first)
        g_string_append_c (json, ',');
      first = FALSE;
      g_string_append (json, "{\"name\":");
      json_append_string (json, ges_timeline_element_get_name (element));
      g_string_append (json, ",\"type\":");
      json_append_string (json, G_OBJECT_TYPE_NAME (element));
      g_string_append (json, ",\"asset\":");
      json_append_string (json, asset ? ges_asset_get_id (asset) : NULL);
      g_string_append_printf (json,
          ",\"layer\":%u,\"start\":%" G_GUINT64_FORMAT
          ",\"inpoint\":%" G_GUINT64_FORMAT ",\"duration\":%" G_GUINT64_FORMAT
          "}",
          ges_layer_get_priority (GES_LAYER (l->data)),
          GES_TIMELINE_ELEMENT_START (element),
          GES_TIMELINE_ELEMENT_INPOINT (element),
          GES_TIMELINE_ELEMENT_DURATION (element));
    }
    g_list_free_full (clips, gst_object_unref);
  }
  g_list_free_full (layers, gst_object_unref);
  g_string_append_c (json, ']');
}

static GstElement *
create_video_sink (void)
{
  GError *err = NULL;
  GstElement *bin, *sink;
  GstPad *pad;

  bin = gst_parse_bin_from_description (
      "videoconvert ! video/x-raw,format=RGBA ! webcanvassink name=canvassink",
      TRUE, &err);
  if (!bin) {
    emit_error ("video-sink", err->message);
    g_clear_error (&err);
    return NULL;
  }

  sink = gst_bin_get_by_name (GST_BIN (bin), "canvassink");
  pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (
      pad, GST_PAD_PROBE_TYPE_BUFFER, rendered_frame_probe, NULL, NULL);
  gst_object_unref (pad);
  gst_object_unref (sink);

  return bin;
}

/* Share the #canvas runner with every web element of the pipeline, so the
 * WebCodecs decoders never create a runner without the canvas first */
static void
set_canvas_context (GstElement *element)
{
  GstWebCanvas *canvas = gst_web_canvas_new ("#canvas");
  GstContext *context = gst_context_new (GST_WEB_CANVAS_CONTEXT_TYPE, TRUE);

  gst_web_utils_context_set_web_canvas (context, canvas);
  gst_element_set_context (element, context);
  gst_context_unref (context);
  gst_object_unref (canvas);
}

static void
on_project_loaded (GESProject *loaded_project, GESTimeline *loaded_timeline,
    gpointer user_data)
{
  GstElement *video_sink;
  GstBus *bus;
  GString *json;

  json = event_new ("loaded");
  append_timeline_description (json);
  g_string_append_c (json, '}');
  emit (json);

  pipeline = ges_pipeline_new ();
  g_signal_connect (pipeline, "deep-element-added",
      G_CALLBACK (on_deep_element_added), NULL);
  set_canvas_context (GST_ELEMENT (pipeline));

  video_sink = create_video_sink ();
  if (!video_sink)
    return;
  ges_pipeline_preview_set_video_sink (pipeline, video_sink);
  if (!ges_pipeline_set_mode (pipeline, GES_PIPELINE_MODE_PREVIEW_VIDEO) ||
      !ges_pipeline_set_timeline (pipeline, gst_object_ref (timeline))) {
    emit_error ("pipeline", "Could not attach the timeline");
    return;
  }

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_watch (bus, on_bus_message, NULL);
  gst_object_unref (bus);

  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PAUSED);
}

static void
on_project_error_loading (GESProject *failed_project,
    GESTimeline *failed_timeline, GError *error, gpointer user_data)
{
  emit_error ("project", error->message);
}

static void
on_project_error_loading_asset (GESProject *failed_project, GError *error,
    gchar *id, GType extractable_type, gpointer user_data)
{
  gchar *text = g_strdup_printf ("%s: %s", id, error->message);

  emit_error ("asset", text);
  g_free (text);
}

static gboolean
do_load (gpointer data)
{
  const gchar *uri = data;
  GError *err = NULL;

  if (pipeline) {
    gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
    gst_clear_object (&pipeline);
  }
  gst_clear_object (&timeline);
  gst_clear_object (&project);

  GST_INFO ("Loading %s", uri);
  project = ges_project_new (uri);
  g_signal_connect (project, "loaded", G_CALLBACK (on_project_loaded), NULL);
  g_signal_connect (project, "error-loading",
      G_CALLBACK (on_project_error_loading), NULL);
  g_signal_connect (project, "error-loading-asset",
      G_CALLBACK (on_project_error_loading_asset), NULL);

  /* Loading is asynchronous; "loaded" fires once assets are discovered */
  timeline = (GESTimeline *) ges_asset_extract (GES_ASSET (project), &err);
  if (!timeline) {
    emit_error ("project", err ? err->message : "extract failed");
    g_clear_error (&err);
    return G_SOURCE_REMOVE;
  }
  gst_object_ref_sink (timeline);

  return G_SOURCE_REMOVE;
}

static gboolean
do_set_state (gpointer data)
{
  if (pipeline)
    gst_element_set_state (GST_ELEMENT (pipeline), GPOINTER_TO_INT (data));
  else
    emit_error ("state", "No timeline loaded");
  return G_SOURCE_REMOVE;
}

static gboolean
do_seek_frame (gpointer data)
{
  guint64 frame = GPOINTER_TO_UINT (data);
  GstClockTime position;
  GString *json;
  gint serial;

  if (!pipeline) {
    emit_error ("seek", "No timeline loaded");
    return G_SOURCE_REMOVE;
  }

  position = ges_timeline_get_frame_time (timeline, frame);
  serial = g_atomic_int_add (&seek_serial, 1) + 1;

  json = event_new ("seek");
  g_string_append_printf (json,
      ",\"seek\":%d,\"frame\":%" G_GUINT64_FORMAT
      ",\"position\":%" G_GUINT64_FORMAT "}",
      serial, frame, position);
  emit (json);

  if (!gst_element_seek_simple (GST_ELEMENT (pipeline), GST_FORMAT_TIME,
          GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, position))
    emit_error ("seek", "Seek failed");

  return G_SOURCE_REMOVE;
}

/* Exported to the page; called on the browser main thread */

EMSCRIPTEN_KEEPALIVE void
ys_load (const char *uri)
{
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT, do_load,
      g_strdup (uri), g_free);
}

EMSCRIPTEN_KEEPALIVE void
ys_play (void)
{
  g_main_context_invoke (
      NULL, do_set_state, GINT_TO_POINTER (GST_STATE_PLAYING));
}

EMSCRIPTEN_KEEPALIVE void
ys_pause (void)
{
  g_main_context_invoke (
      NULL, do_set_state, GINT_TO_POINTER (GST_STATE_PAUSED));
}

EMSCRIPTEN_KEEPALIVE void
ys_seek_frame (int frame)
{
  g_main_context_invoke (NULL, do_seek_frame, GUINT_TO_POINTER (frame));
}

int
main (int argc, char **argv)
{
  GString *json;

  gst_init (NULL, NULL);
  GST_DEBUG_CATEGORY_INIT (example_dbg, "example", 0, "GES timeline example");
  gst_debug_set_threshold_from_string ("example:4,*:2", TRUE);

  register_elements ();
  if (!ges_init ()) {
    emit_error ("init", "ges_init () failed");
    return 1;
  }

  json = event_new ("ready");
  g_string_append (json, ",\"ges\":");
  {
    guint major, minor, micro, nano;
    gchar *version;

    ges_version (&major, &minor, &micro, &nano);
    version = g_strdup_printf ("%u.%u.%u", major, minor, micro);
    json_append_string (json, version);
    g_free (version);
  }
  g_string_append_c (json, '}');
  emit (json);

  /* Default main context, owned by this thread while the loop runs */
  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  return 0;
}
