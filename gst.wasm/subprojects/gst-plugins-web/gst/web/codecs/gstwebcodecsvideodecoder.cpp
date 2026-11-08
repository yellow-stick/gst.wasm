/*
 * GStreamer - gst.wasm WebCodecsVideoDecoder source
 *
 * Copyright 2024 Fluendo S.A.
 * @author: Jorge Zapata <jzapata@fluendo.com>
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
 * Some ideas:
 * Implement the GST_QUERY_ALLOCATOR (propose_allocation) to
 * use buffers that can be sent directly to the decoder (JS)
 * without having to copy the data
 *
 * Implement the negotiate_pool to ask for specific downstream
 * buffers, i.e GL
 *
 * Some interesting links:
 * https://github.com/emscripten-core/emscripten/issues/3515
 * https://emscripten.org/docs/api_reference/val.h.html
 * https://github.com/emscripten-core/emscripten/issues/4927
 * https://github.com/emscripten-core/emscripten/issues/7025
 * https://github.com/w3c/webcodecs/issues/88
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/gl/gl.h>
#include <gst/pbutils/pbutils.h>
#include <emscripten.h>
#include <emscripten/bind.h>
#include <gst/web/gstwebutils.h>
#include <gst/web/gstwebvideoframe.h>

#include "gstwebcodecs.h"
#include "gstwebcodecsvideodecoder.h"

using namespace emscripten;

#define GST_WEB_CODECS_VIDEO_DECODER_MAX_DEQUEUE 32

#define GST_CAT_DEFAULT gst_web_codecs_video_decoder_debug_category
GST_DEBUG_CATEGORY_STATIC (gst_web_codecs_video_decoder_debug_category);

static gpointer parent_class = NULL;

typedef struct _GstWebCodecsVideoDecoderConfigureData
{
  GstWebCodecsVideoDecoder *self;
  GstVideoCodecState *state;
  gboolean ret;
} GstWebCodecsVideoDecoderConfigureData;

typedef struct _GstWebCodecsVideoDecoderDecodeData
{
  GstWebCodecsVideoDecoder *self;
  GstVideoCodecFrame *frame;
} GstWebCodecsVideoDecoderDecodeData;

/* A decoded VideoFrame, wrapped on the runner thread and waiting to be
 * matched with its GstVideoCodecFrame on the streaming thread */
typedef struct _GstWebCodecsVideoDecoderOutput
{
  gint64 timestamp;
  GstVideoFormat format;
  gint width;
  gint height;
  GstWebVideoFrame *memory;
} GstWebCodecsVideoDecoderOutput;

/* WebCodecs timestamps are microseconds. The same value identifies the
 * GstVideoCodecFrame a VideoFrame was decoded from: outputs come in
 * presentation order, the frame list is in decode order. */
static gint64
gst_web_codecs_video_decoder_frame_timestamp (GstVideoCodecFrame *frame)
{
  GstClockTime ts =
      GST_CLOCK_TIME_IS_VALID (frame->pts) ? frame->pts : frame->dts;

  return GST_CLOCK_TIME_IS_VALID (ts) ? (gint64) GST_TIME_AS_USECONDS (ts)
                                      : -1;
}

static void
gst_web_codecs_video_decoder_output_free (GstWebCodecsVideoDecoderOutput *out)
{
  /* Freeing the memory closes the VideoFrame on its runner */
  if (out->memory)
    gst_memory_unref (GST_MEMORY_CAST (out->memory));
  g_free (out);
}

static void
gst_web_codecs_video_decoder_clear_outputs (GstWebCodecsVideoDecoder *self)
{
  GQueue outputs = G_QUEUE_INIT;

  g_mutex_lock (&self->output_lock);
  outputs = self->output_queue;
  g_queue_init (&self->output_queue);
  g_mutex_unlock (&self->output_lock);

  g_queue_clear_full (
      &outputs, (GDestroyNotify) gst_web_codecs_video_decoder_output_free);
}

