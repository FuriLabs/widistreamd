#define _GNU_SOURCE

#include <gst/app/gstappsrc.h>
#include <linux/memfd.h>
#include <sys/mman.h>

#include <avahi-gobject/ga-client.h>

#include "widi-stream-config.h"
#include "nd-cc-provider.h"
#include "nd-meta-provider.h"
#include "nd-mtk-wifi-manager.h"
#include "nd-nm-device-registry.h"
#include "nd-pulseaudio.h"
#include "nd-sink-list-model.h"
#include "nd-wfd-mice-provider.h"

#define WIDI_BUS_NAME      "io.furios.WidiStream"
#define WIDI_MANAGER_PATH  "/io/furios/WidiStream"
#define WIDI_STREAM_PREFIX "/io/furios/WidiStream/Streams"

typedef struct {
  GMainLoop *loop;
  GDBusConnection *connection;

  guint owner_id;
  guint manager_registration_id;

  GDBusNodeInfo *introspection_data;

  GCancellable *cancellable;

  GaClient *avahi_client;
  NdMetaProvider *meta_provider;
  NdWFDMiceProvider *mice_provider;
  NdCCProvider *cc_provider;
  NdNMDeviceRegistry *nm_device_registry;
  NdMtkWifiManager *mtk_wifi_manager;
  NdPulseaudio *pulse;

  NdSinkListModel *sink_list_model;

  GHashTable *sinks_by_id;
  GHashTable *sink_ids_by_sink;
  GHashTable *streams_by_path;
  GHashTable *stream_registration_ids;

  guint next_sink_id;
  guint next_stream_id;

  gboolean discovering;
} WidiService;

typedef struct {
  WidiService *service;

  gchar *id;
  gchar *object_path;
  gchar *sink_id;

  NdSink *sink;
  NdSink *stream_sink;

  GstElement *appsrc;

  gint memfd;
  gsize memfd_size;
  guint width;
  guint height;
  guint stride;
  gchar *format;

  gchar *state;
} WidiStream;

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='io.furios.WidiStream.Manager'>"
  "    <method name='StartDiscovery'/>"
  "    <method name='StopDiscovery'/>"
  "    <method name='ListSinks'>"
  "      <arg name='sinks' type='aa{sv}' direction='out'/>"
  "    </method>"
  "    <method name='Connect'>"
  "      <arg name='sink_id' type='s' direction='in'/>"
  "      <arg name='stream_path' type='o' direction='out'/>"
  "    </method>"
  "    <signal name='SinkAdded'>"
  "      <arg name='sink' type='a{sv}'/>"
  "    </signal>"
  "    <signal name='SinkRemoved'>"
  "      <arg name='sink_id' type='s'/>"
  "    </signal>"
  "    <signal name='SinkChanged'>"
  "      <arg name='sink' type='a{sv}'/>"
  "    </signal>"
  "    <property name='Discovering' type='b' access='read'/>"
  "  </interface>"
  "  <interface name='io.furios.WidiStream.Stream'>"
  "    <method name='GetMemfd'>"
  "      <arg name='width' type='u' direction='in'/>"
  "      <arg name='height' type='u' direction='in'/>"
  "      <arg name='stride' type='u' direction='in'/>"
  "      <arg name='format' type='s' direction='in'/>"
  "      <arg name='fd' type='h' direction='out'/>"
  "    </method>"
  "    <method name='CommitFrame'>"
  "      <arg name='seq' type='t' direction='in'/>"
  "      <arg name='damage' type='a(iiii)' direction='in'/>"
  "    </method>"
  "    <method name='Stop'/>"
  "    <signal name='StateChanged'>"
  "      <arg name='state' type='s'/>"
  "      <arg name='reason' type='s'/>"
  "    </signal>"
  "    <property name='SinkId' type='s' access='read'/>"
  "    <property name='State' type='s' access='read'/>"
  "  </interface>"
  "</node>";

static gboolean
object_has_property (GObject     *object,
                     const gchar *name)
{
  return g_object_class_find_property (G_OBJECT_GET_CLASS (object), name) != NULL;
}

