/* GStreamer
 * Copyright (C) 2014 William Manley <will@williammanley.net>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstpinospay
 *
 * The pinospay element converts regular GStreamer buffers into the format
 * expected by Pinos.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v videotestsrc ! video/x-raw,format=RGB,width=1920,height=1080 \
 *         ! pinospay ! fdsink fd=1 \
 *     | gst-launch-1.0 fdsrc fd=0 ! fddepay \
 *         ! video/x-raw,format=RGB,width=1920,height=1080 ! autovideosink
 * ]|
 * Video frames are created in the first gst-launch-1.0 process and displayed
 * by the second with no copying.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/base/gstbasetransform.h>
#include "gstpinospay.h"
#include "gsttmpfileallocator.h"

#include <gst/net/gstnetcontrolmessagemeta.h>
#include <gst/video/video.h>

#include <gio/gunixfdmessage.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

GST_DEBUG_CATEGORY_STATIC (gst_pinos_pay_debug_category);
#define GST_CAT_DEFAULT gst_pinos_pay_debug_category

static GQuark fdids_quark;
static GQuark orig_buffer_quark;

/* prototypes */

/* pad templates */
static GstStaticPadTemplate gst_pinos_pay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-pinos"));

static GstStaticPadTemplate gst_pinos_pay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstPinosPay, gst_pinos_pay, GST_TYPE_ELEMENT,
    GST_DEBUG_CATEGORY_INIT (gst_pinos_pay_debug_category, "pinospay", 0,
        "debug category for pinospay element"));

/* propose allocation query parameters for input buffers */
static gboolean
gst_pinos_pay_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstPinosPay *pay = GST_PINOS_PAY (parent);
  gboolean res = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_ALLOCATION:
    {
      GST_DEBUG_OBJECT (pay, "propose_allocation");
      gst_query_add_allocation_param (query, pay->allocator, NULL);
      res = TRUE;
      break;
    }
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }
  return res;
}

static gboolean
do_allocation (GstPinosPay *pay, GstCaps *caps)
{
  GstQuery *query;

  GST_DEBUG_OBJECT (pay, "doing allocation query");
  query = gst_query_new_allocation (caps, TRUE);
  if (!gst_pad_peer_query (pay->srcpad, query)) {
    /* not a problem, just debug a little */
    GST_DEBUG_OBJECT (pay, "peer ALLOCATION query failed");
  }
  if (!gst_query_find_allocation_meta (query,
      GST_NET_CONTROL_MESSAGE_META_API_TYPE, NULL))
    goto no_meta;

  gst_query_unref (query);

  return TRUE;

  /* ERRORS */
no_meta:
  {
    GST_ELEMENT_ERROR (pay, STREAM, FORMAT,
        ("Incompatible downstream element"),
        ("The downstream element does not handle control-message metadata API"));
    gst_query_unref (query);
    return FALSE;
  }
}

static gboolean
gst_pinos_pay_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstPinosPay *pay = GST_PINOS_PAY (parent);
  gboolean res = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      GstStructure *str;

      gst_event_parse_caps (event, &caps);
      str = gst_caps_get_structure (caps, 0);
      pay->pinos_input = gst_structure_has_name (str, "application/x-pinos");
      gst_event_unref (event);

      caps = gst_caps_new_empty_simple ("application/x-pinos");
      res = gst_pad_push_event (pay->srcpad, gst_event_new_caps (caps));
      gst_caps_unref (caps);

      if (res)
        /* now negotiate the allocation */
        res = do_allocation (pay, caps);
      break;
    }
    default:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }
  return res;
}

static void
client_buffer_sent (GstPinosPay *pay, GstBuffer *buffer,
    GObject *obj)
{
  GArray *fdids;
  guint i;
  const gchar *client_path;

  fdids = gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buffer), fdids_quark);
  if (fdids == NULL)
    return;

  /* get the client path of this socket */
  client_path = g_object_get_data (obj, "pinos-client-path");
  if (client_path == NULL)
    return;

  for (i = 0; i < fdids->len; i++) {
    gint id = g_array_index (fdids, guint32, i);
    /* now store the id/client-path/buffer in the fdmanager */
    GST_LOG ("fd index %d, client %s increment refcount of buffer %p", id, client_path, buffer);
    pinos_fd_manager_add (pay->fdmanager,
                          client_path, id,
                          gst_buffer_ref (buffer),
                          (GDestroyNotify) gst_buffer_unref);
  }
}

