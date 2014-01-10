/* chowncgroup
 *
 * Copyright © 2013 Serge Hallyn
 * Author: Serge Hallyn <serge.hallyn@ubuntu.com>
 *
 * This is only for testing purposes - we really want to update dbus-send
 * to accept a 'pid' arg type which sends the pid as a scm_credential.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <libgen.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/io.h>
#include <nih/option.h>
#include <nih/main.h>
#include <nih/logging.h>
#include <nih/error.h>

#include <nih-dbus/dbus_connection.h>
#include <nih-dbus/dbus_proxy.h>
#include <nih-dbus/dbus_error.h>

#include <sys/socket.h>

#include "cgmanager.h"
#define PACKAGE_NAME "cgmanager"
#define PACKAGE_VERSION "0.0"
#define PACKAGE_BUGREPORT ""

static const char *controller;
static const char *cgroup;
static int uid = -1, gid = -1;

typedef int (*NihOptionSetter) (NihOption *option, const char *arg);

void send_dummy_msg(DBusConnection *conn)
{
	DBusMessage *message = NULL;
	DBusMessageIter iter;
	int a;
	message = dbus_message_new_method_call(dbus_bus_get_unique_name(conn),
			"/org/linuxcontainers/cgmanager",
			"org.linuxcontainers.cgmanager0_0", "ping");
	dbus_message_set_no_reply(message, TRUE);
	dbus_message_iter_init_append(message, &iter);
        if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &a)) {
                nih_error_raise_no_memory ();
                return;
        }
	dbus_connection_send(conn, message, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(message);
}

int set_gid(NihOption *option, const char *arg)
{
	unsigned long int p;
	errno = 0;
	p = strtoul(arg, NULL, 10);
	nih_assert(errno == 0);
	nih_assert(p < INT_MAX);
	gid = (int)p;
	return 0;
}

int set_uid(NihOption *option, const char *arg)
{
	unsigned long int p;
	errno = 0;
	p = strtoul(arg, NULL, 10);
	nih_assert(errno == 0);
	nih_assert(p < INT_MAX);
	uid = (int)p;
	return 0;
}

int set_cgroup(NihOption *option, const char *arg)
{
	cgroup = arg;
	return 0;
}

int set_controller(NihOption *option, const char *arg)
{
	controller = arg;
	return 0;
}

/**
 * options:
 *
 * Command-line options accepted by this program.
 **/
static NihOption options[] = {
	{ 'c', "controller", N_("Controller for which to act"),
	  NULL, "CONTROLLER", NULL, set_controller },
	{ 'n', "name", N_("Cgroup to chown"),
	  NULL, "NAME", NULL, set_cgroup },
	{ 'u', "uid", N_("Uid to chown cgroup to"),
	  NULL, "UID", NULL, set_uid },
	{ 'g', "gid", N_("Gid to chgrp cgroup to"),
	  NULL, "UID", NULL, set_gid },

	NIH_OPTION_LAST
};

int send_creds(int sock, int rpid, int ruid, int rgid)
{
	struct msghdr msg = { 0 };
	struct iovec iov;
	struct cmsghdr *cmsg;
	struct ucred cred = {
		.pid = rpid,
		.uid = ruid,
		.gid = rgid,
	};
	char cmsgbuf[CMSG_SPACE(sizeof(cred))];
	char buf[1];
	buf[0] = 'p';

	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_CREDENTIALS;
	memcpy(CMSG_DATA(cmsg), &cred, sizeof(cred));

	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (sendmsg(sock, &msg, 0) < 0) {
		perror("sendmsg");
		return -1;
	}
	return 0;
}

static void usage(char *me) {
	fprintf(stderr, "Usage: %s -c controller -n cgroup-name [ -u uid | -g gid ]\n", me);
	exit(1);
}

int
main (int   argc,
      char *argv[])
{
	char **             args;
	DBusConnection *    conn;
	int optval = 1, exitval = 1;
	DBusMessage *message = NULL, *reply = NULL;
	DBusMessageIter iter;
	dbus_uint32_t serial;;
	int sv[2] = {-1, -1};
	char buf[1];

	nih_main_init (argv[0]);
	if (geteuid() != 0) {
		fprintf(stderr, "Must be called by root");
		exit(1);
	}

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
		nih_error("Error creating socketpair: %s", strerror(errno));
		exit(1);
	}
	if (setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		perror("setsockopt");
		exit(1);
	}
	if (setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		perror("setsockopt");
		exit(1);
	}

	nih_option_set_synopsis (_("Control group client"));

	args = nih_option_parser (NULL, argc, argv, options, FALSE);
	if (! args)
		exit (1);

	if (!controller || !cgroup)
		usage(argv[0]);
	if (uid == -1 || gid == -1)
		usage(argv[0]);

	conn = nih_dbus_connect(CGMANAGER_DBUS_PATH, NULL);
	nih_assert (conn != NULL);

	send_dummy_msg(conn);

	message = dbus_message_new_method_call(dbus_bus_get_unique_name(conn),
			"/org/linuxcontainers/cgmanager",
			"org.linuxcontainers.cgmanager0_0", "chownCgroupScm");

	dbus_message_iter_init_append(message, &iter);
        if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING,
                                              &controller)) {
                nih_error_raise_no_memory ();
                return -1;
        }
        if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING,
                                              &cgroup)) {
                nih_error_raise_no_memory ();
                return -1;
        }

	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD,
					      &sv[1])) {
		nih_error_raise_no_memory ();
		return -1;
	}

	if (!dbus_connection_send(conn, message, &serial)) {
		nih_error("failed to send dbus message");
		return -1;
	}
	dbus_connection_flush(conn);
	if (message) {
		dbus_message_unref(message);
		message = NULL;
	}

	if (read(sv[0], &buf, 1) != 1) {
		nih_error("Error getting reply from server over socketpair");
		goto out;
	}
	if (send_creds(sv[0], getpid(), getuid(), getgid())) {
		nih_error("Error sending pid over SCM_CREDENTIAL");
		goto out;
	}
	if (read(sv[0], &buf, 1) != 1) {
		nih_error("Error getting reply from server over socketpair");
		goto out;
	}
	if (send_creds(sv[0], getpid(), uid, gid)) {
		nih_error("Error sending pid over SCM_CREDENTIAL");
		goto out;
	}
	int ret = read(sv[0], buf, 1);
	close(sv[0]);
	close(sv[1]);
	if (ret == 1 && *buf == '1')
		exitval = 0;

out:
	if (message)
		dbus_message_unref(message);
	if (reply)
		dbus_message_unref(reply);
	dbus_connection_unref (conn);

	exit(exitval);
}