static const gchar *
widi_sink_state_to_string (NdSinkState state)
{
  switch (state)
    {
    case ND_SINK_STATE_DISCONNECTED:
      return "disconnected";
    case ND_SINK_STATE_ENSURE_FIREWALL:
      return "ensure-firewall";
    case ND_SINK_STATE_WAIT_P2P:
      return "wait-p2p";
    case ND_SINK_STATE_WAIT_SOCKET:
      return "wait-socket";
    case ND_SINK_STATE_WAIT_STREAMING:
      return "wait-streaming";
    case ND_SINK_STATE_STREAMING:
      return "streaming";
    case ND_SINK_STATE_ERROR:
      return "error";
    default:
      return "unknown";
    }
}

static const gchar *
widi_sink_protocol_to_string (NdSinkProtocol protocol)
{
  switch (protocol)
    {
    case ND_SINK_PROTOCOL_META:
      return "meta";
    case ND_SINK_PROTOCOL_WFD_P2P:
      return "wfd-p2p";
    case ND_SINK_PROTOCOL_WFD_MICE:
      return "wfd-mice";
    case ND_SINK_PROTOCOL_CC:
      return "cc";
    default:
      return "unknown";
    }
}

static void
widi_service_clear_sink_cache (WidiService *self)
{
  g_hash_table_remove_all (self->sinks_by_id);
  g_hash_table_remove_all (self->sink_ids_by_sink);
}

static const gchar *
widi_service_get_sink_id (WidiService *self,
                          NdSink      *sink)
{
  const gchar *id;

  if (!sink || !G_IS_OBJECT (sink))
    return NULL;

  id = g_hash_table_lookup (self->sink_ids_by_sink, sink);

  if (!id)
    {
      g_autofree gchar *new_id = NULL;

      new_id = g_strdup_printf ("sink-%u", self->next_sink_id++);

      g_hash_table_insert (self->sinks_by_id,
                           g_strdup (new_id),
                           g_object_ref (sink));

      g_hash_table_insert (self->sink_ids_by_sink,
                           sink,
                           g_strdup (new_id));

      id = g_hash_table_lookup (self->sink_ids_by_sink, sink);
    }

  return id;
}

static GVariant *
widi_service_sink_to_variant (WidiService *self,
                              NdSink      *sink)
{
  const gchar *id;
  g_autofree gchar *name = NULL;
  NdSinkState state = ND_SINK_STATE_DISCONNECTED;
  NdSinkProtocol protocol = ND_SINK_PROTOCOL_META;
  GVariantBuilder builder;

  id = widi_service_get_sink_id (self, sink);
  if (!id)
    return NULL;

  g_object_get (sink,
                "display-name", &name,
                "state", &state,
                "protocol", &protocol,
                NULL);

  if (!name || !*name)
    {
      g_free (name);
      name = g_strdup (id);
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}", "id", g_variant_new_string (id));
  g_variant_builder_add (&builder, "{sv}", "name", g_variant_new_string (name));
  g_variant_builder_add (&builder, "{sv}", "state", g_variant_new_string (widi_sink_state_to_string (state)));
  g_variant_builder_add (&builder, "{sv}", "type", g_variant_new_string (widi_sink_protocol_to_string (protocol)));

  return g_variant_builder_end (&builder);
}

static void
widi_service_emit_sink_added (WidiService *self,
                              NdSink      *sink)
{
  GVariant *sink_variant;

  if (!sink || !G_IS_OBJECT (sink) || !self->connection)
    return;

  sink_variant = widi_service_sink_to_variant (self, sink);
  if (!sink_variant)
    return;

  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 WIDI_MANAGER_PATH,
                                 "io.furios.WidiStream.Manager",
                                 "SinkAdded",
                                 g_variant_new ("(@a{sv})", sink_variant),
                                 NULL);
}

static void
widi_stream_emit_state (WidiStream  *stream,
                        const gchar *state,
                        const gchar *reason)
{
  g_free (stream->state);
  stream->state = g_strdup (state);

  if (!stream->service->connection)
    return;

  g_dbus_connection_emit_signal (stream->service->connection,
                                 NULL,
                                 stream->object_path,
                                 "io.furios.WidiStream.Stream",
                                 "StateChanged",
                                 g_variant_new ("(ss)", state, reason ? reason : ""),
                                 NULL);
}