static gboolean
gst_web_codecs_video_decoder_parse_format (
    const std::string &vf_format, GstVideoFormat *format)
{
  if (vf_format == "I420")
    *format = GST_VIDEO_FORMAT_I420;
  else if (vf_format == "I420A")
    *format = GST_VIDEO_FORMAT_A420;
  else if (vf_format == "I422")
    *format = GST_VIDEO_FORMAT_Y42B;
  else if (vf_format == "I444")
    *format = GST_VIDEO_FORMAT_Y444;
  else if (vf_format == "NV12")
    *format = GST_VIDEO_FORMAT_NV12;
  else if (vf_format == "RGBA")
    *format = GST_VIDEO_FORMAT_RGBA;
  else if (vf_format == "RGBX")
    *format = GST_VIDEO_FORMAT_RGBx;
  else if (vf_format == "BGRA")
    *format = GST_VIDEO_FORMAT_BGRA;
  else if (vf_format == "BGRX")
    *format = GST_VIDEO_FORMAT_BGRx;
  else
    return FALSE;

  return TRUE;
}

/* Runner thread. Only wraps and queues the VideoFrame: pushing downstream
 * from here would block the runner, which every web element of the
 * pipeline shares, while e.g. a compositor waits for another decoder that
 * also needs the runner. */
static void
gst_web_codecs_video_decoder_on_output (guintptr self_, val video_frame)
{
  GstWebCodecsVideoDecoder *self = (GstWebCodecsVideoDecoder *) self_;
  GstWebCodecsVideoDecoderOutput *out;
  GstWebRunner *runner;
  std::string vf_format;

  out = g_new0 (GstWebCodecsVideoDecoderOutput, 1);
  out->timestamp = (gint64) video_frame["timestamp"].as<double> ();
  out->width = video_frame["displayWidth"].as<int> ();
  out->height = video_frame["displayHeight"].as<int> ();

  /* The format is null for frames the browser can not expose as pixels */
  if (video_frame["format"].isNull () ||
      !gst_web_codecs_video_decoder_parse_format (
          vf_format = video_frame["format"].as<std::string> (),
          &out->format)) {
    GST_ERROR_OBJECT (self, "Unsupported VideoFrame format '%s'",
        vf_format.c_str ());
    video_frame.call<void> ("close");
    g_free (out);
    return;
  }

  runner = gst_web_canvas_get_runner (self->canvas);
  out->memory = gst_web_video_frame_wrap (video_frame, runner);
  gst_object_unref (runner);

  GST_LOG_OBJECT (self, "VideoFrame %s %dx%d at %" G_GINT64_FORMAT " us",
      vf_format.c_str (), out->width, out->height, out->timestamp);

  g_mutex_lock (&self->output_lock);
  g_queue_push_tail (&self->output_queue, out);
  g_mutex_unlock (&self->output_lock);
}

static void
gst_web_codecs_video_decoder_on_error (guintptr self_, val error)
{
  GstWebCodecsVideoDecoder *self = (GstWebCodecsVideoDecoder *) self_;
  std::string message = error["message"].as<std::string> ();

  GST_ERROR_OBJECT (self, "WebCodecs error: %s", message.c_str ());

  g_mutex_lock (&self->output_lock);
  if (!self->error)
    self->error = g_strdup (message.c_str ());
  g_mutex_unlock (&self->output_lock);

  /* Do not leave handle_frame () waiting for a dequeue that never comes */
  g_mutex_lock (&self->dequeue_lock);
  self->dequeue_size = 0;
  g_cond_signal (&self->dequeue_cond);
  g_mutex_unlock (&self->dequeue_lock);
}

static GstFlowReturn
gst_web_codecs_video_decoder_check_error (GstWebCodecsVideoDecoder *self)
{
  gchar *error;

  g_mutex_lock (&self->output_lock);
  error = g_strdup (self->error);
  g_mutex_unlock (&self->output_lock);

  if (!error)
    return GST_FLOW_OK;

  GST_ELEMENT_ERROR (self, STREAM, DECODE, ("WebCodecs decoding failed"),
      ("%s", error));
  g_free (error);
  return GST_FLOW_ERROR;
}

static GstVideoCodecFrame *
gst_web_codecs_video_decoder_find_frame (
    GstWebCodecsVideoDecoder *self, gint64 timestamp)
{
  GstVideoCodecFrame *found = NULL;
  GList *frames, *l;

  frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (self));
  for (l = frames; l; l = l->next) {
    GstVideoCodecFrame *frame = (GstVideoCodecFrame *) l->data;

    if (gst_web_codecs_video_decoder_frame_timestamp (frame) == timestamp) {
      found = gst_video_codec_frame_ref (frame);
      break;
    }
  }
  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);

  return found;
}

