/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <gusb.h>
#include <string.h>
#include <stdlib.h>

#include "unifying-dongle.h"

typedef struct {
	GCancellable		*cancellable;
	GPtrArray		*cmd_array;
	UnifyingDongleKind	 emulation_kind;
} UnifyingToolPrivate;

static void
unifying_tool_private_free (UnifyingToolPrivate *priv)
{
	if (priv == NULL)
		return;
	g_object_unref (priv->cancellable);
	if (priv->cmd_array != NULL)
		g_ptr_array_unref (priv->cmd_array);
	g_free (priv);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(UnifyingToolPrivate, unifying_tool_private_free)

typedef gboolean (*UnifyingToolPrivateCb)	(UnifyingToolPrivate	*util,
					 gchar			**values,
					 GError			**error);

typedef struct {
	gchar		*name;
	gchar		*arguments;
	gchar		*description;
	UnifyingToolPrivateCb	 callback;
} UnifyingToolItem;

static void
unifying_tool_item_free (UnifyingToolItem *item)
{
	g_free (item->name);
	g_free (item->arguments);
	g_free (item->description);
	g_free (item);
}

static gint
unifying_tool_sort_command_name_cb (UnifyingToolItem **item1, UnifyingToolItem **item2)
{
	return g_strcmp0 ((*item1)->name, (*item2)->name);
}

static void
unifying_tool_add (GPtrArray *array,
		      const gchar *name,
		      const gchar *arguments,
		      const gchar *description,
		      UnifyingToolPrivateCb callback)
{
	guint i;
	UnifyingToolItem *item;
	g_auto(GStrv) names = NULL;

	g_return_if_fail (name != NULL);
	g_return_if_fail (description != NULL);
	g_return_if_fail (callback != NULL);

	/* add each one */
	names = g_strsplit (name, ",", -1);
	for (i = 0; names[i] != NULL; i++) {
		item = g_new0 (UnifyingToolItem, 1);
		item->name = g_strdup (names[i]);
		if (i == 0) {
			item->description = g_strdup (description);
		} else {
			item->description = g_strdup_printf ("Alias to %s", names[0]);
		}
		item->arguments = g_strdup (arguments);
		item->callback = callback;
		g_ptr_array_add (array, item);
	}
}

static gchar *
unifying_tool_get_descriptions (GPtrArray *array)
{
	guint i;
	gsize j;
	gsize len;
	const gsize max_len = 31;
	UnifyingToolItem *item;
	GString *string;

	/* print each command */
	string = g_string_new ("");
	for (i = 0; i < array->len; i++) {
		item = g_ptr_array_index (array, i);
		g_string_append (string, "  ");
		g_string_append (string, item->name);
		len = strlen (item->name) + 2;
		if (item->arguments != NULL) {
			g_string_append (string, " ");
			g_string_append (string, item->arguments);
			len += strlen (item->arguments) + 1;
		}
		if (len < max_len) {
			for (j = len; j < max_len + 1; j++)
				g_string_append_c (string, ' ');
			g_string_append (string, item->description);
			g_string_append_c (string, '\n');
		} else {
			g_string_append_c (string, '\n');
			for (j = 0; j < max_len + 1; j++)
				g_string_append_c (string, ' ');
			g_string_append (string, item->description);
			g_string_append_c (string, '\n');
		}
	}

	/* remove trailing newline */
	if (string->len > 0)
		g_string_set_size (string, string->len - 1);

	return g_string_free (string, FALSE);
}

static gboolean
unifying_tool_run (UnifyingToolPrivate *priv,
		   const gchar *command,
		   gchar **values,
		   GError **error)
{
	guint i;
	UnifyingToolItem *item;

	/* find command */
	for (i = 0; i < priv->cmd_array->len; i++) {
		item = g_ptr_array_index (priv->cmd_array, i);
		if (g_strcmp0 (item->name, command) == 0)
			return item->callback (priv, values, error);
	}

	/* not found */
	g_set_error_literal (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_FOUND,
			     "Command not found");
	return FALSE;
}

static UnifyingDongle *
fu_unifying_get_default_dongle (UnifyingToolPrivate *priv, GError **error)
{
	UnifyingDongle *dongle = NULL;
	g_autoptr(GUsbContext) usb_ctx = NULL;
	g_autoptr(GPtrArray) devices = NULL;

	/* get the device */
	usb_ctx = g_usb_context_new (error);
	if (usb_ctx == NULL) {
		g_prefix_error (error, "Failed to open USB devices: ");
		return NULL;
	}
	g_usb_context_enumerate (usb_ctx);
	devices = g_usb_context_get_devices (usb_ctx);
	for (guint i = 0; i < devices->len; i++) {
		GUsbDevice *usb_dev_tmp = g_ptr_array_index (devices, i);
		g_autoptr(UnifyingDongle) dev_tmp = unifying_dongle_new (usb_dev_tmp);
		if (dev_tmp == NULL)
			continue;
		if (unifying_dongle_get_kind (dev_tmp) != UNIFYING_DONGLE_KIND_UNKNOWN) {
			dongle = g_object_ref (dev_tmp);
			break;
		}
	}

	/* nothing supported */
	if (dongle == NULL) {
		g_set_error_literal (error,
				     G_IO_ERROR,
				     G_IO_ERROR_FAILED,
				     "No supported device plugged in");
		return NULL;
	}
	return dongle;
}

static gboolean
unifying_tool_info (UnifyingToolPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(UnifyingDongle) dongle = NULL;

	/* open device */
	dongle = fu_unifying_get_default_dongle (priv, error);
	if (dongle == NULL)
		return FALSE;
	if (!unifying_dongle_open (dongle, error))
		return FALSE;

	/* show on console */
	g_debug ("Found %s", unifying_dongle_kind_to_string (unifying_dongle_get_kind (dongle)));
	g_print ("Firmware Ver: %s\n", unifying_dongle_get_version_fw (dongle));
	g_print ("Bootloader Ver: %s\n", unifying_dongle_get_version_bl (dongle));
	g_print ("GUID: %s\n", unifying_dongle_get_guid (dongle));

	/* close device */
	return unifying_dongle_close (dongle, error);
}

static void
fu_unifying_write_progress_cb (goffset current, goffset total, gpointer user_data)
{
	UnifyingToolPrivate *priv = (UnifyingToolPrivate *) user_data;
	gdouble percentage = -1.f;
	if (priv->emulation_kind != UNIFYING_DONGLE_KIND_UNKNOWN)
		return;
	if (total > 0)
		percentage = (100.f * (gdouble) current) / (gdouble) total;
	g_print ("Written %" G_GOFFSET_FORMAT "/%" G_GOFFSET_FORMAT " bytes [%.1f%%]\n",
		 current, total, percentage);
}

static gboolean
unifying_tool_write (UnifyingToolPrivate *priv, gchar **values, GError **error)
{
	gsize len;
	g_autofree guint8 *data = NULL;
	g_autoptr(GBytes) fw = NULL;
	g_autoptr(UnifyingDongle) dongle = NULL;

	/* check args */
	if (g_strv_length (values) != 1) {
		g_set_error_literal (error,
				     G_IO_ERROR,
				     G_IO_ERROR_FAILED,
				     "Invalid arguments, expected FILENAME"
				     " -- e.g. `firmware.hex`");
		return FALSE;
	}

	/* open device */
	if (priv->emulation_kind == UNIFYING_DONGLE_KIND_UNKNOWN) {
		dongle = fu_unifying_get_default_dongle (priv, error);
		if (dongle == NULL)
			return FALSE;
	} else {
		dongle = unifying_dongle_emulated_new (priv->emulation_kind);
	}
	if (!unifying_dongle_open (dongle, error))
		return FALSE;

	/* do we need to go into bootloader mode */
	if (unifying_dongle_get_kind (dongle) == UNIFYING_DONGLE_KIND_RUNTIME) {
		if (!unifying_dongle_detach (dongle, error))
			return FALSE;
		g_print ("Switched to bootloader, now run again\n");
		return TRUE;
	}

	/* load firmware file */
	if (!g_file_get_contents (values[0], (gchar **) &data, &len, error)) {
		g_prefix_error (error, "Failed to load %s: ", values[0]);
		return FALSE;
	}

	/* update with data blob */
	fw = g_bytes_new (data, len);
	if (!unifying_dongle_write_firmware (dongle, fw,
						fu_unifying_write_progress_cb, priv,
						error))
		return FALSE;

	/* detach back into runtime */
	if (!unifying_dongle_attach (dongle, error))
		return FALSE;
	if (!unifying_dongle_close (dongle, error))
		return FALSE;

	return TRUE;
}

static gboolean
unifying_tool_attach (UnifyingToolPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(UnifyingDongle) dongle = NULL;
	dongle = fu_unifying_get_default_dongle (priv, error);
	if (dongle == NULL)
		return FALSE;
	if (!unifying_dongle_open (dongle, error))
		return FALSE;
	if (!unifying_dongle_attach (dongle, error))
		return FALSE;
	if (!unifying_dongle_close (dongle, error))
		return FALSE;
	return TRUE;
}

static gboolean
unifying_tool_detach (UnifyingToolPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(UnifyingDongle) dongle = NULL;
	dongle = fu_unifying_get_default_dongle (priv, error);
	if (dongle == NULL)
		return FALSE;
	if (!unifying_dongle_open (dongle, error))
		return FALSE;
	if (!unifying_dongle_detach (dongle, error))
		return FALSE;
	if (!unifying_dongle_close (dongle, error))
		return FALSE;
	return TRUE;
}

static void
unifying_tool_log_handler_cb (const gchar *log_domain,
			      GLogLevelFlags log_level,
			      const gchar *message,
			      gpointer user_data)
{
	g_print ("%s\t%s\n", log_domain, message);
}

int
main (int argc, char **argv)
{
	gboolean verbose = FALSE;
	g_autofree gchar *cmd_descriptions = NULL;
	g_autofree gchar *emulation_kind = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(UnifyingDongle) dongle = NULL;
	g_autoptr(UnifyingToolPrivate) priv = g_new0 (UnifyingToolPrivate, 1);
	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			"Print verbose debug statements", NULL },
		{ "emulate", 'e', 0, G_OPTION_ARG_STRING, &emulation_kind,
			"Emulate a device type", NULL },
		{ NULL}
	};

	/* FIXME: do stuff on ctrl+c */
	priv->cancellable = g_cancellable_new ();

	/* add commands */
	priv->cmd_array = g_ptr_array_new_with_free_func ((GDestroyNotify) unifying_tool_item_free);
	unifying_tool_add (priv->cmd_array,
			   "info", NULL,
			   "Show information about the device",
			   unifying_tool_info);
	unifying_tool_add (priv->cmd_array,
			   "write", "FILENAME",
			   "Update the firmware",
			   unifying_tool_write);
	unifying_tool_add (priv->cmd_array,
			   "attach", NULL,
			   "Attach to firmware mode",
			   unifying_tool_attach);
	unifying_tool_add (priv->cmd_array,
			   "detach", NULL,
			   "Detach to bootloader mode",
			   unifying_tool_detach);

	/* sort by command name */
	g_ptr_array_sort (priv->cmd_array,
			  (GCompareFunc) unifying_tool_sort_command_name_cb);


	/* get a list of the commands */
	context = g_option_context_new (NULL);
	cmd_descriptions = unifying_tool_get_descriptions (priv->cmd_array);
	g_option_context_set_summary (context, cmd_descriptions);
	g_set_application_name ("Logitech Unifying Debug Tool");
	g_option_context_add_main_entries (context, options, NULL);
	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_print ("%s: %s\n", "Failed to parse arguments", error->message);
		return EXIT_FAILURE;
	}

	/* emulate */
	priv->emulation_kind = unifying_dongle_kind_from_string (emulation_kind);
	if (priv->emulation_kind != UNIFYING_DONGLE_KIND_UNKNOWN)
		g_log_set_default_handler (unifying_tool_log_handler_cb, priv);

	/* set verbose? */
	if (verbose)
		g_setenv ("G_MESSAGES_DEBUG", "all", FALSE);

	/* run the specified command */
	if (!unifying_tool_run (priv, argv[1], (gchar**) &argv[2], &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			g_autofree gchar *tmp = NULL;
			tmp = g_option_context_get_help (context, TRUE, NULL);
			g_print ("%s\n\n%s", error->message, tmp);
		} else {
			g_print ("%s\n", error->message);
		}
		return EXIT_FAILURE;
	}

	return 0;
}