static GstElement *
widi_stream_create_video_source_cb (WidiStream *stream,
                                    NdSink     *sink)
{
  GstElement *appsrc;

  appsrc = gst_element_factory_make ("appsrc", "widi-memfd-appsrc");

  if (!appsrc)
    {
      g_warning ("Could not create appsrc");
      return NULL;
    }

  stream->appsrc = appsrc;

  g_object_set (appsrc,
                "is-live", TRUE,
                "format", GST_FORMAT_TIME,
                "do-timestamp", TRUE,
                "block", TRUE,
                NULL);

  return gst_object_ref (appsrc);
}

static GstElement *
widi_stream_create_audio_source_cb (WidiStream *stream,
                                    NdSink     *sink)
{
  GstElement *res;

  if (!stream->service->pulse)
    return NULL;

  res = nd_pulseaudio_get_source (stream->service->pulse);

  if (res && GST_IS_ELEMENT (res))
    {
      GstElementFactory *factory = gst_element_get_factory (res);

      if (factory && g_strcmp0 (GST_OBJECT_NAME (factory), "pulsesrc") == 0)
        g_object_set (res,
                      "latency-time", 10000,
                      "buffer-time", 20000,
                      "provide-clock", FALSE,
                      NULL);
    }

  return res ? g_object_ref_sink (res) : NULL;
}

static void
widi_stream_sink_notify_state_cb (WidiStream *stream,
                                  GParamSpec *pspec,
                                  NdSink     *sink)
{
  NdSinkState state = ND_SINK_STATE_DISCONNECTED;
  const gchar *state_string;

  g_object_get (sink, "state", &state, NULL);

  state_string = widi_sink_state_to_string (state);

  widi_stream_emit_state (stream, state_string, "");

  if (state == ND_SINK_STATE_STREAMING)
    {
      GObjectClass *sink_class = G_OBJECT_GET_CLASS (sink);

      if (g_object_class_find_property (sink_class, "max-lateness"))
        g_object_set (sink, "max-lateness", (gint64) 16666667, NULL);

      if (g_object_class_find_property (sink_class, "qos"))
        g_object_set (sink, "qos", TRUE, NULL);

      if (g_object_class_find_property (sink_class, "processing-deadline"))
        g_object_set (sink, "processing-deadline", (gint64) 20000000, NULL);
    }
}

static void
widi_stream_cleanup (WidiStream *stream)
{
  if (!stream)
    return;

  if (stream->stream_sink)
    {
      nd_sink_stop_stream (stream->stream_sink);
      g_signal_handlers_disconnect_by_data (stream->stream_sink, stream);
      g_clear_object (&stream->stream_sink);
    }

  g_clear_object (&stream->sink);

  stream->appsrc = NULL;

  if (stream->memfd >= 0)
    close (stream->memfd);

  g_free (stream->id);
  g_free (stream->object_path);
  g_free (stream->sink_id);
  g_free (stream->format);
  g_free (stream->state);
  g_free (stream);
}

static WidiStream *
widi_stream_new (WidiService *service,
                 const gchar *sink_id,
                 NdSink      *sink)
{
  WidiStream *stream;

  stream = g_new0 (WidiStream, 1);
  stream->service = service;
  stream->id = g_strdup_printf ("stream_%u", service->next_stream_id++);
  stream->object_path = g_strdup_printf ("%s/%s", WIDI_STREAM_PREFIX, stream->id);
  stream->sink_id = g_strdup (sink_id);
  stream->sink = g_object_ref (sink);
  stream->memfd = -1;
  stream->state = g_strdup ("created");

  return stream;
}

static gboolean
widi_stream_start (WidiStream  *stream,
                   GError     **error)
{
  stream->stream_sink = nd_sink_start_stream (stream->sink);

  if (!stream->stream_sink)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Could not start stream");
      return FALSE;
    }

  g_signal_connect_data (stream->stream_sink,
                         "create-source",
                         G_CALLBACK (widi_stream_create_video_source_cb),
                         stream,
                         NULL,
                         G_CONNECT_SWAPPED);

  g_signal_connect_data (stream->stream_sink,
                         "create-audio-source",
                         G_CALLBACK (widi_stream_create_audio_source_cb),
                         stream,
                         NULL,
                         G_CONNECT_SWAPPED);

  if (object_has_property (G_OBJECT (stream->stream_sink), "state"))
    g_signal_connect_data (stream->stream_sink,
                           "notify::state",
                           G_CALLBACK (widi_stream_sink_notify_state_cb),
                           stream,
                           NULL,
                           G_CONNECT_SWAPPED);

  widi_stream_emit_state (stream, "connecting", "");

  return TRUE;
}