/* Called with the stream lock taken */
static GstFlowReturn
gst_web_codecs_video_decoder_finish_output (
    GstWebCodecsVideoDecoder *self, GstWebCodecsVideoDecoderOutput *out)
{
  GstVideoDecoder *dec = GST_VIDEO_DECODER (self);
  GstVideoCodecFrame *frame;
  GstFlowReturn ret;

  if (!self->output_state || out->format != self->format ||
      out->width != self->width || out->height != self->height) {
    self->format = out->format;
    self->width = out->width;
    self->height = out->height;
    self->need_negotiation = TRUE;
    if (!gst_video_decoder_negotiate (dec)) {
      gst_web_codecs_video_decoder_output_free (out);
      return GST_FLOW_NOT_NEGOTIATED;
    }
  }

  frame = gst_web_codecs_video_decoder_find_frame (self, out->timestamp);
  if (!frame) {
    /* Decoded before a flush; its codec frame is gone */
    GST_DEBUG_OBJECT (self, "No pending frame for %" G_GINT64_FORMAT " us",
        out->timestamp);
    gst_web_codecs_video_decoder_output_free (out);
    return GST_FLOW_OK;
  }

  if (self->system_memory) {
    GstMapInfo map;
    gboolean copied;

    ret = gst_video_decoder_allocate_output_frame (dec, frame);
    if (ret != GST_FLOW_OK) {
      gst_video_decoder_release_frame (dec, frame);
      gst_web_codecs_video_decoder_output_free (out);
      return ret;
    }
    if (!gst_buffer_map (frame->output_buffer, &map, GST_MAP_WRITE)) {
      GST_ELEMENT_ERROR (self, RESOURCE, WRITE, (NULL),
          ("Failed to map the output buffer"));
      gst_video_decoder_release_frame (dec, frame);
      gst_web_codecs_video_decoder_output_free (out);
      return GST_FLOW_ERROR;
    }
    copied = gst_web_video_frame_copy_to (
        out->memory, &self->output_state->info, map.data, map.size);
    gst_buffer_unmap (frame->output_buffer, &map);
    if (!copied) {
      GST_ELEMENT_ERROR (self, STREAM, DECODE, (NULL),
          ("Failed to copy the VideoFrame to system memory"));
      gst_video_decoder_release_frame (dec, frame);
      gst_web_codecs_video_decoder_output_free (out);
      return GST_FLOW_ERROR;
    }
  } else {
    GstBuffer *b = gst_buffer_new ();

    gst_buffer_insert_memory (
        b, -1, gst_memory_ref (GST_MEMORY_CAST (out->memory)));
    frame->output_buffer = b;
  }
  gst_web_codecs_video_decoder_output_free (out);

  return gst_video_decoder_finish_frame (dec, frame);
}

/* Called with the stream lock taken */
static GstFlowReturn
gst_web_codecs_video_decoder_push_outputs (GstWebCodecsVideoDecoder *self)
{
  GstFlowReturn ret = gst_web_codecs_video_decoder_check_error (self);

  while (TRUE) {
    GstWebCodecsVideoDecoderOutput *out;

    g_mutex_lock (&self->output_lock);
    out = (GstWebCodecsVideoDecoderOutput *) g_queue_pop_head (
        &self->output_queue);
    g_mutex_unlock (&self->output_lock);
    if (!out)
      break;

    if (ret == GST_FLOW_OK)
      ret = gst_web_codecs_video_decoder_finish_output (self, out);
    else
      gst_web_codecs_video_decoder_output_free (out);
  }

  return ret;
}

static void
gst_web_codecs_video_decoder_on_dequeue (guintptr self_, val event)
{
  GstWebCodecsVideoDecoder *self = (GstWebCodecsVideoDecoder *) self_;
  gint dequeue_size;

  dequeue_size = self->decoder["decodeQueueSize"].as<int> ();

  GST_INFO_OBJECT (
      self, "Dequeue received with current size %d", dequeue_size);
  g_mutex_lock (&self->dequeue_lock);
  self->dequeue_size = dequeue_size;
  g_cond_signal (&self->dequeue_cond);
  g_mutex_unlock (&self->dequeue_lock);
  GST_DEBUG_OBJECT (self, "Handle frame notified");
}

