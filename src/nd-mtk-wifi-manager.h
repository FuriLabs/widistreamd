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

#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * NdWiFiState:
 * @ND_WIFI_STATE_AP: Access Point mode
 * @ND_WIFI_STATE_P2P: Peer-to-Peer mode
 * @ND_WIFI_STATE_DUAL_AP: Dual Access Point mode
 * @ND_WIFI_STATE_DUAL_P2P: Dual Peer-to-Peer mode
 * @ND_WIFI_STATE_ON: WiFi on
 * @ND_WIFI_STATE_OFF: WiFi off
 *
 * WiFi state enumeration for MediaTek WiFi manager.
 */
typedef enum {
    ND_WIFI_STATE_AP = 1,
    ND_WIFI_STATE_P2P = 2,
    ND_WIFI_STATE_DUAL_AP = 3,
    ND_WIFI_STATE_DUAL_P2P = 4,
    ND_WIFI_STATE_ON = 5,
    ND_WIFI_STATE_OFF = 6
} NdWiFiState;

#define ND_TYPE_MTK_WIFI_MANAGER (nd_mtk_wifi_manager_get_type())
G_DECLARE_FINAL_TYPE (NdMtkWifiManager, nd_mtk_wifi_manager, ND, MTK_WIFI_MANAGER, GObject)

/**
 * nd_mtk_wifi_manager_new:
 *
 * Creates a new MediaTek WiFi manager instance.
 *
 * Returns: (transfer full): A new NdMtkWifiManager
 */
NdMtkWifiManager *nd_mtk_wifi_manager_new (void);

/**
 * nd_mtk_wifi_manager_init_async:
 * @self: An NdMtkWifiManager
 * @callback: Callback to call when initialization is complete
 * @user_data: User data for the callback
 *
 * Asynchronously initializes the MediaTek WiFi manager by connecting
 * to the D-Bus service and retrieving the current WiFi state.
 *
 * Returns: %TRUE if initialization started successfully
 */
gboolean nd_mtk_wifi_manager_init_async (NdMtkWifiManager *self,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

/**
 * nd_mtk_wifi_manager_init_finish:
 * @self: An NdMtkWifiManager
 * @result: A GAsyncResult
 * @error: (out) (optional): Error return location
 *
 * Finishes the asynchronous initialization of the MediaTek WiFi manager.
 *
 * Returns: %TRUE if initialization completed successfully
 */
gboolean nd_mtk_wifi_manager_init_finish (NdMtkWifiManager *self,
                                          GAsyncResult *result,
                                          GError **error);

/**
 * nd_mtk_wifi_manager_ensure_p2p_mode:
 * @self: An NdMtkWifiManager
 *
 * Ensures the WiFi is in P2P mode. If not currently in P2P mode,
 * switches to P2P mode and stores the original state for later restoration.
 */
void nd_mtk_wifi_manager_ensure_p2p_mode (NdMtkWifiManager *self);

/**
 * nd_mtk_wifi_manager_restore_ap_mode:
 * @self: An NdMtkWifiManager
 *
 * Restores the WiFi to its original state (typically AP mode) if it was
 * changed by this manager.
 */
void nd_mtk_wifi_manager_restore_ap_mode (NdMtkWifiManager *self);

/**
 * nd_mtk_wifi_manager_refresh_p2p_async:
 * @self: An NdMtkWifiManager
 * @callback: (nullable): Callback to call when refresh is complete
 * @user_data: User data for the callback
 *
 * Asynchronously refreshes P2P by removing all P2P groups, flushing
 * the P2P peer table, and starting P2P discovery.
 */
void nd_mtk_wifi_manager_refresh_p2p_async (NdMtkWifiManager *self,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

/**
 * nd_mtk_wifi_manager_refresh_p2p_finish:
 * @self: An NdMtkWifiManager
 * @result: A GAsyncResult
 * @error: (out) (optional): Error return location
 *
 * Finishes the asynchronous P2P refresh operation.
 *
 * Returns: %TRUE if the P2P refresh completed successfully
 */
gboolean nd_mtk_wifi_manager_refresh_p2p_finish (NdMtkWifiManager *self,
                                                 GAsyncResult *result,
                                                 GError **error);

/**
 * nd_mtk_wifi_manager_get_current_state:
 * @self: An NdMtkWifiManager
 *
 * Gets the current WiFi state.
 *
 * Returns: The current NdWiFiState
 */
NdWiFiState nd_mtk_wifi_manager_get_current_state (NdMtkWifiManager *self);

/**
 * nd_mtk_wifi_manager_is_available:
 * @self: An NdMtkWifiManager
 *
 * Checks if the MediaTek WiFi manager D-Bus service is available.
 *
 * Returns: %TRUE if the service is available
 */
gboolean nd_mtk_wifi_manager_is_available (NdMtkWifiManager *self);

G_END_DECLS
