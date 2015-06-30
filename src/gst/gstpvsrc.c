/* GStreamer
 * Copyright (C) <2015> Wim Taymans <wim.taymans@gmail.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-pinossrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pinossrc ! videoconvert ! ximagesink
 * ]| Shows pinos output in an X window.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstpvsrc.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdmessage.h>
#include <gst/allocators/gstfdmemory.h>



GST_DEBUG_CATEGORY_STATIC (pinos_src_debug);
#define GST_CAT_DEFAULT pinos_src_debug

enum
{
  PROP_0,
  PROP_SOURCE
};


#define PVS_VIDEO_CAPS GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL)

static GstStaticPadTemplate gst_pinos_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

#define gst_pinos_src_parent_class parent_class
G_DEFINE_TYPE (GstPinosSrc, gst_pinos_src, GST_TYPE_PUSH_SRC);

static GstStateChangeReturn
gst_pinos_src_change_state (GstElement * element, GstStateChange transition);

static gboolean gst_pinos_src_negotiate (GstBaseSrc * basesrc);
static GstCaps *gst_pinos_src_getcaps (GstBaseSrc * bsrc, GstCaps * filter);
static gboolean gst_pinos_src_setcaps (GstBaseSrc * bsrc, GstCaps * caps);
static GstCaps *gst_pinos_src_src_fixate (GstBaseSrc * bsrc,
    GstCaps * caps);

static GstFlowReturn gst_pinos_src_create (GstPushSrc * psrc,
    GstBuffer ** buffer);
static gboolean gst_pinos_src_start (GstBaseSrc * basesrc);
static gboolean gst_pinos_src_stop (GstBaseSrc * basesrc);

static void
gst_pinos_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPinosSrc *pvsrc = GST_PINOS_SRC (object);

  switch (prop_id) {
    case PROP_SOURCE:
      g_free (pvsrc->source);
      pvsrc->source = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPinosSrc *pvsrc = GST_PINOS_SRC (object);

  switch (prop_id) {
    case PROP_SOURCE:
      g_value_set_string (value, pvsrc->source);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_src_finalize (GObject * object)
{
  GstPinosSrc *pvsrc = GST_PINOS_SRC (object);

  g_object_unref (pvsrc->fd_allocator);
  g_mutex_clear (&pvsrc->lock);
  g_cond_clear (&pvsrc->cond);
  g_free (pvsrc->source);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_pinos_src_class_init (GstPinosSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpushsrc_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->finalize = gst_pinos_src_finalize;
  gobject_class->set_property = gst_pinos_src_set_property;
  gobject_class->get_property = gst_pinos_src_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_SOURCE,
                                   g_param_spec_string ("source",
                                                        "Source",
                                                        "The source name to connect to (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));


  gstelement_class->change_state = gst_pinos_src_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "Pinos source", "Source/Video",
      "Uses pinos to create video", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pinos_src_template));

  gstbasesrc_class->negotiate = gst_pinos_src_negotiate;
  gstbasesrc_class->get_caps = gst_pinos_src_getcaps;
  gstbasesrc_class->set_caps = gst_pinos_src_setcaps;
  gstbasesrc_class->fixate = gst_pinos_src_src_fixate;
  gstbasesrc_class->start = gst_pinos_src_start;
  gstbasesrc_class->stop = gst_pinos_src_stop;

  gstpushsrc_class->create = gst_pinos_src_create;

  GST_DEBUG_CATEGORY_INIT (pinos_src_debug, "pinossrc", 0,
      "Pinos Source");
}

static void
gst_pinos_src_init (GstPinosSrc * src)
{
  /* we operate in time */
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);
  gst_base_src_set_do_timestamp (GST_BASE_SRC (src), TRUE);

  src->fd_allocator = gst_fd_allocator_new ();
  g_mutex_init (&src->lock);
  g_cond_init (&src->cond);
}