EMSCRIPTEN_BINDINGS (gst_web_codecs_video_decoder)
{
  function ("gst_web_codecs_video_decoder_on_output",
      &gst_web_codecs_video_decoder_on_output);
  function ("gst_web_codecs_video_decoder_on_error",
      &gst_web_codecs_video_decoder_on_error);
  function ("gst_web_codecs_video_decoder_on_dequeue",
      &gst_web_codecs_video_decoder_on_dequeue);
}

static void
gst_web_codecs_video_decoder_decode (gpointer data)
{
  GstWebCodecsVideoDecoderDecodeData *decode_data =
      (GstWebCodecsVideoDecoderDecodeData *) data;
  GstWebCodecsVideoDecoder *self = decode_data->self;
  GstVideoCodecFrame *frame = decode_data->frame;
  GstMapInfo map;
  val chunkclass = val::global ("EncodedVideoChunk");
  val options = val::object ();

  GST_DEBUG_OBJECT (self,
      "Decoding frame at %" GST_TIME_FORMAT " with duration %" GST_TIME_FORMAT,
      GST_TIME_ARGS (frame->pts), GST_TIME_ARGS (frame->duration));

  /* WebCodecs requires a key frame after configure () or reset (). A delta
   * here can only be a frame queued before a flush: decoding it would put
   * the VideoDecoder in the closed state. */
  if (self->key_required && !GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame)) {
    GST_DEBUG_OBJECT (self, "Skipping delta frame while a key is required");
    gst_video_codec_frame_unref (frame);
    return;
  }
  self->key_required = FALSE;

  /* TODO use 'transfer' option in case we provide an allocator to avoid
   * the copy. For that, we need to create the memory in JS and give the
   * ownership to it.
   */
  options.set ("timestamp",
      (double) gst_web_codecs_video_decoder_frame_timestamp (frame));
  if (GST_CLOCK_TIME_IS_VALID (frame->duration))
    options.set (
        "duration", (double) GST_TIME_AS_USECONDS (frame->duration));
  options.set (
      "type", GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame) ? "key" : "delta");
  gst_buffer_map (frame->input_buffer, &map, GST_MAP_READ);
  val buffer_data = val (typed_memory_view (map.size, map.data));
  options.set ("data", buffer_data);

  val chunk = chunkclass.new_ (options);
  self->decoder.call<void> ("decode", chunk);
  gst_buffer_unmap (frame->input_buffer, &map);

  gst_video_codec_frame_unref (frame);
  GST_DEBUG_OBJECT (self, "Done decoding");
}

static void
gst_web_codecs_video_decoder_configure (gpointer data)
{
  GstWebCodecsVideoDecoderConfigureData *conf_data =
      (GstWebCodecsVideoDecoderConfigureData *) data;
  GstWebCodecsVideoDecoder *self = conf_data->self;
  GstVideoCodecState *state = conf_data->state;
  GstStructure *s;
  const GValue *codec_data_value = NULL;
  GstBuffer *codec_data = NULL;
  GstMapInfo map;
  gchar *mime_codec;
  val config = val::object ();

  mime_codec = gst_codec_utils_caps_get_mime_codec (state->caps);
  config.set ("codec", std::string (mime_codec));
  g_free (mime_codec);

  s = gst_caps_get_structure (state->caps, 0);
  codec_data_value = gst_structure_get_value (s, "codec_data");
  if (!codec_data_value) {
    GST_ERROR_OBJECT (self, "Caps do not have codec_data");
    conf_data->ret = FALSE;
    return;
  }

  codec_data = gst_value_get_buffer (codec_data_value);
  if (!gst_buffer_map (codec_data, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "Impossible to map the buffer");
    conf_data->ret = FALSE;
    return;
  }

  val codec_data_js = val (typed_memory_view (map.size, map.data));
  config.set ("description", codec_data_js);

  GST_DEBUG_OBJECT (self, "Setting format");
  self->decoder.call<void> ("configure", config);
  gst_buffer_unmap (codec_data, &map);
  self->key_required = TRUE;
  conf_data->ret = TRUE;
}