static gboolean
widi_stream_prepare_memfd (WidiStream  *stream,
                           guint        width,
                           guint        height,
                           guint        stride,
                           const gchar *format,
                           GError     **error)
{
  gsize size;
  gint fd;

  if (width == 0 || height == 0 || stride == 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid frame geometry");
      return FALSE;
    }

  if (g_strcmp0 (format, "BGRA") != 0 &&
      g_strcmp0 (format, "BGRx") != 0 &&
      g_strcmp0 (format, "RGBA") != 0 &&
      g_strcmp0 (format, "RGBx") != 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_ARGUMENT,
                   "Unsupported format '%s'; expected BGRA, BGRx, RGBA, or RGBx",
                   format);
      return FALSE;
    }

  size = (gsize) stride * (gsize) height;

  fd = memfd_create ("widi-framebuffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (errno),
                   "memfd_create failed: %s",
                   g_strerror (errno));
      return FALSE;
    }

  if (ftruncate (fd, size) < 0)
    {
      gint saved_errno = errno;

      close (fd);

      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (saved_errno),
                   "ftruncate failed: %s",
                   g_strerror (saved_errno));
      return FALSE;
    }

  if (stream->memfd >= 0)
    close (stream->memfd);

  stream->memfd = fd;
  stream->memfd_size = size;
  stream->width = width;
  stream->height = height;
  stream->stride = stride;

  g_free (stream->format);
  stream->format = g_strdup (format);

  if (stream->appsrc)
    {
      g_autofree gchar *caps_string = NULL;
      GstCaps *caps;

      caps_string = g_strdup_printf ("video/x-raw,format=%s,width=%u,height=%u,framerate=60/1",
                                     format,
                                     width,
                                     height);

      caps = gst_caps_from_string (caps_string);
      gst_app_src_set_caps (GST_APP_SRC (stream->appsrc), caps);
      gst_caps_unref (caps);
    }

  return TRUE;
}

static gboolean
widi_stream_commit_frame (WidiStream  *stream,
                          guint64      seq,
                          GVariant    *damage,
                          GError     **error)
{
  gpointer data;
  GstBuffer *buffer;
  GstMapInfo map;
  GstFlowReturn flow;

  if (stream->memfd < 0 || stream->memfd_size == 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "GetMemfd must be called before CommitFrame");
      return FALSE;
    }

  if (!stream->appsrc)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Stream appsrc is not ready yet");
      return FALSE;
    }

  data = mmap (NULL, stream->memfd_size, PROT_READ, MAP_SHARED, stream->memfd, 0);
  if (data == MAP_FAILED)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (errno),
                   "mmap failed: %s",
                   g_strerror (errno));
      return FALSE;
    }

  buffer = gst_buffer_new_allocate (NULL, stream->memfd_size, NULL);
  if (!buffer)
    {
      munmap (data, stream->memfd_size);

      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Could not allocate GstBuffer");
      return FALSE;
    }

  if (!gst_buffer_map (buffer, &map, GST_MAP_WRITE))
    {
      gst_buffer_unref (buffer);
      munmap (data, stream->memfd_size);

      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Could not map GstBuffer");
      return FALSE;
    }

  memcpy (map.data, data, stream->memfd_size);

  gst_buffer_unmap (buffer, &map);
  munmap (data, stream->memfd_size);

  GST_BUFFER_OFFSET (buffer) = seq;

  flow = gst_app_src_push_buffer (GST_APP_SRC (stream->appsrc), buffer);
  if (flow != GST_FLOW_OK)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "gst_app_src_push_buffer failed: %s",
                   gst_flow_get_name (flow));
      return FALSE;
    }

  return TRUE;
}

static GVariant *
manager_get_property (GDBusConnection  *connection,
                      const gchar      *sender,
                      const gchar      *object_path,
                      const gchar      *interface_name,
                      const gchar      *property_name,
                      GError          **error,
                      gpointer          user_data)
{
  WidiService *self = user_data;

  if (g_strcmp0 (property_name, "Discovering") == 0)
    return g_variant_new_boolean (self->discovering);

  return NULL;
}