static GstCaps *
gst_pinos_src_src_fixate (GstBaseSrc * bsrc, GstCaps * caps)
{
  GstStructure *structure;

  caps = gst_caps_make_writable (caps);

  structure = gst_caps_get_structure (caps, 0);

  gst_structure_fixate_field_nearest_int (structure, "width", 320);
  gst_structure_fixate_field_nearest_int (structure, "height", 240);
  gst_structure_fixate_field_nearest_fraction (structure, "framerate", 30, 1);

  if (gst_structure_has_field (structure, "pixel-aspect-ratio"))
    gst_structure_fixate_field_nearest_fraction (structure,
        "pixel-aspect-ratio", 1, 1);
  else
    gst_structure_set (structure, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
        NULL);

  if (gst_structure_has_field (structure, "colorimetry"))
    gst_structure_fixate_field_string (structure, "colorimetry", "bt601");
  if (gst_structure_has_field (structure, "chroma-site"))
    gst_structure_fixate_field_string (structure, "chroma-site", "mpeg2");

  if (gst_structure_has_field (structure, "interlace-mode"))
    gst_structure_fixate_field_string (structure, "interlace-mode",
        "progressive");
  else
    gst_structure_set (structure, "interlace-mode", G_TYPE_STRING,
        "progressive", NULL);

  caps = GST_BASE_SRC_CLASS (parent_class)->fixate (bsrc, caps);

  return caps;
}

static void
on_new_buffer (GObject    *gobject,
               gpointer    user_data)
{
  GstPinosSrc *pvsrc = user_data;

  g_cond_signal (&pvsrc->cond);
}

static void
on_stream_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PvStreamState state;
  GstPinosSrc *pvsrc = user_data;

  state = pv_stream_get_state (pvsrc->stream);
  g_print ("got stream state %d\n", state);
  g_cond_broadcast (&pvsrc->cond);

  if (state == PV_STREAM_STATE_ERROR) {
    GST_ELEMENT_ERROR (pvsrc, RESOURCE, FAILED,
        ("Failed to connect stream: %s",
          pv_stream_get_error (pvsrc->stream)->message), (NULL));
  }
}