static void
client_buffer_received (GstPinosPay *pay, GstBuffer *buffer,
    GObject *obj)
{
  PinosBuffer pbuf;
  PinosBufferIter it;
  PinosBufferBuilder b;
  GstMapInfo info;
  const gchar *client_path;
  gboolean have_out = FALSE;

  client_path = g_object_get_data (obj, "pinos-client-path");
  if (client_path == NULL)
    return;

  if (pay->pinos_input) {
    pinos_buffer_builder_init (&b);
  }

  gst_buffer_map (buffer, &info, GST_MAP_READ);
  pinos_buffer_init_data (&pbuf, info.data, info.size, NULL, 0);
  pinos_buffer_iter_init (&it, &pbuf);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_RELEASE_FD_PAYLOAD:
      {
        PinosPacketReleaseFDPayload p;
        gint id;

        if (!pinos_buffer_iter_parse_release_fd_payload (&it, &p))
          continue;

        id = p.id;

        GST_LOG ("fd index %d for client %s is released", id, client_path);
        pinos_fd_manager_remove (pay->fdmanager, client_path, id);
        break;
      }
      case PINOS_PACKET_TYPE_REFRESH_REQUEST:
      {
        PinosPacketRefreshRequest p;

        if (!pinos_buffer_iter_parse_refresh_request (&it, &p))
          continue;

        GST_LOG ("refresh request");
        if (!pay->pinos_input) {
          gst_pad_push_event (pay->sinkpad,
              gst_video_event_new_upstream_force_key_unit (p.pts,
              p.request_type == 1, 0));
        } else {
          pinos_buffer_builder_add_refresh_request (&b, &p);
          have_out = TRUE;
        }
        break;
      }
      default:
        break;
    }
  }
  pinos_buffer_iter_end (&it);
  pinos_buffer_unref (&pbuf);
  gst_buffer_unmap (buffer, &info);

  if (pay->pinos_input) {
    GstBuffer *outbuf;
    GstEvent *ev;
    gsize size;
    gpointer data;

    if (have_out) {
      pinos_buffer_builder_end (&b, &pbuf);

      data = pinos_buffer_steal_data (&pbuf, &size);

      outbuf = gst_buffer_new_wrapped (data, size);
      ev = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM,
              gst_structure_new ("GstNetworkMessage",
                  "object", G_TYPE_OBJECT, pay,
                  "buffer", GST_TYPE_BUFFER, outbuf, NULL));
      gst_buffer_unref (outbuf);

      gst_pad_push_event (pay->sinkpad, ev);
    } else {
      pinos_buffer_builder_clear (&b);
    }
  }
}

static gboolean
gst_pinos_pay_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstPinosPay *pay = GST_PINOS_PAY (parent);
  gboolean res = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:
    {
      if (gst_event_has_name (event, "GstNetworkMessageDispatched")) {
        const GstStructure *str = gst_event_get_structure (event);
        GstBuffer *buf;
        GObject *obj;

        gst_structure_get (str, "object", G_TYPE_OBJECT, &obj,
            "buffer", GST_TYPE_BUFFER, &buf, NULL);

        client_buffer_sent (pay, buf, obj);
        gst_buffer_unref (buf);
        g_object_unref (obj);
      }
      else if (gst_event_has_name (event, "GstNetworkMessage")) {
        const GstStructure *str = gst_event_get_structure (event);
        GstBuffer *buf;
        GObject *obj;

        gst_structure_get (str, "object", G_TYPE_OBJECT, &obj,
            "buffer", GST_TYPE_BUFFER, &buf, NULL);

        client_buffer_received (pay, buf, obj);
        gst_buffer_unref (buf);
        g_object_unref (obj);

      }
      res = TRUE;
      gst_event_unref (event);
      break;
    }
    default:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }
  return res;
}

static GstMemory *
gst_pinos_pay_get_fd_memory (GstPinosPay * tmpfilepay, GstBuffer * buffer, gboolean *tmpfile)
{
  GstMemory *mem = NULL;

  if (gst_buffer_n_memory (buffer) == 1
      && gst_is_fd_memory (gst_buffer_peek_memory (buffer, 0))) {
    mem = gst_buffer_get_memory (buffer, 0);
    *tmpfile = gst_is_tmpfile_memory (mem);
  } else {
    GstMapInfo info;
    GstAllocationParams params = {0, 0, 0, 0, { NULL, }};
    gsize size = gst_buffer_get_size (buffer);
    GST_INFO_OBJECT (tmpfilepay, "Buffer cannot be payloaded without copying");
    mem = gst_allocator_alloc (tmpfilepay->allocator, size, &params);
    if (!gst_memory_map (mem, &info, GST_MAP_WRITE))
      return NULL;
    gst_buffer_extract (buffer, 0, info.data, size);
    gst_memory_unmap (mem, &info);
    *tmpfile = TRUE;
  }
  return mem;
}

