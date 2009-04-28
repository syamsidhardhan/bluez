/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <glib.h>

#include <dbus/dbus.h>

#include <gdbus.h>

#include "dbus-common.h"
#include "logging.h"
#include "adapter.h"
#include "error.h"
#include "manager.h"

static char base_path[50] = "/org/bluez";

static DBusConnection *connection = NULL;
static int default_adapter_id = -1;
static GSList *adapters = NULL;

const struct btd_adapter_ops *adapter_ops = NULL;

const char *manager_get_base_path(void)
{
	return base_path;
}

int manager_update_adapter(uint16_t dev_id, uint8_t svc, gboolean starting)
{
	struct btd_adapter *adapter;

	adapter = manager_find_adapter_by_id(dev_id);
	if (!adapter)
		return -EINVAL;

	return adapter_update(adapter, svc, starting);
}

int manager_startup_complete(void)
{
	GSList *l;

	for (l = adapters; l != NULL; l = l->next) {
		struct btd_adapter *adapter = l->data;
		adapter_update(adapter, 0, FALSE);
	}

	return 0;
}

int manager_get_adapter_class(uint16_t dev_id, uint8_t *cls)
{
	struct btd_adapter *adapter;

	adapter = manager_find_adapter_by_id(dev_id);
	if (!adapter)
		return -EINVAL;

	return adapter_get_class(adapter, cls);
}

static inline DBusMessage *invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg,
			ERROR_INTERFACE ".InvalidArguments",
			"Invalid arguments in method call");
}

static inline DBusMessage *no_such_adapter(DBusMessage *msg)
{
	return g_dbus_create_error(msg,
			ERROR_INTERFACE ".NoSuchAdapter",
			"No such adapter");
}

