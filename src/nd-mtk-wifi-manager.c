/*
 * Copyright 2025 Bardia Moshiri <bardia@furilabs.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "nd-mtk-wifi-manager.h"
#include <glib/gi18n.h>

struct _NdMtkWifiManager
{
  GObject parent_instance;

  GDBusConnection *connection;
  GDBusProxy *proxy;

  NdWiFiState original_state;
  NdWiFiState current_state;

  gboolean is_available;
  gboolean state_changed_by_us;
};

G_DEFINE_FINAL_TYPE (NdMtkWifiManager, nd_mtk_wifi_manager, G_TYPE_OBJECT)

typedef struct {
  NdMtkWifiManager *manager;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GTask *task;
} InitAsyncData;

static void
on_state_changed_signal (GDBusProxy *proxy,
                         gchar *sender_name,
                         gchar *signal_name,
                         GVariant *parameters,
                         gpointer user_data)
{
  NdMtkWifiManager *self = ND_MTK_WIFI_MANAGER (user_data);
  guint32 new_state;

  if (g_strcmp0 (signal_name, "StateChanged") != 0)
    return;

  g_variant_get (parameters, "(u)", &new_state);
  self->current_state = (NdWiFiState) new_state;

  g_debug ("NdMtkWifiManager: WiFi state changed to %u", new_state);
}

static void
on_proxy_ready (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  InitAsyncData *data = (InitAsyncData *) user_data;
  NdMtkWifiManager *self = data->manager;
  g_autoptr(GError) error = NULL;
  GVariant *state_variant;

  self->proxy = g_dbus_proxy_new_finish (res, &error);

  if (!self->proxy) {
    g_debug ("NdMtkWifiManager: Failed to create D-Bus proxy: %s",
             error ? error->message : "Unknown error");
    self->is_available = FALSE;

    if (data->task) {
      g_task_return_boolean (data->task, TRUE); /* Success even if not available */
      g_object_unref (data->task);
    }
    g_free (data);
    return;
  }

  self->is_available = TRUE;

  g_signal_connect (self->proxy,
                    "g-signal",
                    G_CALLBACK (on_state_changed_signal),
                    self);

  state_variant = g_dbus_proxy_get_cached_property (self->proxy, "State");
  if (state_variant) {
    self->current_state = g_variant_get_uint32 (state_variant);
    self->original_state = self->current_state;
    g_variant_unref (state_variant);
    g_debug ("NdMtkWifiManager: Current WiFi state is %u", self->current_state);
  } else {
    g_warning ("NdMtkWifiManager: Could not get current WiFi state");
    self->current_state = ND_WIFI_STATE_AP; /* Default fallback */
    self->original_state = ND_WIFI_STATE_AP;
  }

  if (data->task) {
    g_task_return_boolean (data->task, TRUE);
    g_object_unref (data->task);
  }

  g_free (data);
}

static void
on_bus_ready (GObject *source_object,
              GAsyncResult *res,
              gpointer user_data)
{
  InitAsyncData *data = (InitAsyncData *) user_data;
  NdMtkWifiManager *self = data->manager;
  g_autoptr(GError) error = NULL;

  self->connection = g_bus_get_finish (res, &error);

  if (!self->connection) {
    g_debug ("NdMtkWifiManager: Failed to get system bus: %s",
             error ? error->message : "Unknown error");
    self->is_available = FALSE;

    if (data->task) {
      g_task_return_boolean (data->task, TRUE); /* Success even if not available */
      g_object_unref (data->task);
    }
    g_free (data);
    return;
  }

  g_dbus_proxy_new (self->connection,
                    G_DBUS_PROXY_FLAGS_NONE,
                    NULL,
                    "com.MediaTek.WiFiManager",
                    "/com/MediaTek/WiFiManager",
                    "com.MediaTek.WiFiManager",
                    NULL,
                    on_proxy_ready,
                    data);
}

gboolean
nd_mtk_wifi_manager_init_async (NdMtkWifiManager *self,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
  InitAsyncData *data;

  g_return_val_if_fail (ND_IS_MTK_WIFI_MANAGER (self), FALSE);

  data = g_new0 (InitAsyncData, 1);
  data->manager = self;
  data->callback = callback;
  data->user_data = user_data;

  if (callback)
    data->task = g_task_new (self, NULL, callback, user_data);

  g_bus_get (G_BUS_TYPE_SYSTEM, NULL, on_bus_ready, data);

  return TRUE;
}