static GVariant *
stream_get_property (GDBusConnection  *connection,
                     const gchar      *sender,
                     const gchar      *object_path,
                     const gchar      *interface_name,
                     const gchar      *property_name,
                     GError          **error,
                     gpointer          user_data)
{
  WidiService *self = user_data;
  WidiStream *stream;

  stream = g_hash_table_lookup (self->streams_by_path, object_path);

  if (!stream)
    return NULL;

  if (g_strcmp0 (property_name, "SinkId") == 0)
    return g_variant_new_string (stream->sink_id);

  if (g_strcmp0 (property_name, "State") == 0)
    return g_variant_new_string (stream->state);

  return NULL;
}

static void
sink_list_items_changed_cb (GListModel  *model,
                            guint        position,
                            guint        removed,
                            guint        added,
                            WidiService *self)
{
  guint i;

  if (removed > 0)
    widi_service_clear_sink_cache (self);

  if (!self->discovering)
    return;

  for (i = 0; i < added; i++)
    {
      g_autoptr(NdSink) sink = NULL;

      sink = g_list_model_get_item (model, position + i);
      if (!sink)
        continue;

      widi_service_emit_sink_added (self, sink);
    }
}

static void stream_method_call (GDBusConnection       *connection,
                                const gchar           *sender,
                                const gchar           *object_path,
                                const gchar           *interface_name,
                                const gchar           *method_name,
                                GVariant              *parameters,
                                GDBusMethodInvocation *invocation,
                                gpointer               user_data);

static const GDBusInterfaceVTable stream_vtable =
{
  stream_method_call,
  stream_get_property,
  NULL
};

static void
manager_method_call (GDBusConnection       *connection,
                     const gchar           *sender,
                     const gchar           *object_path,
                     const gchar           *interface_name,
                     const gchar           *method_name,
                     GVariant              *parameters,
                     GDBusMethodInvocation *invocation,
                     gpointer               user_data)
{
  WidiService *self = user_data;

  if (g_strcmp0 (method_name, "StartDiscovery") == 0)
    {
      self->discovering = TRUE;

      if (self->meta_provider &&
          object_has_property (G_OBJECT (self->meta_provider), "discover"))
        g_object_set (self->meta_provider, "discover", TRUE, NULL);

      widi_service_clear_sink_cache (self);

      g_dbus_method_invocation_return_value (invocation, NULL);
      return;
    }

  if (g_strcmp0 (method_name, "StopDiscovery") == 0)
    {
      self->discovering = FALSE;

      if (self->meta_provider &&
          object_has_property (G_OBJECT (self->meta_provider), "discover"))
        g_object_set (self->meta_provider, "discover", FALSE, NULL);

      g_dbus_method_invocation_return_value (invocation, NULL);
      return;
    }

  if (g_strcmp0 (method_name, "ListSinks") == 0)
    {
      GVariantBuilder array_builder;
      GHashTableIter iter;
      gpointer key;
      gpointer value;

      g_variant_builder_init (&array_builder, G_VARIANT_TYPE ("aa{sv}"));

      g_hash_table_iter_init (&iter, self->sinks_by_id);

      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          NdSink *sink = value;
          GVariant *sink_variant;

          sink_variant = widi_service_sink_to_variant (self, sink);
          if (!sink_variant)
            continue;

          g_variant_builder_add_value (&array_builder, sink_variant);
        }

      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(aa{sv})", &array_builder));
      return;
    }

  if (g_strcmp0 (method_name, "Connect") == 0)
    {
      const gchar *sink_id;
      NdSink *sink;
      WidiStream *stream;
      GDBusInterfaceInfo *stream_interface;
      g_autoptr(GError) error = NULL;
      guint registration_id;

      g_variant_get (parameters, "(&s)", &sink_id);

      sink = g_hash_table_lookup (self->sinks_by_id, sink_id);

      if (!sink)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_IO_ERROR,
                                                 G_IO_ERROR_NOT_FOUND,
                                                 "Unknown sink id '%s'",
                                                 sink_id);
          return;
        }

      stream = widi_stream_new (self, sink_id, sink);

      stream_interface = g_dbus_node_info_lookup_interface (self->introspection_data,
                                                            "io.furios.WidiStream.Stream");

      registration_id = g_dbus_connection_register_object (self->connection,
                                                           stream->object_path,
                                                           stream_interface,
                                                           &stream_vtable,
                                                           self,
                                                           NULL,
                                                           &error);

      if (registration_id == 0)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_IO_ERROR,
                                                 G_IO_ERROR_FAILED,
                                                 "Could not register stream object: %s",
                                                 error ? error->message : "invalid D-Bus object path");
          widi_stream_cleanup (stream);
          return;
        }

      g_hash_table_insert (self->stream_registration_ids,
                           g_strdup (stream->object_path),
                           GUINT_TO_POINTER (registration_id));

      g_hash_table_insert (self->streams_by_path,
                           g_strdup (stream->object_path),
                           stream);

      if (!widi_stream_start (stream, &error))
        {
          const gchar *stream_path = stream->object_path;
          gpointer registration_ptr;

          registration_ptr = g_hash_table_lookup (self->stream_registration_ids,
                                                  stream_path);

          if (registration_ptr)
            g_dbus_connection_unregister_object (self->connection,
                                                 GPOINTER_TO_UINT (registration_ptr));

          g_hash_table_remove (self->stream_registration_ids, stream_path);

          g_dbus_method_invocation_return_error (invocation,
                                                 G_IO_ERROR,
                                                 G_IO_ERROR_FAILED,
                                                 "%s",
                                                 error->message);

          g_hash_table_remove (self->streams_by_path, stream_path);
          return;
        }

      if (self->meta_provider &&
          object_has_property (G_OBJECT (self->meta_provider), "discover"))
        g_object_set (self->meta_provider, "discover", FALSE, NULL);

      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(o)", stream->object_path));
      return;
    }

  g_dbus_method_invocation_return_error (invocation,
                                         G_IO_ERROR,
                                         G_IO_ERROR_NOT_SUPPORTED,
                                         "Unknown method '%s'",
                                         method_name);
}

