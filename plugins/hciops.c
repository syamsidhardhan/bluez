/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
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

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <glib.h>

#include <dbus/dbus.h>

#include "hcid.h"
#include "sdpd.h"
#include "adapter.h"
#include "plugin.h"
#include "logging.h"
#include "manager.h"
#include "storage.h"

static int child_pipe[2];

static gboolean child_exit(GIOChannel *io, GIOCondition cond, void *user_data)
{
	int status, fd = g_io_channel_unix_get_fd(io);
	pid_t child_pid;

	if (read(fd, &child_pid, sizeof(child_pid)) != sizeof(child_pid)) {
		error("child_exit: unable to read child pid from pipe");
		return TRUE;
	}

	if (waitpid(child_pid, &status, 0) != child_pid)
		error("waitpid(%d) failed", child_pid);
	else
		debug("child %d exited", child_pid);

	return TRUE;
}

static void at_child_exit(void)
{
	pid_t pid = getpid();

	if (write(child_pipe[1], &pid, sizeof(pid)) != sizeof(pid))
		error("unable to write to child pipe");
}

static void configure_device(int dev_id)
{
	struct hci_dev_info di;
	uint16_t policy;
	int dd;

	if (hci_devinfo(dev_id, &di) < 0)
		return;

	if (hci_test_bit(HCI_RAW, &di.flags))
		return;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		error("Can't open device hci%d: %s (%d)",
						dev_id, strerror(errno), errno);
		return;
	}

	/* Set device name */
	if ((main_opts.flags & (1 << HCID_SET_NAME)) && main_opts.name) {
		change_local_name_cp cp;

		memset(cp.name, 0, sizeof(cp.name));
		expand_name((char *) cp.name, sizeof(cp.name),
						main_opts.name, dev_id);

		hci_send_cmd(dd, OGF_HOST_CTL, OCF_CHANGE_LOCAL_NAME,
					CHANGE_LOCAL_NAME_CP_SIZE, &cp);
	}

	/* Set device class */
	if ((main_opts.flags & (1 << HCID_SET_CLASS))) {
		write_class_of_dev_cp cp;
		uint32_t class;
		uint8_t cls[3];

		if (read_local_class(&di.bdaddr, cls) < 0) {
			class = htobl(main_opts.class);
			cls[2] = get_service_classes(&di.bdaddr);
			memcpy(cp.dev_class, &class, 3);
		} else {
			if (!(main_opts.scan & SCAN_INQUIRY))
				cls[1] &= 0xdf; /* Clear discoverable bit */
			cls[2] = get_service_classes(&di.bdaddr);
			memcpy(cp.dev_class, cls, 3);
		}

		hci_send_cmd(dd, OGF_HOST_CTL, OCF_WRITE_CLASS_OF_DEV,
					WRITE_CLASS_OF_DEV_CP_SIZE, &cp);
	}

	/* Set page timeout */
	if ((main_opts.flags & (1 << HCID_SET_PAGETO))) {
		write_page_timeout_cp cp;

		cp.timeout = htobs(main_opts.pageto);
		hci_send_cmd(dd, OGF_HOST_CTL, OCF_WRITE_PAGE_TIMEOUT,
					WRITE_PAGE_TIMEOUT_CP_SIZE, &cp);
	}

	/* Set default link policy */
	policy = htobs(main_opts.link_policy);
	hci_send_cmd(dd, OGF_LINK_POLICY,
				OCF_WRITE_DEFAULT_LINK_POLICY, 2, &policy);

	hci_close_dev(dd);
}

static void init_device(int dev_id)
{
	struct hci_dev_req dr;
	struct hci_dev_info di;
	pid_t pid;
	int dd;

	/* Do initialization in the separate process */
	pid = fork();
	switch (pid) {
		case 0:
			atexit(at_child_exit);
			break;
		case -1:
			error("Fork failed. Can't init device hci%d: %s (%d)",
					dev_id, strerror(errno), errno);
		default:
			debug("child %d forked", pid);
			return;
	}

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		error("Can't open device hci%d: %s (%d)",
					dev_id, strerror(errno), errno);
		exit(1);
	}

	memset(&dr, 0, sizeof(dr));
	dr.dev_id = dev_id;

	/* Set link mode */
	dr.dev_opt = main_opts.link_mode;
	if (ioctl(dd, HCISETLINKMODE, (unsigned long) &dr) < 0) {
		error("Can't set link mode on hci%d: %s (%d)",
					dev_id, strerror(errno), errno);
	}

	/* Set link policy */
	dr.dev_opt = main_opts.link_policy;
	if (ioctl(dd, HCISETLINKPOL, (unsigned long) &dr) < 0 &&
							errno != ENETDOWN) {
		error("Can't set link policy on hci%d: %s (%d)",
					dev_id, strerror(errno), errno);
	}

	/* Start HCI device */
	if (ioctl(dd, HCIDEVUP, dev_id) < 0 && errno != EALREADY) {
		error("Can't init device hci%d: %s (%d)",
					dev_id, strerror(errno), errno);
		goto fail;
	}

	if (hci_devinfo(dev_id, &di) < 0)
		goto fail;

	if (hci_test_bit(HCI_RAW, &di.flags))
		goto done;

done:
	hci_close_dev(dd);
	exit(0);

fail:
	hci_close_dev(dd);
	exit(1);
}

static void device_devreg_setup(int dev_id)
{
	struct hci_dev_info di;
	gboolean devup;

	init_device(dev_id);

	memset(&di, 0, sizeof(di));

	if (hci_devinfo(dev_id, &di) < 0)
		return;

	devup = hci_test_bit(HCI_UP, &di.flags);

	if (!hci_test_bit(HCI_RAW, &di.flags))
		manager_register_adapter(dev_id, devup);
}