static DBusMessage *default_adapter(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	struct btd_adapter *adapter;
	const gchar *path;

	adapter = manager_find_adapter_by_id(default_adapter_id);
	if (!adapter)
		return no_such_adapter(msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	path = adapter_get_path(adapter);

	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *find_adapter(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	struct btd_adapter *adapter;
	const char *pattern;
	int dev_id;
	const gchar *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pattern,
							DBUS_TYPE_INVALID))
		return NULL;

	/* hci_devid() would make sense to use here, except it
	   is restricted to devices which are up */
	if (!strcmp(pattern, "any") || !strcmp(pattern, "00:00:00:00:00:00")) {
		path = adapter_any_get_path();
		if (path != NULL)
			goto done;
		return no_such_adapter(msg);
	} else if (!strncmp(pattern, "hci", 3) && strlen(pattern) >= 4) {
		dev_id = atoi(pattern + 3);
		adapter = manager_find_adapter_by_id(dev_id);
	} else
		adapter = manager_find_adapter_by_address(pattern);

	if (!adapter)
		return no_such_adapter(msg);

	path = adapter_get_path(adapter);

done:
	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *list_adapters(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	DBusMessage *reply;
	GSList *l;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH_AS_STRING, &array_iter);

	for (l = adapters; l; l = l->next) {
		struct btd_adapter *adapter = l->data;
		const gchar *path = adapter_get_path(adapter);

		dbus_message_iter_append_basic(&array_iter,
					DBUS_TYPE_OBJECT_PATH, &path);
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	return reply;
}

static DBusMessage *get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	GSList *list;
	char **array;
	int i;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	array = g_new0(char *, g_slist_length(adapters) + 1);
	for (i = 0, list = adapters; list; list = list->next, i++) {
		struct btd_adapter *adapter = list->data;

		if (!adapter_is_ready(adapter))
			continue;

		array[i] = (char *) adapter_get_path(adapter);
	}
	dict_append_array(&dict, "Adapters", DBUS_TYPE_OBJECT_PATH, &array, i);
	g_free(array);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable manager_methods[] = {
	{ "GetProperties",	"",	"a{sv}",get_properties	},
	{ "DefaultAdapter",	"",	"o",	default_adapter	},
	{ "FindAdapter",	"s",	"o",	find_adapter	},
	{ "ListAdapters",	"",	"ao",	list_adapters	},
	{ }
};

static GDBusSignalTable manager_signals[] = {
	{ "PropertyChanged",		"sv"	},
	{ "AdapterAdded",		"o"	},
	{ "AdapterRemoved",		"o"	},
	{ "DefaultAdapterChanged",	"o"	},
	{ }
};

dbus_bool_t manager_init(DBusConnection *conn, const char *path)
{
	connection = conn;

	snprintf(base_path, sizeof(base_path), "/org/bluez/%d", getpid());

	return g_dbus_register_interface(conn, "/", MANAGER_INTERFACE,
			manager_methods, manager_signals,
			NULL, NULL, NULL);
}

static void manager_update_adapters(void)
{
	GSList *list;
	char **array;
	int i;

	array = g_new0(char *, g_slist_length(adapters) + 1);
	for (i = 0, list = adapters; list; list = list->next, i++) {
		struct btd_adapter *adapter = list->data;

		if (!adapter_is_ready(adapter))
			continue;

		array[i] = (char *) adapter_get_path(adapter);
	}

	emit_array_property_changed(connection, "/",
					MANAGER_INTERFACE, "Adapters",
					DBUS_TYPE_OBJECT_PATH, &array);

	g_free(array);
}

static void manager_remove_adapter(struct btd_adapter *adapter)
{
	uint16_t dev_id = adapter_get_dev_id(adapter);
	const gchar *path = adapter_get_path(adapter);

	adapters = g_slist_remove(adapters, adapter);

	manager_update_adapters();

	if (default_adapter_id == dev_id || default_adapter_id < 0) {
		int new_default = hci_get_route(NULL);

		manager_set_default_adapter(new_default);
	}

	g_dbus_emit_signal(connection, "/",
			MANAGER_INTERFACE, "AdapterRemoved",
			DBUS_TYPE_OBJECT_PATH, &path,
			DBUS_TYPE_INVALID);

	adapter_remove(adapter);
}

void manager_cleanup(DBusConnection *conn, const char *path)
{
	g_slist_foreach(adapters, (GFunc) manager_remove_adapter, NULL);
	g_slist_free(adapters);

	g_dbus_unregister_interface(conn, "/", MANAGER_INTERFACE);
}

static gint adapter_id_cmp(gconstpointer a, gconstpointer b)
{
	struct btd_adapter *adapter = (struct btd_adapter *) a;
	uint16_t id = GPOINTER_TO_UINT(b);
	uint16_t dev_id = adapter_get_dev_id(adapter);

	return dev_id == id ? 0 : -1;
}

static gint adapter_path_cmp(gconstpointer a, gconstpointer b)
{
	struct btd_adapter *adapter = (struct btd_adapter *) a;
	const char *path = b;
	const gchar *adapter_path = adapter_get_path(adapter);

	return strcmp(adapter_path, path);
}

static gint adapter_cmp(gconstpointer a, gconstpointer b)
{
	struct btd_adapter *adapter = (struct btd_adapter *) a;
	const bdaddr_t *bdaddr = b;
	bdaddr_t src;

	adapter_get_address(adapter, &src);

	return bacmp(&src, bdaddr);
}

static gint adapter_address_cmp(gconstpointer a, gconstpointer b)
{
	struct btd_adapter *adapter = (struct btd_adapter *) a;
	const char *address = b;
	bdaddr_t bdaddr;
	char addr[18];

	adapter_get_address(adapter, &bdaddr);
	ba2str(&bdaddr, addr);

	return strcmp(addr, address);
}

struct btd_adapter *manager_find_adapter(const bdaddr_t *sba)
{
	GSList *match;

	match = g_slist_find_custom(adapters, sba, adapter_cmp);
	if (!match)
		return NULL;

	return match->data;
}

struct btd_adapter *manager_find_adapter_by_address(const char *address)
{
	GSList *match;

	match = g_slist_find_custom(adapters, address, adapter_address_cmp);
	if (!match)
		return NULL;

	return match->data;
}

struct btd_adapter *manager_find_adapter_by_path(const char *path)
{
	GSList *match;

	match = g_slist_find_custom(adapters, path, adapter_path_cmp);
	if (!match)
		return NULL;

	return match->data;
}

struct btd_adapter *manager_find_adapter_by_id(int id)
{
	GSList *match;

	match = g_slist_find_custom(adapters, GINT_TO_POINTER(id), adapter_id_cmp);
	if (!match)
		return NULL;

	return match->data;
}

GSList *manager_get_adapters(void)
{
	return adapters;
}

void manager_add_adapter(const char *path)
{
	g_dbus_emit_signal(connection, "/",
			MANAGER_INTERFACE, "AdapterAdded",
			DBUS_TYPE_OBJECT_PATH, &path,
			DBUS_TYPE_INVALID);

	manager_update_adapters();
}

int manager_register_adapter(int id, gboolean devup)
{
	struct btd_adapter *adapter;

	adapter = manager_find_adapter_by_id(id);
	if (adapter) {
		error("Unable to register adapter: hci%d already exist", id);
		return -1;
	}

	adapter = adapter_create(connection, id, devup);
	if (!adapter)
		return -1;

	adapters = g_slist_append(adapters, adapter);

	return 0;
}

int manager_unregister_adapter(int id)
{
	struct btd_adapter *adapter;
	const gchar *path;

	adapter = manager_find_adapter_by_id(id);
	if (!adapter)
		return -1;

	path = adapter_get_path(adapter);

	info("Unregister path: %s", path);

	manager_remove_adapter(adapter);

	return 0;
}

int manager_start_adapter(int id)
{
	struct btd_adapter* adapter;
	int ret;

	adapter = manager_find_adapter_by_id(id);
	if (!adapter) {
		error("Getting device data failed: hci%d", id);
		return -EINVAL;
	}

	ret = adapter_start(adapter);
	if (ret < 0)
		return ret;

	if (default_adapter_id < 0)
		manager_set_default_adapter(id);

	return ret;
}

int manager_stop_adapter(int id)
{
	struct btd_adapter *adapter;

	adapter = manager_find_adapter_by_id(id);
	if (!adapter) {
		error("Getting device data failed: hci%d", id);
		return -EINVAL;
	}

	return adapter_stop(adapter);
}

int manager_get_default_adapter()
{
	return default_adapter_id;
}

void manager_set_default_adapter(int id)
{
	struct btd_adapter *adapter;
	const gchar *path;

	default_adapter_id = id;

	adapter = manager_find_adapter_by_id(id);
	if (!adapter)
		return;

	path = adapter_get_path(adapter);

	g_dbus_emit_signal(connection, "/",
			MANAGER_INTERFACE,
			"DefaultAdapterChanged",
			DBUS_TYPE_OBJECT_PATH, &path,
			DBUS_TYPE_INVALID);
}

int btd_register_adapter_ops(struct btd_adapter_ops *btd_adapter_ops)
{
	/* Already registered */
	if (adapter_ops)
		return -EALREADY;

	if (btd_adapter_ops->setup == NULL)
		return -EINVAL;

	adapter_ops = btd_adapter_ops;

	return 0;
}

void btd_adapter_cleanup_ops()
{
	adapter_ops->cleanup();
}

void manager_init_adapters()
{
	int err;

	if (!adapter_ops) {
		info("No adapter_ops registered.");
		return;
	}

	err = adapter_ops->setup();
	if (err < 0)
		error("btd_register_adapter_ops failed:%s (%d)", strerror(-err), -err);
}