gboolean
nd_mtk_wifi_manager_init_finish (NdMtkWifiManager *self,
                                 GAsyncResult *result,
                                 GError **error)
{
  g_return_val_if_fail (ND_IS_MTK_WIFI_MANAGER (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
set_state_cb (GObject *source_object,
              GAsyncResult *res,
              gpointer user_data)
{
  GDBusProxy *proxy = G_DBUS_PROXY (source_object);
  NdMtkWifiManager *self = ND_MTK_WIFI_MANAGER (user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) result = NULL;

  result = g_dbus_proxy_call_finish (proxy, res, &error);

  if (error) {
    g_warning ("NdMtkWifiManager: Failed to set WiFi state: %s", error->message);
  } else {
    g_debug ("NdMtkWifiManager: Successfully set WiFi state");
    self->state_changed_by_us = TRUE;
  }
}

static void
set_wifi_state (NdMtkWifiManager *self, NdWiFiState state)
{
  if (!self->is_available || !self->proxy) {
    g_debug ("NdMtkWifiManager: WiFi manager not available, cannot set state");
    return;
  }

  if (self->current_state == state) {
    g_debug ("NdMtkWifiManager: Already in desired state %u", state);
    return;
  }

  g_debug ("NdMtkWifiManager: Setting WiFi state to %u", state);

  g_dbus_proxy_call (self->proxy,
                     "SetState",
                     g_variant_new ("(u)", (guint32) state),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL,
                     set_state_cb,
                     self);
}

static void
refresh_p2p_cb (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  GDBusProxy *proxy = G_DBUS_PROXY (source_object);
  GTask *task = G_TASK (user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) result = NULL;
  gboolean success = FALSE;

  result = g_dbus_proxy_call_finish (proxy, res, &error);

  if (error) {
    g_debug ("NdMtkWifiManager: Failed to refresh P2P: %s", error->message);
    g_task_return_error (task, g_steal_pointer (&error));
  } else {
    g_variant_get (result, "(b)", &success);
    g_debug ("NdMtkWifiManager: P2P refresh completed with success: %s",
             success ? "true" : "false");

    if (success)
      g_task_return_boolean (task, TRUE);
    else
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_FAILED,
                               "P2P refresh operation failed");
  }

  g_object_unref (task);
}

void
nd_mtk_wifi_manager_refresh_p2p_async (NdMtkWifiManager *self,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  GTask *task;

  g_return_if_fail (ND_IS_MTK_WIFI_MANAGER (self));

  task = g_task_new (self, NULL, callback, user_data);

  if (!self->is_available || !self->proxy) {
    g_debug ("NdMtkWifiManager: WiFi manager not available, cannot refresh P2P");
    g_task_return_new_error (task,
                             G_IO_ERROR,
                             G_IO_ERROR_NOT_CONNECTED,
                             "WiFi manager service not available");
    g_object_unref (task);
    return;
  }

  g_debug ("NdMtkWifiManager: Starting P2P refresh");

  g_dbus_proxy_call (self->proxy,
                     "WpaRefreshP2P",
                     NULL,
                     G_DBUS_CALL_FLAGS_NONE,
                     30000, /* 30 second timeout for P2P operations */
                     NULL,
                     refresh_p2p_cb,
                     task);
}

gboolean
nd_mtk_wifi_manager_refresh_p2p_finish (NdMtkWifiManager *self,
                                        GAsyncResult *result,
                                        GError **error)
{
  g_return_val_if_fail (ND_IS_MTK_WIFI_MANAGER (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
nd_mtk_wifi_manager_ensure_p2p_mode (NdMtkWifiManager *self)
{
  g_return_if_fail (ND_IS_MTK_WIFI_MANAGER (self));

  if (!self->is_available) {
    g_debug ("NdMtkWifiManager: WiFi manager not available for P2P mode");
    return;
  }

  g_debug ("NdMtkWifiManager: Ensuring P2P mode (current: %u, original: %u)",
           self->current_state, self->original_state);

  if (self->current_state != ND_WIFI_STATE_P2P) {
    /* Store original state if we haven't changed it yet */
    if (!self->state_changed_by_us)
      self->original_state = self->current_state;
    set_wifi_state (self, ND_WIFI_STATE_P2P);
  }
}

void
nd_mtk_wifi_manager_restore_ap_mode (NdMtkWifiManager *self)
{
  g_return_if_fail (ND_IS_MTK_WIFI_MANAGER (self));

  if (!self->is_available) {
    g_debug ("NdMtkWifiManager: WiFi manager not available for AP mode restore");
    return;
  }

  /* If original state was P2P, default to AP mode when closing */
  NdWiFiState target_state = (self->original_state == ND_WIFI_STATE_P2P) ?
                              ND_WIFI_STATE_AP : self->original_state;

  g_debug ("NdMtkWifiManager: Restoring WiFi state to %u (original was %u, current is %u)",
           target_state, self->original_state, self->current_state);

  /* Always restore to a non-P2P state when app closes */
  if (self->current_state != target_state) {
    set_wifi_state (self, target_state);
    self->state_changed_by_us = FALSE; /* Mark as restored */
  } else {
    g_debug ("NdMtkWifiManager: Already in target state %u, no change needed", target_state);
  }
}

NdWiFiState
nd_mtk_wifi_manager_get_current_state (NdMtkWifiManager *self)
{
  g_return_val_if_fail (ND_IS_MTK_WIFI_MANAGER (self), ND_WIFI_STATE_AP);

  return self->current_state;
}

gboolean
nd_mtk_wifi_manager_is_available (NdMtkWifiManager *self)
{
  g_return_val_if_fail (ND_IS_MTK_WIFI_MANAGER (self), FALSE);

  return self->is_available;
}

static void
nd_mtk_wifi_manager_finalize (GObject *object)
{
  NdMtkWifiManager *self = ND_MTK_WIFI_MANAGER (object);

  /* Restore original WiFi state on cleanup */
  nd_mtk_wifi_manager_restore_ap_mode (self);

  g_clear_object (&self->proxy);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (nd_mtk_wifi_manager_parent_class)->finalize (object);
}

static void
nd_mtk_wifi_manager_class_init (NdMtkWifiManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = nd_mtk_wifi_manager_finalize;
}

static void
nd_mtk_wifi_manager_init (NdMtkWifiManager *self)
{
  self->connection = NULL;
  self->proxy = NULL;
  self->original_state = ND_WIFI_STATE_AP;
  self->current_state = ND_WIFI_STATE_AP;
  self->is_available = FALSE;
  self->state_changed_by_us = FALSE;
}

NdMtkWifiManager *
nd_mtk_wifi_manager_new (void)
{
  return g_object_new (ND_TYPE_MTK_WIFI_MANAGER, NULL);
}
