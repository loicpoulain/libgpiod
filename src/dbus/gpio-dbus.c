// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * This file is part of libgpiod.
 *
 * Copyright (C) 2018 Bartosz Golaszewski <bartekgola@gmail.com>
 */

#include <gpiod.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gudev/gudev.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

typedef struct MainLoopCtx {
	GMainLoop *loop;
	GUdevClient *udev;
	GDBusConnection *bus;
} MainLoopCtx;

static G_GNUC_NORETURN void die(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	g_logv(NULL, G_LOG_LEVEL_CRITICAL, fmt, va);
	va_end(va);

	exit(EXIT_FAILURE);
}

static const gchar *log_level_to_priority(GLogLevelFlags lvl)
{
	if (lvl & G_LOG_LEVEL_ERROR)
		/*
		 * GLib's ERROR log level is always fatal so translate it
		 * to syslog's EMERG level.
		 */
		return "0";
	else if (lvl & G_LOG_LEVEL_CRITICAL)
		/*
		 * Use GLib's CRITICAL level for error messages. We don't
		 * necessarily want to abort() everytime an error occurred.
		 */
		return "3";
	else if (lvl & G_LOG_LEVEL_WARNING)
		return "4";
	else if (lvl & G_LOG_LEVEL_MESSAGE)
		return "5";
	else if (lvl & G_LOG_LEVEL_INFO)
		return "6";
	else if (lvl & G_LOG_LEVEL_DEBUG)
		return "7";

	/* Default to LOG_NOTICE. */
	return "5";
}

static void handle_log_debug(const gchar *domain, GLogLevelFlags lvl,
			     const gchar *msg, gpointer data G_GNUC_UNUSED)
{
	g_log_structured(domain, lvl, "MESSAGE", msg);
}

static GLogWriterOutput log_write(GLogLevelFlags lvl, const GLogField *fields,
				  gsize n_fields, gpointer data G_GNUC_UNUSED)
{
	const gchar *msg = NULL, *prio;
	const GLogField *field;
	gsize i;

	for (i = 0; i < n_fields; i++) {
		field = &fields[i];

		/* We're only interested in the MESSAGE field. */
		if (!g_strcmp0(field->key, "MESSAGE")) {
			msg = (const gchar *)field->value;
			break;
		}
	}
	if (!msg)
		return G_LOG_WRITER_UNHANDLED;

	prio = log_level_to_priority(lvl);

	g_printerr("<%s>%s\n", prio, msg);

	return G_LOG_WRITER_HANDLED;
}

static gboolean on_sigterm(gpointer data)
{
	MainLoopCtx *ctx = data;

	g_debug("SIGTERM received");

	g_main_loop_quit(ctx->loop);

	return G_SOURCE_REMOVE;
}

static gboolean on_sigint(gpointer data)
{
	MainLoopCtx *ctx = data;

	g_debug("SIGINT received");

	g_main_loop_quit(ctx->loop);

	return G_SOURCE_REMOVE;
}

static gboolean on_sighup(gpointer data G_GNUC_UNUSED)
{
	g_debug("SIGHUB received");

	return G_SOURCE_CONTINUE;
}

static const gchar* const udev_subsystems[] = { "gpio", NULL };

static void on_uevent(GUdevClient *udev, const gchar *action,
		      GUdevDevice *dev, gpointer data)
{
	MainLoopCtx *ctx = data;

	g_debug("uevent: %s action on %s device",
		action, g_udev_device_get_name(dev));
}

static void on_bus_method_call(GDBusConnection *conn,
			       const gchar *sender,
			       const gchar *obj_path,
			       const gchar *interface,
			       const gchar *method,
			       GVariant *params,
			       GDBusMethodInvocation *invc,
			       gpointer data)
{
	g_debug("DBus method call");
}

static void on_bus_acquired(GDBusConnection *conn,
			    const gchar *name G_GNUC_UNUSED, gpointer data)
{
	MainLoopCtx *ctx = data;

	g_debug("DBus connection acquired");

	ctx->bus = conn;
}

static void on_name_acquired(GDBusConnection *conn,
			     const gchar *name, gpointer data)
{
	MainLoopCtx *ctx = data;
	GList *devs;

	g_debug("DBus name acquired: '%s'", name);

	g_signal_connect(ctx->udev, "uevent", G_CALLBACK(on_uevent), ctx);
	devs = g_udev_client_query_by_subsystem(ctx->udev, "gpio");
	

	g_list_free(devs);
}

static void on_name_lost(GDBusConnection *conn,
			 const gchar *name, gpointer data G_GNUC_UNUSED)
{
	g_debug("DBus name lost: '%s'", name);

	if (!conn)
		die("unable to make connection to the bus");

	if (g_dbus_connection_is_closed(conn))
		die("connection to the bus closed, dying...");

	die("name '%s' lost on the bus, dying...", name);
}

static gboolean opt_debug;

static GOptionEntry opts[] = {
	{
		.long_name		= "debug",
		.short_name		= 'd',
		.flags			= 0,
		.arg			= G_OPTION_ARG_NONE,
		.arg_data		= &opt_debug,
		.description		= "print additional debug messages",
		.arg_description	= NULL,
	},
	{ }
};

static void parse_opts(int argc, char **argv)
{
	GError *error = NULL;
	GOptionContext *ctx;
	gchar *summary;
	gboolean rv;

	ctx = g_option_context_new(NULL);

	summary = g_strdup_printf("%s (libgpiod) v%s - dbus daemon for libgpiod",
				  g_get_prgname(), gpiod_version_string());
	g_option_context_set_summary(ctx, summary);
	g_free(summary);

	g_option_context_add_main_entries(ctx, opts, NULL);

	rv = g_option_context_parse(ctx, &argc, &argv, &error);
	if (!rv)
		die("option parsing failed: %s", error->message);

	g_option_context_free(ctx);
}

int main(int argc, char **argv)
{
	MainLoopCtx ctx;
	guint bus_id;

	memset(&ctx, 0, sizeof(ctx));
	g_set_prgname(program_invocation_short_name);
	g_log_set_writer_func(log_write, NULL, NULL);

	parse_opts(argc, argv);

	if (opt_debug)
		g_log_set_handler(NULL,
				  G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO,
				  handle_log_debug, NULL);

	g_message("initiating %s", g_get_prgname());

	ctx.loop = g_main_loop_new(NULL, FALSE);

	g_unix_signal_add(SIGTERM, on_sigterm, &ctx);
	g_unix_signal_add(SIGINT, on_sigint, &ctx);
	g_unix_signal_add(SIGHUP, on_sighup, NULL); /* Ignore SIGHUP. */

	bus_id = g_bus_own_name(G_BUS_TYPE_SYSTEM, "org.gpiod",
				G_BUS_NAME_OWNER_FLAGS_NONE,
				on_bus_acquired,
				on_name_acquired,
				on_name_lost,
				&ctx, NULL);

	ctx.udev = g_udev_client_new(udev_subsystems);

	g_message("%s started", g_get_prgname());

	g_main_loop_run(ctx.loop);

	g_object_unref(ctx.udev);
	g_bus_unown_name(bus_id);
	g_main_loop_unref(ctx.loop);

	g_message("%s exiting cleanly", g_get_prgname());

	return EXIT_SUCCESS;
}