static void
release_fds (GstPinosPay *pay, GstBuffer *buffer)
{
  GArray *fdids;
  guint i;
  PinosBufferBuilder b;
  PinosPacketReleaseFDPayload r;
  PinosBuffer pbuf;
  gsize size;
  gpointer data;
  GstBuffer *outbuf;
  GstEvent *ev;

  fdids = gst_mini_object_steal_qdata (GST_MINI_OBJECT_CAST (buffer),
      fdids_quark);
  if (fdids == NULL)
    return;

  pinos_buffer_builder_init (&b);

  for (i = 0; i < fdids->len; i++) {
    r.id = g_array_index (fdids, guint32, i);
    GST_LOG ("release fd index %d", r.id);
    pinos_buffer_builder_add_release_fd_payload (&b, &r);
  }
  pinos_buffer_builder_end (&b, &pbuf);
  g_array_unref (fdids);

  data = pinos_buffer_steal_data (&pbuf, &size);

  outbuf = gst_buffer_new_wrapped (data, size);
  ev = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM,
          gst_structure_new ("GstNetworkMessage",
              "object", G_TYPE_OBJECT, pay,
              "buffer", GST_TYPE_BUFFER, outbuf, NULL));
  gst_buffer_unref (outbuf);

  gst_pad_push_event (pay->sinkpad, ev);
  g_object_unref (pay);
}

static GstFlowReturn
gst_pinos_pay_chain_pinos (GstPinosPay *pay, GstBuffer * buffer)
{
  GstMapInfo info;
  PinosBuffer pbuf;
  PinosBufferIter it;
  GArray *fdids = NULL;

  gst_buffer_map (buffer, &info, GST_MAP_READ);
  pinos_buffer_init_data (&pbuf, info.data, info.size, NULL, 0);
  pinos_buffer_iter_init (&it, &pbuf);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_FD_PAYLOAD:
      {
        PinosPacketFDPayload p;

        if (!pinos_buffer_iter_parse_fd_payload (&it, &p))
          continue;

        if (fdids == NULL)
          fdids = g_array_new (FALSE, FALSE, sizeof (guint32));

        GST_LOG ("track fd index %d", p.id);
        g_array_append_val (fdids, p.id);
        break;
      }
      case PINOS_PACKET_TYPE_FORMAT_CHANGE:
      {
        PinosPacketFormatChange p;
        GstCaps * caps;

        if (!pinos_buffer_iter_parse_format_change (&it, &p))
          continue;

        caps = gst_caps_from_string (p.format);

        gst_element_post_message (GST_ELEMENT (pay),
            gst_message_new_element (GST_OBJECT (pay),
                gst_structure_new ("PinosPayloaderFormatChange",
                    "format", GST_TYPE_CAPS, caps, NULL)));
        gst_caps_unref (caps);
        break;
      }
      default:
        break;
    }
  }
  pinos_buffer_iter_end (&it);
  pinos_buffer_unref (&pbuf);
  gst_buffer_unmap (buffer, &info);

  if (fdids != NULL) {
    gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buffer),
        fdids_quark, fdids, NULL);
    gst_mini_object_weak_ref (GST_MINI_OBJECT_CAST (buffer),
        (GstMiniObjectNotify) release_fds, g_object_ref (pay));
  }

  return gst_pad_push (pay->srcpad, buffer);
}