static void
stream_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
  WidiService *self = user_data;
  WidiStream *stream;

  stream = g_hash_table_lookup (self->streams_by_path, object_path);

  if (!stream)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_FOUND,
                                             "Unknown stream object");
      return;
    }

  if (g_strcmp0 (method_name, "GetMemfd") == 0)
    {
      guint width;
      guint height;
      guint stride;
      const gchar *format;
      g_autoptr(GError) error = NULL;
      g_autoptr(GUnixFDList) fd_list = NULL;
      gint handle;

      g_variant_get (parameters, "(uuu&s)", &width, &height, &stride, &format);

      if (!widi_stream_prepare_memfd (stream,
                                      width,
                                      height,
                                      stride,
                                      format,
                                      &error))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_IO_ERROR,
                                                 error->code,
                                                 "%s",
                                                 error->message);
          return;
        }

      fd_list = g_unix_fd_list_new ();
      handle = g_unix_fd_list_append (fd_list, stream->memfd, &error);

      if (handle < 0)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_IO_ERROR,
                                                 G_IO_ERROR_FAILED,
                                                 "Could not append fd: %s",
                                                 error->message);
          return;
        }

      g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
                                                               g_variant_new ("(h)", handle),
                                                               fd_list);
      return;
    }

  if (g_strcmp0 (method_name, "CommitFrame") == 0)
    {
      guint64 seq;
      GVariant *damage;
      g_autoptr(GError) error = NULL;

      g_variant_get (parameters, "(t@a(iiii))", &seq, &damage);

      if (!widi_stream_commit_frame (stream, seq, damage, &error))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_IO_ERROR,
                                                 error->code,
                                                 "%s",
                                                 error->message);
          g_variant_unref (damage);
          return;
        }

      g_variant_unref (damage);
      g_dbus_method_invocation_return_value (invocation, NULL);
      return;
    }

  if (g_strcmp0 (method_name, "Stop") == 0)
    {
      gpointer registration_ptr;

      widi_stream_emit_state (stream, "stopped", "");

      registration_ptr = g_hash_table_lookup (self->stream_registration_ids,
                                              object_path);

      if (registration_ptr)
        g_dbus_connection_unregister_object (self->connection,
                                             GPOINTER_TO_UINT (registration_ptr));

      g_hash_table_remove (self->stream_registration_ids, object_path);
      g_hash_table_remove (self->streams_by_path, object_path);

      if (self->meta_provider &&
          object_has_property (G_OBJECT (self->meta_provider), "discover"))
        g_object_set (self->meta_provider, "discover", TRUE, NULL);

      g_dbus_method_invocation_return_value (invocation, NULL);
      return;
    }

  g_dbus_method_invocation_return_error (invocation,
                                         G_IO_ERROR,
                                         G_IO_ERROR_NOT_SUPPORTED,
                                         "Unknown stream method '%s'",
                                         method_name);
}