static void device_devup_setup(int dev_id)
{
	configure_device(dev_id);

	start_security_manager(dev_id);

	/* Return value 1 means ioctl(DEVDOWN) was performed */
	if (manager_start_adapter(dev_id) == 1)
		stop_security_manager(dev_id);
}

static void device_event(int event, int dev_id)
{
	switch (event) {
	case HCI_DEV_REG:
		info("HCI dev %d registered", dev_id);
		device_devreg_setup(dev_id);
		break;

	case HCI_DEV_UNREG:
		info("HCI dev %d unregistered", dev_id);
		manager_unregister_adapter(dev_id);
		break;

	case HCI_DEV_UP:
		info("HCI dev %d up", dev_id);
		device_devup_setup(dev_id);
		break;

	case HCI_DEV_DOWN:
		info("HCI dev %d down", dev_id);
		manager_stop_adapter(dev_id);
		stop_security_manager(dev_id);
		break;
	}
}

static int init_all_devices(int ctl)
{
	struct hci_dev_list_req *dl;
	struct hci_dev_req *dr;
	int i;

	dl = g_try_malloc0(HCI_MAX_DEV * sizeof(struct hci_dev_req) + sizeof(uint16_t));
	if (!dl) {
		info("Can't allocate devlist buffer: %s (%d)",
							strerror(errno), errno);
		return errno;
	}

	dl->dev_num = HCI_MAX_DEV;
	dr = dl->dev_req;

	if (ioctl(ctl, HCIGETDEVLIST, (void *) dl) < 0) {
		info("Can't get device list: %s (%d)",
							strerror(errno), errno);
		return errno;
	}

	for (i = 0; i < dl->dev_num; i++, dr++) {
		gboolean devup;

		device_event(HCI_DEV_REG, dr->dev_id);

		devup = hci_test_bit(HCI_UP, &dr->dev_opt);
		if (devup)
			device_event(HCI_DEV_UP, dr->dev_id);
	}

	g_free(dl);
	return 0;
}

static gboolean io_stack_event(GIOChannel *chan, GIOCondition cond,
								gpointer data)
{
	unsigned char buf[HCI_MAX_FRAME_SIZE], *ptr;
	evt_stack_internal *si;
	evt_si_device *sd;
	hci_event_hdr *eh;
	int type;
	size_t len;
	GIOError err;

	ptr = buf;

	err = g_io_channel_read(chan, (gchar *) buf, sizeof(buf), &len);
	if (err) {
		if (err == G_IO_ERROR_AGAIN)
			return TRUE;

		error("Read from control socket failed: %s (%d)",
							strerror(errno), errno);
		return FALSE;
	}

	type = *ptr++;

	if (type != HCI_EVENT_PKT)
		return TRUE;

	eh = (hci_event_hdr *) ptr;
	if (eh->evt != EVT_STACK_INTERNAL)
		return TRUE;

	ptr += HCI_EVENT_HDR_SIZE;

	si = (evt_stack_internal *) ptr;
	switch (si->type) {
	case EVT_SI_DEVICE:
		sd = (void *) &si->data;
		device_event(sd->event, sd->dev_id);
		break;
	}

	return TRUE;
}

static int hciops_setup(void)
{
	struct sockaddr_hci addr;
	struct hci_filter flt;
	GIOChannel *ctl_io, *child_io;
	int sock;

	if (pipe(child_pipe) < 0) {
		error("pipe(): %s (%d)", strerror(errno), errno);
		return errno;
	}

	child_io = g_io_channel_unix_new(child_pipe[0]);
	g_io_channel_set_close_on_unref(child_io, TRUE);
	g_io_add_watch(child_io,
			G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
			child_exit, NULL);
	g_io_channel_unref(child_io);

	/* Create and bind HCI socket */
	sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (sock < 0) {
		error("Can't open HCI socket: %s (%d)", strerror(errno),
								errno);
		return errno;
	}

	/* Set filter */
	hci_filter_clear(&flt);
	hci_filter_set_ptype(HCI_EVENT_PKT, &flt);
	hci_filter_set_event(EVT_STACK_INTERNAL, &flt);
	if (setsockopt(sock, SOL_HCI, HCI_FILTER, &flt,
							sizeof(flt)) < 0) {
		error("Can't set filter: %s (%d)", strerror(errno), errno);
		return errno;
	}

	memset(&addr, 0, sizeof(addr));
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = HCI_DEV_NONE;
	if (bind(sock, (struct sockaddr *) &addr,
							sizeof(addr)) < 0) {
		error("Can't bind HCI socket: %s (%d)",
							strerror(errno), errno);
		return errno;
	}

	ctl_io = g_io_channel_unix_new(sock);
	g_io_channel_set_close_on_unref(ctl_io, TRUE);

	g_io_add_watch(ctl_io, G_IO_IN, io_stack_event, NULL);

	g_io_channel_unref(ctl_io);

	/* Initialize already connected devices */
	return init_all_devices(sock);
}

static void hciops_cleanup(void)
{
}

static struct btd_adapter_ops hci_ops = {
	.setup = hciops_setup,
	.cleanup = hciops_cleanup,
};

static int hciops_init(void)
{
	return btd_register_adapter_ops(&hci_ops);
}
static void hciops_exit(void)
{
	btd_adapter_cleanup_ops(&hci_ops);
}

BLUETOOTH_PLUGIN_DEFINE(hciops, VERSION,
			BLUETOOTH_PLUGIN_PRIORITY_DEFAULT, hciops_init, hciops_exit)