static GstFlowReturn
gst_pinos_pay_chain_other (GstPinosPay *pay, GstBuffer * buffer)
{
  GstMemory *fdmem = NULL;
  GError *err = NULL;
  GstBuffer *outbuf;
  PinosBuffer pbuf;
  PinosBufferBuilder builder;
  PinosPacketHeader hdr;
  PinosPacketFDPayload p;
  gsize size;
  gpointer data;
  GSocketControlMessage *msg;
  gboolean tmpfile = TRUE;
  gint *fds, n_fds, i;

  hdr.flags = 0;
  hdr.seq = GST_BUFFER_OFFSET (buffer);
  hdr.pts = GST_BUFFER_PTS (buffer) + GST_ELEMENT_CAST (pay)->base_time;
  hdr.dts_offset = 0;

  pinos_buffer_builder_init (&builder);
  pinos_buffer_builder_add_header (&builder, &hdr);

  msg = g_unix_fd_message_new ();

  fdmem = gst_pinos_pay_get_fd_memory (pay, buffer, &tmpfile);
  p.fd_index = pinos_buffer_builder_add_fd (&builder, gst_fd_memory_get_fd (fdmem));
  p.id = pinos_fd_manager_get_id (pay->fdmanager);
  p.offset = fdmem->offset;
  p.size = fdmem->size;
  pinos_buffer_builder_add_fd_payload (&builder, &p);

  pinos_buffer_builder_end (&builder, &pbuf);

  data = pinos_buffer_steal_data (&pbuf, &size);
  fds = pinos_buffer_steal_fds (&pbuf, &n_fds);
  pinos_buffer_unref (&pbuf);

  outbuf = gst_buffer_new_wrapped (data, size);
  GST_BUFFER_PTS (outbuf) = GST_BUFFER_PTS (buffer);
  GST_BUFFER_DTS (outbuf) = GST_BUFFER_DTS (buffer);
  GST_BUFFER_DURATION (outbuf) = GST_BUFFER_DURATION (buffer);
  GST_BUFFER_OFFSET (outbuf) = GST_BUFFER_OFFSET (buffer);
  GST_BUFFER_OFFSET_END (outbuf) = GST_BUFFER_OFFSET_END (buffer);

  msg = g_unix_fd_message_new ();
  for (i = 0; i < n_fds; i++) {
    if (!g_unix_fd_message_append_fd (G_UNIX_FD_MESSAGE (msg), fds[i], &err))
      goto add_fd_failed;
  }
  gst_buffer_add_net_control_message_meta (outbuf, msg);
  g_object_unref (msg);
  g_free (fds);

  gst_memory_unref(fdmem);
  fdmem = NULL;

  if (!tmpfile) {
    GArray *fdids;
    /* we are using the original buffer fd in the control message, we need
     * to make sure it is not reused before everyone is finished with it.
     * We tag the output buffer with the array of fds in it and the original
     * buffer (to keep it alive). All clients that receive the fd will
     * increment outbuf refcount, all clients that do release-fd on the fd
     * will decrease the refcount again. */
    fdids = g_array_new (FALSE, FALSE, sizeof (guint32));
    g_array_append_val (fdids, p.id);
    gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (outbuf),
        fdids_quark, fdids, (GDestroyNotify) g_array_unref);
    gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (outbuf),
        orig_buffer_quark, buffer, (GDestroyNotify) gst_buffer_unref);
  } else {
    gst_buffer_unref (buffer);
  }

  return gst_pad_push (pay->srcpad, outbuf);

  /* ERRORS */
add_fd_failed:
  {
    GST_WARNING_OBJECT (pay, "Adding fd failed: %s", err->message);
    gst_object_unref(msg);
    g_clear_error (&err);

    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_pinos_pay_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstPinosPay *pay = GST_PINOS_PAY (parent);

  if (pay->pinos_input)
    return gst_pinos_pay_chain_pinos (pay, buffer);
  else
    return gst_pinos_pay_chain_other (pay, buffer);
}
static void
gst_pinos_pay_finalize (GObject * object)
{
  GstPinosPay *pay = GST_PINOS_PAY (object);

  GST_DEBUG_OBJECT (pay, "finalize");

  g_object_unref (pay->allocator);
  g_object_unref (pay->fdmanager);

  G_OBJECT_CLASS (gst_pinos_pay_parent_class)->finalize (object);
}

static void
gst_pinos_pay_init (GstPinosPay * pay)
{
  pay->srcpad = gst_pad_new_from_static_template (&gst_pinos_pay_src_template, "src");
  gst_pad_set_event_function (pay->srcpad, gst_pinos_pay_src_event);
  gst_element_add_pad (GST_ELEMENT (pay), pay->srcpad);

  pay->sinkpad = gst_pad_new_from_static_template (&gst_pinos_pay_sink_template, "sink");
  gst_pad_set_chain_function (pay->sinkpad, gst_pinos_pay_chain);
  gst_pad_set_event_function (pay->sinkpad, gst_pinos_pay_sink_event);
  gst_pad_set_query_function (pay->sinkpad, gst_pinos_pay_query);
  gst_element_add_pad (GST_ELEMENT (pay), pay->sinkpad);

  pay->allocator = gst_tmpfile_allocator_new ();
  pay->fdmanager = pinos_fd_manager_get (PINOS_FD_MANAGER_DEFAULT);
}

static void
gst_pinos_pay_class_init (GstPinosPayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_pinos_pay_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_pinos_pay_sink_template));

  gst_element_class_set_static_metadata (element_class,
      "Pinos Payloader", "Generic",
      "Pinos Payloader for zero-copy IPC with Pinos",
      "Wim Taymans <wim.taymans@gmail.com>");

  gobject_class->finalize = gst_pinos_pay_finalize;

  fdids_quark = g_quark_from_static_string ("GstPinosPayFDIds");
  orig_buffer_quark = g_quark_from_static_string ("GstPinosPayOrigBuffer");
}