/* Runner thread */
static void
gst_web_codecs_video_decoder_close_decoder (gpointer data)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (data);

  if (!self->has_decoder)
    return;
  if (self->decoder["state"].as<std::string> () != "closed")
    self->decoder.call<void> ("close");
  self->decoder = val::undefined ();
  self->has_decoder = FALSE;
}

static void
gst_web_codecs_video_decoder_ctor (gpointer data)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (data);
  val vdecclass = val::global ("VideoDecoder");
  val mod = val::global ("Module");
  val options = val::object ();

  /* New caps: drop the previous VideoDecoder */
  gst_web_codecs_video_decoder_close_decoder (self);

  /* clang-format off */
  EM_ASM ({
    const self = $0;
    const options = Emval.toValue ($1);
    options["output"] = (data) => {
      Module.gst_web_codecs_video_decoder_on_output (self, data);
    };
    options["error"] = (e) => {
      Module.gst_web_codecs_video_decoder_on_error (self, e);
    }
  }, (guintptr) self, options.as_handle ());
  /* clang-format on */

  self->decoder = vdecclass.new_ (options);
  self->has_decoder = TRUE;

  /* clang-format off */
  EM_ASM ({
    const self = $0;
    const decoder = Emval.toValue ($1);

    decoder.addEventListener ("dequeue", (event) => {
      Module.gst_web_codecs_video_decoder_on_dequeue (self, event);
    });
  }, (guintptr) self, self->decoder.as_handle ());
  /* clang-format on */

  GST_DEBUG_OBJECT (self, "decoder created successfully");
}

/* Called with the streaming lock taken */
static gboolean
gst_web_codecs_video_decoder_negotiate (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstCaps *peer_caps;
  GstCaps *caps;

  /* If we don't know the output format yet, skip this */
  if (!self->need_negotiation)
    return TRUE;

  self->need_negotiation = FALSE;

  if (self->output_state) {
    gst_video_codec_state_unref (self->output_state);
    self->output_state = NULL;
  }

  self->output_state = gst_video_decoder_set_output_state (
      decoder, self->format, self->width, self->height, self->input_state);

  /* Keep VideoFrames as memory only when downstream prefers them, e.g. a
   * webcanvassink linked directly. Otherwise (unlinked decodebin pads,
   * converters, mixers) copy them to system memory. */
  caps = gst_video_info_to_caps (&self->output_state->info);
  peer_caps =
      gst_pad_peer_query_caps (GST_VIDEO_DECODER_SRC_PAD (decoder), NULL);
  self->system_memory = TRUE;
  if (peer_caps && !gst_caps_is_any (peer_caps) &&
      !gst_caps_is_empty (peer_caps)) {
    GstCapsFeatures *features = gst_caps_get_features (peer_caps, 0);

    self->system_memory = !(features &&
        gst_caps_features_contains (
            features, GST_CAPS_FEATURE_MEMORY_WEB_VIDEO_FRAME));
  }
  gst_clear_caps (&peer_caps);

  if (!self->system_memory)
    gst_caps_set_features_simple (caps,
        gst_caps_features_new (
            GST_CAPS_FEATURE_MEMORY_WEB_VIDEO_FRAME, (char *) NULL));
  GST_INFO_OBJECT (self, "Output caps %" GST_PTR_FORMAT, caps);

  gst_clear_caps (&self->output_state->caps);
  self->output_state->caps = caps;

  return GST_VIDEO_DECODER_CLASS (parent_class)->negotiate (decoder);
}