static gboolean
gst_pinos_src_negotiate (GstBaseSrc * basesrc)
{
  GstPinosSrc *pvsrc = GST_PINOS_SRC (basesrc);
  GstCaps *thiscaps;
  GstCaps *caps = NULL;
  GstCaps *peercaps = NULL;
  gboolean result = FALSE;

  /* first see what is possible on our source pad */
  thiscaps = gst_pad_query_caps (GST_BASE_SRC_PAD (basesrc), NULL);
  GST_DEBUG_OBJECT (basesrc, "caps of src: %" GST_PTR_FORMAT, thiscaps);
  /* nothing or anything is allowed, we're done */
  if (thiscaps == NULL)
    goto no_nego_needed;

  if (G_UNLIKELY (gst_caps_is_empty (thiscaps)))
    goto no_caps;

  /* get the peer caps */
  peercaps = gst_pad_peer_query_caps (GST_BASE_SRC_PAD (basesrc), thiscaps);
  GST_DEBUG_OBJECT (basesrc, "caps of peer: %" GST_PTR_FORMAT, peercaps);
  if (peercaps) {
    /* The result is already a subset of our caps */
    caps = peercaps;
    gst_caps_unref (thiscaps);
  } else {
    /* no peer, work with our own caps then */
    caps = thiscaps;
  }
  if (caps && !gst_caps_is_empty (caps)) {
    GBytes *accepted, *possible;
    gchar *str;

    GST_DEBUG_OBJECT (basesrc, "have caps: %" GST_PTR_FORMAT, caps);
    /* open a connection with these caps */
    str = gst_caps_to_string (caps);
    accepted = g_bytes_new_take (str, strlen (str) + 1);

    g_mutex_lock (&pvsrc->lock);
    pv_stream_connect_capture (pvsrc->stream, pvsrc->source, 0, accepted);

    while (TRUE) {
      PvStreamState state = pv_stream_get_state (pvsrc->stream);

      if (state == PV_STREAM_STATE_READY)
        break;

      if (state == PV_STREAM_STATE_ERROR)
        goto connect_error;

      g_cond_wait (&pvsrc->cond, &pvsrc->lock);
    }
    g_mutex_unlock (&pvsrc->lock);

    g_object_get (pvsrc->stream, "possible-formats", &possible, NULL);
    if (possible) {
      GstCaps *newcaps;

      newcaps = gst_caps_from_string (g_bytes_get_data (possible, NULL));
      if (newcaps)
        caps = newcaps;
    }
    /* now fixate */
    GST_DEBUG_OBJECT (basesrc, "server fixated caps: %" GST_PTR_FORMAT, caps);
    if (gst_caps_is_any (caps)) {
      GST_DEBUG_OBJECT (basesrc, "any caps, we stop");
      /* hmm, still anything, so element can do anything and
       * nego is not needed */
      result = TRUE;
    } else {
      caps = gst_pinos_src_src_fixate (basesrc, caps);
      GST_DEBUG_OBJECT (basesrc, "fixated to: %" GST_PTR_FORMAT, caps);
      if (gst_caps_is_fixed (caps)) {
        /* yay, fixed caps, use those then, it's possible that the subclass does
         * not accept this caps after all and we have to fail. */
        result = gst_base_src_set_caps (basesrc, caps);
      }
    }
    gst_caps_unref (caps);
  } else {
    if (caps)
      gst_caps_unref (caps);
    GST_DEBUG_OBJECT (basesrc, "no common caps");
  }
  pvsrc->negotiated = result;
  return result;

no_nego_needed:
  {
    GST_DEBUG_OBJECT (basesrc, "no negotiation needed");
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
no_caps:
  {
    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element did not produce valid caps"));
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
connect_error:
  {
    g_mutex_unlock (&pvsrc->lock);
    return FALSE;
  }
}

static GstCaps *
gst_pinos_src_getcaps (GstBaseSrc * bsrc, GstCaps * filter)
{
  return GST_BASE_SRC_CLASS (parent_class)->get_caps (bsrc, filter);
}

static gboolean
gst_pinos_src_setcaps (GstBaseSrc * bsrc, GstCaps * caps)
{
  GstPinosSrc *pvsrc;
  gchar *str;
  GBytes *format;
  gboolean res;

  pvsrc = GST_PINOS_SRC (bsrc);

  str = gst_caps_to_string (caps);
  format = g_bytes_new_take (str, strlen (str) + 1);

  g_mutex_lock (&pvsrc->lock);
  res = pv_stream_start (pvsrc->stream, format, PV_STREAM_MODE_BUFFER);
  g_mutex_unlock (&pvsrc->lock);

  return res;
}

static GstFlowReturn
gst_pinos_src_create (GstPushSrc * psrc, GstBuffer ** buffer)
{
  GstPinosSrc *pvsrc;
  PvBufferInfo info;
  gint *fds, n_fds;
  GstMemory *fdmem = NULL;

  pvsrc = GST_PINOS_SRC (psrc);

  if (!pvsrc->negotiated)
    goto not_negotiated;

again:
  g_mutex_lock (&pvsrc->lock);
  while (TRUE) {
    PvStreamState state;

    g_cond_wait (&pvsrc->cond, &pvsrc->lock);

    state = pv_stream_get_state (pvsrc->stream);
    if (state == PV_STREAM_STATE_ERROR)
      goto streaming_error;

    if (state != PV_STREAM_STATE_STREAMING)
      goto streaming_stopped;

    pv_stream_capture_buffer (pvsrc->stream, &info);
    if (info.message != NULL)
      break;
  }
  g_mutex_unlock (&pvsrc->lock);

  if (g_socket_control_message_get_msg_type (info.message) != SCM_RIGHTS)
    goto again;

  fds = g_unix_fd_message_steal_fds (G_UNIX_FD_MESSAGE (info.message), &n_fds);
  if (n_fds < 1 || fds[0] < 0)
    goto again;

  fdmem = gst_fd_allocator_alloc (pvsrc->fd_allocator, fds[0],
            info.offset + info.size, GST_FD_MEMORY_FLAG_NONE);
  gst_memory_resize (fdmem, info.offset, info.size);

  *buffer = gst_buffer_new ();
  gst_buffer_append_memory (*buffer, fdmem);

  return GST_FLOW_OK;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
streaming_error:
  {
    g_mutex_unlock (&pvsrc->lock);
    return GST_FLOW_ERROR;
  }
streaming_stopped:
  {
    g_mutex_unlock (&pvsrc->lock);
    return GST_FLOW_FLUSHING;
  }
}

static gboolean
gst_pinos_src_start (GstBaseSrc * basesrc)
{
  return TRUE;
}

static gboolean
gst_pinos_src_stop (GstBaseSrc * basesrc)
{
  return TRUE;
}

static GPrivate src_key;

static gint
do_poll (GPollFD *ufds, guint nfsd, gint timeout_)
{
  gint res;
  GstPinosSrc *this = g_private_get (&src_key);

  g_mutex_unlock (&this->lock);
  res = this->poll_func (ufds, nfsd, timeout_);
  g_mutex_lock (&this->lock);

  return res;
}

static gpointer
handle_mainloop (GstPinosSrc *this)
{
  g_mutex_lock (&this->lock);
  g_private_set (&src_key, this);
  this->poll_func = g_main_context_get_poll_func (this->context);
  g_main_context_set_poll_func (this->context, do_poll);
  g_main_context_push_thread_default (this->context);
  g_print ("run mainloop\n");
  g_main_loop_run (this->loop);
  g_print ("quit mainloop\n");
  g_main_context_pop_thread_default (this->context);
  g_mutex_unlock (&this->lock);

  return NULL;
}

static void
on_state_notify (GObject    *gobject,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
  GstPinosSrc *pvsrc = user_data;
  PvContextState state;

  state = pv_context_get_state (pvsrc->ctx);
  g_print ("got context state %d\n", state);
  g_cond_broadcast (&pvsrc->cond);

  if (state == PV_CONTEXT_STATE_ERROR) {
    GST_ELEMENT_ERROR (pvsrc, RESOURCE, FAILED,
        ("Failed to connect stream: %s",
          pv_context_get_error (pvsrc->ctx)->message), (NULL));
  }
}

static gboolean
gst_pinos_src_open (GstPinosSrc * pvsrc)
{

  g_mutex_lock (&pvsrc->lock);
  pvsrc->ctx = pv_context_new (pvsrc->context, "test-client", NULL);
  g_signal_connect (pvsrc->ctx, "notify::state", (GCallback) on_state_notify, pvsrc);

  pv_context_connect(pvsrc->ctx, PV_CONTEXT_FLAGS_NONE);

  while (TRUE) {
    PvContextState state = pv_context_get_state (pvsrc->ctx);

    if (state == PV_CONTEXT_STATE_READY)
      break;

    if (state == PV_CONTEXT_STATE_ERROR)
      goto connect_error;

    g_cond_wait (&pvsrc->cond, &pvsrc->lock);
  }

  pvsrc->stream = pv_stream_new (pvsrc->ctx, "test", NULL);
  g_signal_connect (pvsrc->stream, "notify::state", (GCallback) on_stream_notify, pvsrc);
  g_signal_connect (pvsrc->stream, "new-buffer", (GCallback) on_new_buffer, pvsrc);
  g_mutex_unlock (&pvsrc->lock);

  return TRUE;

  /* ERRORS */
connect_error:
  {
    g_mutex_unlock (&pvsrc->lock);
    return FALSE;
  }
}

static GstStateChangeReturn
gst_pinos_src_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPinosSrc *this = GST_PINOS_SRC_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      this->context = g_main_context_new ();
      g_print ("context %p\n", this->context);
      this->loop = g_main_loop_new (this->context, FALSE);
      this->thread = g_thread_new ("pinos", (GThreadFunc) handle_mainloop, this);
      if (!gst_pinos_src_open (this)) {
        ret = GST_STATE_CHANGE_FAILURE;
        goto exit;
      }
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      /* uncork and start recording */
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      /* stop recording ASAP by corking */
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      this->negotiated = FALSE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      g_main_loop_quit (this->loop);
      g_thread_join (this->thread);
      g_main_loop_unref (this->loop);
      g_main_context_unref (this->context);
      break;
    default:
      break;
  }

exit:
  return ret;
}