static const GDBusInterfaceVTable manager_vtable =
{
  manager_method_call,
  manager_get_property,
  NULL
};

static void
pulse_init_cb (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  WidiService *self = user_data;
  g_autoptr(GError) error = NULL;

  if (!g_async_initable_init_finish (G_ASYNC_INITABLE (source_object), res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Error initializing PulseAudio: %s", error->message);

      g_object_unref (source_object);
      return;
    }

  self->pulse = ND_PULSEAUDIO (source_object);
}

static void
mtk_wifi_manager_init_cb (GObject      *source_object,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  WidiService *self = user_data;
  NdMtkWifiManager *wifi_manager = ND_MTK_WIFI_MANAGER (source_object);
  g_autoptr(GError) error = NULL;

  if (!nd_mtk_wifi_manager_init_finish (wifi_manager, res, &error))
    {
      g_debug ("MediaTek WiFi manager init failed: %s",
               error ? error->message : "unknown error");
      return;
    }

  if (nd_mtk_wifi_manager_is_available (wifi_manager))
    {
      nd_nm_device_registry_set_mtk_wifi_manager (self->nm_device_registry,
                                                  wifi_manager);
      nd_mtk_wifi_manager_ensure_p2p_mode (wifi_manager);
    }
}

static gboolean
widi_service_init_backends (WidiService  *self,
                            GError      **error)
{
  NdPulseaudio *pulse;

  self->cancellable = g_cancellable_new ();
  self->avahi_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);

  self->meta_provider = nd_meta_provider_new ();

  self->mtk_wifi_manager = nd_mtk_wifi_manager_new ();

  self->nm_device_registry = nd_nm_device_registry_new (self->meta_provider,
                                                        self->mtk_wifi_manager);

  nd_mtk_wifi_manager_init_async (self->mtk_wifi_manager,
                                  mtk_wifi_manager_init_cb,
                                  self);

  if (!ga_client_start (self->avahi_client, error))
    return FALSE;

  self->mice_provider = nd_wfd_mice_provider_new (self->avahi_client);
  self->cc_provider = nd_cc_provider_new (self->avahi_client);

  if (!nd_wfd_mice_provider_browse (self->mice_provider, error ? *error : NULL))
    return FALSE;

  if (!nd_cc_provider_browse (self->cc_provider, error ? *error : NULL))
    return FALSE;

  nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (self->mice_provider));
  nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (self->cc_provider));

  self->sink_list_model = nd_sink_list_model_new (ND_PROVIDER (self->meta_provider));

  g_signal_connect (self->sink_list_model,
                    "items-changed",
                    G_CALLBACK (sink_list_items_changed_cb),
                    self);

  pulse = nd_pulseaudio_new ("WiDiStream", "widistream");
  g_async_initable_init_async (G_ASYNC_INITABLE (pulse),
                               G_PRIORITY_LOW,
                               self->cancellable,
                               pulse_init_cb,
                               self);

  self->discovering = FALSE;

  if (object_has_property (G_OBJECT (self->meta_provider), "discover"))
    g_object_set (self->meta_provider, "discover", FALSE, NULL);

  return TRUE;
}