static GstFlowReturn
gst_web_codecs_video_decoder_handle_frame (
    GstVideoDecoder *decoder, GstVideoCodecFrame *frame)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebCodecsVideoDecoderDecodeData *decode_data;
  GstWebRunner *runner;
  GstFlowReturn res;
  guint epoch;

  GST_DEBUG_OBJECT (decoder, "Handling frame");
  /* Wait until there is nothing pending to be to dequeued or there is a buffer
   */
  epoch = self->flush_epoch;
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  g_mutex_lock (&self->dequeue_lock);
  while (self->dequeue_size >= GST_WEB_CODECS_VIDEO_DECODER_MAX_DEQUEUE) {
    GST_DEBUG_OBJECT (self, "Reached queue limit [%d/%d], waiting for dequeue",
        self->dequeue_size, GST_WEB_CODECS_VIDEO_DECODER_MAX_DEQUEUE);
    g_cond_wait (&self->dequeue_cond, &self->dequeue_lock);
  }
  self->dequeue_size++;
  g_mutex_unlock (&self->dequeue_lock);
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  /* A flush happened while waiting: this frame belongs to the old segment */
  if (epoch != self->flush_epoch) {
    GST_DEBUG_OBJECT (self, "Dropping frame submitted before a flush");
    gst_video_decoder_release_frame (decoder, frame);
    return GST_FLOW_FLUSHING;
  }

  res = gst_web_codecs_video_decoder_check_error (self);
  if (res != GST_FLOW_OK) {
    gst_video_decoder_release_frame (decoder, frame);
    return res;
  }

  /* We are ready to process data */
  GST_DEBUG_OBJECT (self, "Ready to process more data");
  decode_data = g_new (GstWebCodecsVideoDecoderDecodeData, 1);
  decode_data->self = self;
  decode_data->frame = frame;
  /* We can not keep the stream lock taken here and when the decoder outputs
   * frames, do it asynchronous
   */
  runner = gst_web_canvas_get_runner (self->canvas);
  gst_web_runner_send_message_async (
      runner, gst_web_codecs_video_decoder_decode, decode_data, g_free);
  gst_object_unref (runner);

  /* Push whatever the decoder produced so far */
  res = gst_web_codecs_video_decoder_push_outputs (self);

  GST_DEBUG_OBJECT (decoder, "Handle frame done");

  return res;
}

/* Runner thread. Delivers every pending output to on_output () */
static void
gst_web_codecs_video_decoder_flush_decoder (gpointer data)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (data);

  if (!self->has_decoder)
    return;
  if (self->decoder["state"].as<std::string> () == "configured")
    self->decoder.call<val> ("flush").await ();
}

/* Called with the stream lock taken, at EOS and before caps changes */
static GstFlowReturn
gst_web_codecs_video_decoder_drain (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebRunner *runner;

  if (!self->canvas)
    return GST_FLOW_OK;

  GST_DEBUG_OBJECT (self, "Draining");
  /* on_output () does not take the stream lock, but the runner must be free
   * to run it while flush () is pending */
  runner = gst_web_canvas_get_runner (self->canvas);
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  gst_web_runner_send_message (
      runner, gst_web_codecs_video_decoder_flush_decoder, self);
  GST_VIDEO_DECODER_STREAM_LOCK (self);
  gst_object_unref (runner);

  return gst_web_codecs_video_decoder_push_outputs (self);
}

static gboolean
gst_web_codecs_video_decoder_set_format (
    GstVideoDecoder *decoder, GstVideoCodecState *state)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebCodecsVideoDecoderConfigureData conf_data;
  GstWebRunner *runner;

  GST_INFO_OBJECT (
      self, "Setting format with sink caps %" GST_PTR_FORMAT, state->caps);
#if 0
  /* If the output format IS GL, we need to request the context
   * and do everything on the GL context thread, otherwise create
   * our own thread. This is a limitation on how Emscripten works
   * tegarding emscripten::val and threads. The vals are not shared
   * among threads, are local.
   * We have an open issue for this case:
   * https://github.com/emscripten-core/emscripten/issues/22541
   *
   * In any case we'll have a situation, the video_frame belongs to the WebRunner
   * transferring the image to a GLTexture requires that both, the GL thread and the
   * WebRunner thread share the js video_frame ... we'll figure that out later.
   * There are several options to handle this
   */

  if (!gst_web_runner_run (self->runner, NULL)) {
    GST_ERROR_OBJECT (self, "Impossible to run the runner");
    return FALSE;
  }
#endif
  /* TODO Check if downstream OpenGL is supported, if so, request the GL
   * context */

  /* Call constructor */
  runner = gst_web_canvas_get_runner (self->canvas);
  gst_web_runner_send_message (
      runner, gst_web_codecs_video_decoder_ctor, self);
  /* Configure */
  conf_data.self = self;
  conf_data.state = state;
  gst_web_runner_send_message (
      runner, gst_web_codecs_video_decoder_configure, &conf_data);

  /* Keep the input state available */
  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = gst_video_codec_state_ref (state);

  gst_object_unref (runner);

  return conf_data.ret;
}

