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

#ifndef __GST_WEB_CODECS_VIDEO_DECODER_H__
#define __GST_WEB_CODECS_VIDEO_DECODER_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>
#include <emscripten/bind.h>
#include <string.h>
#include <gst/web/gstwebcanvas.h>
#include <gst/web/gstwebrunner.h>

G_BEGIN_DECLS

#define GST_TYPE_WEB_CODECS_VIDEO_DECODER                                     \
  (gst_web_codecs_video_decoder_get_type ())
#define GST_WEB_CODECS_VIDEO_DECODER(obj)                                     \
  (G_TYPE_CHECK_INSTANCE_CAST (                                               \
      (obj), GST_TYPE_WEB_CODECS_VIDEO_DECODER, GstWebCodecsVideoDecoder))
#define GST_WEB_CODECS_VIDEO_DECODER_CLASS(klass)                             \
  (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_WEB_CODECS_VIDEO_DECODER,       \
      GstWebCodecsVideoDecoderClass))
#define GST_IS_WEB_CODECS_VIDEO_DECODER(obj)                                  \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_WEB_CODECS_VIDEO_DECODER))
#define GST_IS_WEB_CODECS_VIDEO_DECODER_CLASS(klass)                          \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_WEB_CODECS_VIDEO_DECODER))

typedef struct _GstWebCodecsVideoDecoder GstWebCodecsVideoDecoder;
typedef struct _GstWebCodecsVideoDecoderClass GstWebCodecsVideoDecoderClass;

/**
 * GstWebCodecsVideoDecoder:
 *
 * Opaque object data structure.
 */
struct _GstWebCodecsVideoDecoder
{
  GstVideoDecoder base;

  GstVideoCodecState *input_state;
  GstVideoCodecState *output_state;

  /* TODO move this to a prv struct */
  GstWebCanvas *canvas;
  gint width;
  gint height;
  GstVideoFormat format;
  gboolean need_negotiation;
  /* Downstream does not prefer memory:WebVideoFrame, copy to system memory */
  gboolean system_memory;

  emscripten::val decoder;
  /* Runner thread only: decoder holds a VideoDecoder */
  gboolean has_decoder;
  /* Runner thread only: next chunk must be a key frame */
  gboolean key_required;
  /* Amount of the output frames pending to be dequeued */
  gint dequeue_size;
  GMutex dequeue_lock;
  GCond dequeue_cond;

  /* VideoFrames produced on the runner thread, pushed downstream from the
   * streaming thread. Protected by output_lock */
  GMutex output_lock;
  GQueue output_queue;
  /* Incremented on every flush, to discard frames submitted before it */
  guint flush_epoch;
  /* VideoDecoder.flush () started by drain () not settled yet. Protected
   * by output_lock, signalled with flush_cond */
  gboolean flush_pending;
  GCond flush_cond;
  /* Set from the WebCodecs error callback */
  gchar *error;
};

struct _GstWebCodecsVideoDecoderClass
{
  GstVideoDecoderClass base;
};

GType gst_web_codecs_video_decoder_get_type (void);

G_END_DECLS

#endif /* __GST_WEB_CODECS_VIDEO_DECODER_H__ */