static void
bus_acquired_cb (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  WidiService *self = user_data;
  GDBusInterfaceInfo *manager_interface;
  g_autoptr(GError) error = NULL;

  self->connection = g_object_ref (connection);

  manager_interface = g_dbus_node_info_lookup_interface (self->introspection_data,
                                                         "io.furios.WidiStream.Manager");

  self->manager_registration_id =
    g_dbus_connection_register_object (connection,
                                       WIDI_MANAGER_PATH,
                                       manager_interface,
                                       &manager_vtable,
                                       self,
                                       NULL,
                                       &error);

  if (self->manager_registration_id == 0)
    {
      g_warning ("Could not register manager object: %s", error->message);
      g_main_loop_quit (self->loop);
      return;
    }

  if (!widi_service_init_backends (self, &error))
    {
      g_warning ("Could not initialize service backends: %s",
                 error ? error->message : "unknown error");
      g_main_loop_quit (self->loop);
      return;
    }

  g_debug ("WiDiStream D-Bus service ready on %s", WIDI_BUS_NAME);
}

static void
name_acquired_cb (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("Acquired D-Bus name %s", name);
}

static void
name_lost_cb (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  WidiService *self = user_data;

  g_warning ("Lost D-Bus name %s", name);
  g_main_loop_quit (self->loop);
}

static WidiService *
widi_service_new (void)
{
  WidiService *self;
  g_autoptr(GError) error = NULL;

  self = g_new0 (WidiService, 1);
  self->loop = g_main_loop_new (NULL, FALSE);

  self->introspection_data = g_dbus_node_info_new_for_xml (introspection_xml,
                                                           &error);
  if (!self->introspection_data)
    g_error ("Could not parse introspection XML: %s", error->message);

  self->sinks_by_id = g_hash_table_new_full (g_str_hash,
                                             g_str_equal,
                                             g_free,
                                             g_object_unref);

  self->sink_ids_by_sink = g_hash_table_new_full (g_direct_hash,
                                                  g_direct_equal,
                                                  NULL,
                                                  g_free);

  self->streams_by_path = g_hash_table_new_full (g_str_hash,
                                                 g_str_equal,
                                                 g_free,
                                                 (GDestroyNotify) widi_stream_cleanup);

  self->stream_registration_ids = g_hash_table_new_full (g_str_hash,
                                                         g_str_equal,
                                                         g_free,
                                                         NULL);

  self->next_sink_id = 1;
  self->next_stream_id = 1;

  return self;
}

static void
widi_service_cleanup (WidiService *self)
{
  if (!self)
    return;

  if (self->owner_id)
    g_bus_unown_name (self->owner_id);

  if (self->connection && self->manager_registration_id)
    g_dbus_connection_unregister_object (self->connection,
                                         self->manager_registration_id);

  g_clear_pointer (&self->stream_registration_ids, g_hash_table_unref);
  g_clear_pointer (&self->streams_by_path, g_hash_table_unref);
  g_clear_pointer (&self->sinks_by_id, g_hash_table_unref);
  g_clear_pointer (&self->sink_ids_by_sink, g_hash_table_unref);

  if (self->pulse)
    {
      nd_pulseaudio_unload (self->pulse);
      g_clear_object (&self->pulse);
    }

  if (self->mtk_wifi_manager)
    {
      nd_mtk_wifi_manager_restore_ap_mode (self->mtk_wifi_manager);
      g_clear_object (&self->mtk_wifi_manager);
    }

  if (self->cancellable)
    {
      g_cancellable_cancel (self->cancellable);
      g_clear_object (&self->cancellable);
    }

  g_clear_object (&self->sink_list_model);
  g_clear_object (&self->nm_device_registry);
  g_clear_object (&self->mice_provider);
  g_clear_object (&self->cc_provider);
  g_clear_object (&self->meta_provider);

  if (self->avahi_client)
    {
      g_object_run_dispose (G_OBJECT (self->avahi_client));
      g_clear_object (&self->avahi_client);
    }

  g_clear_object (&self->connection);
  g_clear_pointer (&self->introspection_data, g_dbus_node_info_unref);
  g_clear_pointer (&self->loop, g_main_loop_unref);

  g_free (self);
}

int
main (int   argc,
      char *argv[])
{
  WidiService *service;

  gst_init (&argc, &argv);

  service = widi_service_new ();

  service->owner_id =
    g_bus_own_name (G_BUS_TYPE_SESSION,
                    WIDI_BUS_NAME,
                    G_BUS_NAME_OWNER_FLAGS_NONE,
                    bus_acquired_cb,
                    name_acquired_cb,
                    name_lost_cb,
                    service,
                    NULL);

  g_main_loop_run (service->loop);

  widi_service_cleanup (service);

  return 0;
}