/* Runner thread. Discards queued work, then configures again: after
 * reset () a VideoDecoder is unconfigured. */
static void
gst_web_codecs_video_decoder_reset_decoder (gpointer data)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (data);
  GstWebCodecsVideoDecoderConfigureData conf_data;

  if (!self->has_decoder)
    return;
  if (self->decoder["state"].as<std::string> () == "closed")
    return;

  self->decoder.call<void> ("reset");
  if (self->input_state) {
    conf_data.self = self;
    conf_data.state = self->input_state;
    conf_data.ret = FALSE;
    gst_web_codecs_video_decoder_configure (&conf_data);
  }
}

/* Called with the stream lock taken, on FLUSH_STOP (seeks) */
static gboolean
gst_web_codecs_video_decoder_flush (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebRunner *runner;

  GST_DEBUG_OBJECT (self, "Flushing");
  self->flush_epoch++;

  if (self->canvas) {
    runner = gst_web_canvas_get_runner (self->canvas);
    gst_web_runner_send_message (
        runner, gst_web_codecs_video_decoder_reset_decoder, self);
    gst_object_unref (runner);
  }

  /* Outputs of the old segment, their codec frames are being dropped */
  gst_web_codecs_video_decoder_clear_outputs (self);

  g_mutex_lock (&self->dequeue_lock);
  self->dequeue_size = 0;
  g_cond_signal (&self->dequeue_cond);
  g_mutex_unlock (&self->dequeue_lock);

  GST_DEBUG_OBJECT (self, "Flushed");

  return TRUE;
}

static gboolean
gst_web_codecs_video_decoder_open (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  gboolean ret = FALSE;

  if (!gst_web_utils_element_ensure_canvas (
          GST_ELEMENT (self), &self->canvas, NULL)) {
    GST_ERROR_OBJECT (self, "Failed requesting a WebCanvas context");
    goto done;
  }

  ret = TRUE;

done:
  return ret;
}

static gboolean
gst_web_codecs_video_decoder_start (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebRunner *runner;
  gboolean ret = FALSE;

  GST_DEBUG_OBJECT (self, "Start");
  runner = gst_web_canvas_get_runner (self->canvas);
  if (!gst_web_runner_run (runner, NULL)) {
    GST_ERROR_OBJECT (self, "Impossible to run the runner");
    goto done;
  }
  GST_DEBUG_OBJECT (self, "Started");
  ret = TRUE;

done:
  gst_object_unref (runner);
  return ret;
}

static gboolean
gst_web_codecs_video_decoder_stop (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebRunner *runner;

  GST_DEBUG_OBJECT (self, "Stop");
  if (self->canvas) {
    runner = gst_web_canvas_get_runner (self->canvas);
    gst_web_runner_send_message (
        runner, gst_web_codecs_video_decoder_close_decoder, self);
    gst_object_unref (runner);
  }
  gst_web_codecs_video_decoder_clear_outputs (self);

  g_mutex_lock (&self->output_lock);
  g_clear_pointer (&self->error, g_free);
  g_mutex_unlock (&self->output_lock);

  g_mutex_lock (&self->dequeue_lock);
  self->dequeue_size = 0;
  g_mutex_unlock (&self->dequeue_lock);

  if (self->input_state) {
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  if (self->output_state) {
    gst_video_codec_state_unref (self->output_state);
    self->output_state = NULL;
  }

  GST_DEBUG_OBJECT (self, "Stopped");

  return TRUE;
}

static void
gst_web_codecs_video_decoder_set_context (
    GstElement *element, GstContext *context)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (element);

  gst_web_utils_element_set_context (element, context, &self->canvas);
}

static gboolean
gst_web_codecs_video_decoder_query (GstElement *element, GstQuery *query)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (element);
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      ret = gst_web_utils_element_handle_context_query (
          element, query, self->canvas);
      break;
    default:
      break;
  }

  if (!ret)
    ret = GST_ELEMENT_CLASS (parent_class)->query (element, query);

  return ret;
}

static void
gst_web_codecs_video_decoder_finalize (GObject *object)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (object);

  g_mutex_clear (&self->dequeue_lock);
  g_cond_clear (&self->dequeue_cond);
  g_queue_clear_full (&self->output_queue,
      (GDestroyNotify) gst_web_codecs_video_decoder_output_free);
  g_mutex_clear (&self->output_lock);
  g_free (self->error);
  if (self->canvas) {
    gst_object_unref (self->canvas);
    self->canvas = NULL;
  }

  GST_DEBUG_OBJECT (self, "End of finalize");
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_web_codecs_video_decoder_init (
    GstWebCodecsVideoDecoder *self, GstWebCodecsVideoDecoderClass g_class)
{
  gst_video_decoder_set_needs_sync_point (GST_VIDEO_DECODER (self), TRUE);
  g_mutex_init (&self->dequeue_lock);
  g_cond_init (&self->dequeue_cond);
  g_mutex_init (&self->output_lock);
  g_queue_init (&self->output_queue);
}

static void
gst_web_codecs_video_decoder_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstCaps *sink_caps;
  GstCaps *src_caps;
  GstCaps *copy_to_caps;
  GstCaps *gl_caps;
  GstCaps *video_frame_caps;
  GstPadTemplate *templ;

  sink_caps = (GstCaps *) g_type_get_qdata (
      G_TYPE_FROM_CLASS (g_class), gst_web_codecs_data_quark);
  /* This happens for the base class and abstract subclasses */
  if (!sink_caps)
    return;

  /* System memory, filled with VideoFrame.copyTo () when downstream does
   * not prefer VideoFrames */
  copy_to_caps = gst_caps_from_string (
      "video/x-raw, format = " GST_WEB_MEMORY_VIDEO_FORMATS_STR);

  video_frame_caps = gst_caps_from_string (
      "video/x-raw(" GST_CAPS_FEATURE_MEMORY_WEB_VIDEO_FRAME "),"
      "format = { (string) RGBA, (string) RGBx, (string) BGRA, (string) BGRx, "
      "(string) I420, (string) A420, (string) Y42B, (string) Y444, (string) "
      "NV12 }");
  gl_caps = gst_caps_from_string (
      "video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY
      "), format = (string) RGBA, texture-target = (string) 2D");

  src_caps = gst_caps_merge (gl_caps, copy_to_caps);
  src_caps = gst_caps_merge (src_caps, video_frame_caps);
  /* Add pad templates */
  templ =
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sink_caps);
  gst_element_class_add_pad_template (element_class, templ);

  templ = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, src_caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (gl_caps);
}

static void
gst_web_codecs_video_decoder_class_init (
    GstWebCodecsVideoDecoderClass *klass, gpointer klass_data)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  gobject_class->finalize = gst_web_codecs_video_decoder_finalize;
  element_class->set_context = gst_web_codecs_video_decoder_set_context;
  element_class->query = gst_web_codecs_video_decoder_query;
  gst_element_class_set_static_metadata (element_class,
      "WebCodecs base video decoder", "Codec/Decoder/Video",
      "decode streams using WebCodecs API",
      "Fluendo S.A. <engineering@fluendo.com>");

  video_decoder_class->open =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_open);
  video_decoder_class->start =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_start);
  video_decoder_class->stop =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_stop);
  video_decoder_class->flush =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_flush);
  video_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_set_format);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_handle_frame);
  video_decoder_class->negotiate =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_negotiate);
  video_decoder_class->drain =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_drain);
  video_decoder_class->finish =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_drain);

  parent_class = g_type_class_peek_parent (klass);
}

GType
gst_web_codecs_video_decoder_get_type (void)
{
  static gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = { sizeof (GstWebCodecsVideoDecoderClass),
      (GBaseInitFunc) gst_web_codecs_video_decoder_base_init, NULL,
      (GClassInitFunc) gst_web_codecs_video_decoder_class_init, NULL, NULL,
      sizeof (GstWebCodecsVideoDecoder), 0,
      (GInstanceInitFunc) gst_web_codecs_video_decoder_init, NULL };

    _type = g_type_register_static (GST_TYPE_VIDEO_DECODER,
        "GstWebCodecsVideoDecoder", &info, G_TYPE_FLAG_NONE);

    GST_DEBUG_CATEGORY_INIT (gst_web_codecs_video_decoder_debug_category,
        "webcodecsviddec", 0, "WebCodecs Video Decoder");

    g_once_init_leave (&type, _type);
  }
  return type;
}
