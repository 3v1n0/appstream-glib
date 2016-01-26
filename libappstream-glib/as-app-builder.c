/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * SECTION:as-app-builder
 * @short_description: Scan the filesystem for installed languages
 * @include: appstream-glib.h
 * @stability: Stable
 *
 * This object will parse a gettext catalog directory and calculate the
 * language stats for an application.
 *
 * See also: #AsApp
 */

#include "config.h"

#include <fnmatch.h>
#include <string.h>

#include "as-app-builder.h"

typedef struct {
	gchar		*locale;
	guint		 nstrings;
	guint		 percentage;
} AsAppBuilderEntry;

typedef struct {
	guint		 max_nstrings;
	GList		*data;
	GPtrArray	*translations;		/* no ref */
} AsAppBuilderContext;

/**
 * as_app_builder_entry_new:
 **/
static AsAppBuilderEntry *
as_app_builder_entry_new (void)
{
	AsAppBuilderEntry *entry;
	entry = g_slice_new0 (AsAppBuilderEntry);
	return entry;
}

/**
 * as_app_builder_entry_free:
 **/
static void
as_app_builder_entry_free (AsAppBuilderEntry *entry)
{
	g_free (entry->locale);
	g_slice_free (AsAppBuilderEntry, entry);
}

/**
 * as_app_builder_ctx_new:
 **/
static AsAppBuilderContext *
as_app_builder_ctx_new (void)
{
	AsAppBuilderContext *ctx;
	ctx = g_new0 (AsAppBuilderContext, 1);
	return ctx;
}

/**
 * as_app_builder_ctx_free:
 **/
static void
as_app_builder_ctx_free (AsAppBuilderContext *ctx)
{
	g_list_free_full (ctx->data, (GDestroyNotify) as_app_builder_entry_free);
	g_free (ctx);
}

typedef struct {
	guint32		 magic;
	guint32		 revision;
	guint32		 nstrings;
	guint32		 orig_tab_offset;
	guint32		 trans_tab_offset;
	guint32		 hash_tab_size;
	guint32		 hash_tab_offset;
	guint32		 n_sysdep_segments;
	guint32		 sysdep_segments_offset;
	guint32		 n_sysdep_strings;
	guint32		 orig_sysdep_tab_offset;
	guint32		 trans_sysdep_tab_offset;
} AsAppBuilderGettextHeader;

/**
 * as_app_builder_parse_file_gettext:
 **/
static gboolean
as_app_builder_parse_file_gettext (AsAppBuilderContext *ctx,
				   const gchar *locale,
				   const gchar *filename,
				   GError **error)
{
	AsAppBuilderEntry *entry;
	AsAppBuilderGettextHeader *h;
	g_autofree gchar *data = NULL;
	gboolean swapped;

	/* read data, although we only strictly need the header */
	if (!g_file_get_contents (filename, &data, NULL, error))
		return FALSE;

	h = (AsAppBuilderGettextHeader *) data;
	if (h->magic == 0x950412de)
		swapped = FALSE;
	else if (h->magic == 0xde120495)
		swapped = TRUE;
	else {
		g_set_error_literal (error,
				     AS_APP_ERROR,
				     AS_APP_ERROR_FAILED,
				     "file is invalid");
		return FALSE;
	}
	entry = as_app_builder_entry_new ();
	entry->locale = g_strdup (locale);
	if (swapped)
		entry->nstrings = GUINT32_SWAP_LE_BE (h->nstrings);
	else
		entry->nstrings = h->nstrings;
	if (entry->nstrings > ctx->max_nstrings)
		ctx->max_nstrings = entry->nstrings;
	ctx->data = g_list_prepend (ctx->data, entry);
	return TRUE;
}

/**
 * as_app_builder_search_locale_gettext:
 **/
static gboolean
as_app_builder_search_locale_gettext (AsAppBuilderContext *ctx,
				      const gchar *locale,
				      const gchar *messages_path,
				      AsAppBuilderFlags flags,
				      GError **error)
{
	const gchar *filename;
	gboolean found_anything = FALSE;
	guint i;
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GPtrArray) mo_paths = NULL;

	/* list files */
	dir = g_dir_open (messages_path, 0, error);
	if (dir == NULL)
		return FALSE;

	/* do a first pass at this, trying to find the prefered .mo */
	mo_paths = g_ptr_array_new_with_free_func (g_free);
	while ((filename = g_dir_read_name (dir)) != NULL) {
		g_autofree gchar *path = NULL;
		path = g_build_filename (messages_path, filename, NULL);
		if (!g_file_test (path, G_FILE_TEST_EXISTS))
			continue;
		for (i = 0; i < ctx->translations->len; i++) {
			AsTranslation *t = g_ptr_array_index (ctx->translations, i);
			g_autofree gchar *fn = NULL;
			if (as_translation_get_kind (t) != AS_TRANSLATION_KIND_GETTEXT &&
			    as_translation_get_kind (t) != AS_TRANSLATION_KIND_UNKNOWN)
				continue;
			fn = g_strdup_printf ("%s.mo", as_translation_get_id (t));
			if (g_strcmp0 (filename, fn) == 0) {
				if (!as_app_builder_parse_file_gettext (ctx,
									locale,
									path,
									error))
					return FALSE;
				found_anything = TRUE;
			}
		}
		g_ptr_array_add (mo_paths, g_strdup (path));
	}

	/* we got data from one or more of the translations */
	if (found_anything == TRUE)
		return TRUE;

	/* fall back to parsing *everything*, which might give us more
	 * language results than is actually true */
	if (flags & AS_APP_BUILDER_FLAG_USE_FALLBACKS) {
		for (i = 0; i < mo_paths->len; i++) {
			filename = g_ptr_array_index (mo_paths, i);
			if (!as_app_builder_parse_file_gettext (ctx,
								locale,
								filename,
								error))
				return FALSE;
		}
	}

	return TRUE;
}

typedef enum {
	AS_APP_TRANSLATION_QM_TAG_END		= 1,
	/* SourceText16 */
	AS_APP_TRANSLATION_QM_TAG_TRANSLATION	= 3,
	/* Context16 */
	AS_APP_TRANSLATION_QM_TAG_OBSOLETE1	= 5,
	AS_APP_TRANSLATION_QM_TAG_SOURCE_TEXT	= 6,
	AS_APP_TRANSLATION_QM_TAG_CONTEXT	= 7,
	AS_APP_TRANSLATION_QM_TAG_COMMENT	= 8,
	/* Obsolete2 */
	AS_APP_TRANSLATION_QM_TAG_LAST
} AsAppBuilderQmTag;

static guint8
_read_uint8 (const guint8 *data, guint32 *offset)
{
	guint8 tmp;
	tmp = data[*offset];
	(*offset) += 1;
	return tmp;
}

static guint32
_read_uint32 (const guint8 *data, guint32 *offset)
{
	guint32 tmp = 0;
	memcpy (&tmp, data + *offset, 4);
	(*offset) += 4;
	return GUINT32_FROM_BE (tmp);
}

/**
 * as_app_builder_parse_file_qt:
 **/
static gboolean
as_app_builder_parse_file_qt (AsAppBuilderContext *ctx,
			      const gchar *locale,
			      const gchar *filename,
			      GError **error)
{
	AsAppBuilderEntry *entry;
	guint32 addr = 0;
	guint32 len;
	guint32 m = 0;
	guint nstrings = 0;
	g_autofree guint8 *data = NULL;
	const guint8 qm_magic[] = {
		0x3c, 0xb8, 0x64, 0x18, 0xca, 0xef, 0x9c, 0x95,
		0xcd, 0x21, 0x1c, 0xbf, 0x60, 0xa1, 0xbd, 0xdd
	};

	/* load file */
	if (!g_file_get_contents (filename, (gchar **) &data, (gsize *) &len, error))
		return FALSE;

	/* check header */
	if (len < sizeof(qm_magic) ||
	    memcmp (data, qm_magic, sizeof(qm_magic)) != 0) {
		g_set_error_literal (error,
				     AS_APP_ERROR,
				     AS_APP_ERROR_FAILED,
				     "file is invalid");
		return FALSE;
	}
	m += sizeof(qm_magic);

	/* unknown value 0x42? */
	_read_uint8 (data, &m);

	/* find offset to data table */
	addr = _read_uint32 (data, &m);
	m += addr;

	/* unknown! */
	_read_uint8 (data, &m);
	_read_uint32 (data, &m);
	//g_debug ("seeking to QM @ %x\n", m);

	/* read data */
	while (m < len) {
		guint8 tag;
		guint32 tag_len;
		//g_debug ("QM @%x", m);
		tag = _read_uint8(data, &m);
		switch (tag) {
		case AS_APP_TRANSLATION_QM_TAG_END:
			//g_debug ("QM{END}");
			break;
		case AS_APP_TRANSLATION_QM_TAG_OBSOLETE1:
			m += 4;
			break;
		case AS_APP_TRANSLATION_QM_TAG_TRANSLATION:
			tag_len = _read_uint32 (data, &m);
			if (tag_len < 0xffffffff)
				m += tag_len;
			//g_debug ("QM{TRANSLATION} len %i", tag_len);
			nstrings++;
			break;
		case AS_APP_TRANSLATION_QM_TAG_SOURCE_TEXT:
			tag_len = _read_uint32 (data, &m);
			m += tag_len;
			//g_debug ("QM{SOURCE_TEXT} len %i", tag_len);
			break;
		case AS_APP_TRANSLATION_QM_TAG_CONTEXT:
			tag_len = _read_uint32 (data, &m);
			m += tag_len;
			//g_debug ("QM{CONTEXT} len %i", tag_len);
			break;
		case AS_APP_TRANSLATION_QM_TAG_COMMENT:
			tag_len = _read_uint32 (data, &m);
			m += tag_len;
			//g_debug ("QM{COMMENT} len %i", tag_len);
			break;
		default:
			//g_debug ("QM{unknown} tag kind %i", tag);
			m = G_MAXUINT32;
			break;
		}
	}

//	g_debug ("for QT locale %s, nstrings=%i", locale, nstrings);

	/* add new entry */
	entry = as_app_builder_entry_new ();
	entry->locale = g_strdup (locale);
	entry->nstrings = nstrings;
	if (entry->nstrings > ctx->max_nstrings)
		ctx->max_nstrings = entry->nstrings;
	ctx->data = g_list_prepend (ctx->data, entry);
	return TRUE;
}

/**
 * as_app_builder_search_translations_qt:
 **/
static gboolean
as_app_builder_search_translations_qt (AsAppBuilderContext *ctx,
				       const gchar *prefix,
				       AsAppBuilderFlags flags,
				       GError **error)
{
	guint i;

	/* search for each translation ID */
	for (i = 0; i < ctx->translations->len; i++) {
		AsTranslation *t;
		const gchar *filename;
		const gchar *install_dir;
		g_autofree gchar *path = NULL;
		g_autoptr(GDir) dir = NULL;

		/* FIXME: this path probably has to be specified as an attribute
		 * in the <translations> tag from the AppData file */
		t = g_ptr_array_index (ctx->translations, i);
		install_dir = as_translation_get_id (t);
		path = g_build_filename (prefix,
					 "share",
					 install_dir,
					 "translations",
					 NULL);
		if (!g_file_test (path, G_FILE_TEST_EXISTS))
			return TRUE;
		dir = g_dir_open (path, 0, error);
		if (dir == NULL)
			return FALSE;

		/* the format is ${prefix}/share/${install_dir}/translations/${id}_${locale}.qm */
		while ((filename = g_dir_read_name (dir)) != NULL) {
			g_autofree gchar *fn = NULL;
			g_autofree gchar *locale = NULL;
			if (as_translation_get_kind (t) != AS_TRANSLATION_KIND_QT &&
			    as_translation_get_kind (t) != AS_TRANSLATION_KIND_UNKNOWN)
				continue;
			if (!g_str_has_prefix (filename, as_translation_get_id (t)))
				continue;
			locale = g_strdup (filename + strlen (as_translation_get_id (t)) + 1);
			g_strdelimit (locale, ".", '\0');
			fn = g_build_filename (path, filename, NULL);
			if (!as_app_builder_parse_file_qt (ctx, locale, fn, error))
				return FALSE;
		}
	}

	return TRUE;
}

/**
 * as_app_builder_search_translations_gettext:
 **/
static gboolean
as_app_builder_search_translations_gettext (AsAppBuilderContext *ctx,
					    const gchar *prefix,
					    AsAppBuilderFlags flags,
					    GError **error)
{
	const gchar *locale;
	g_autofree gchar *path = NULL;
	g_autoptr(GDir) dir = NULL;

	path = g_build_filename (prefix, "share", "locale", NULL);
	if (!g_file_test (path, G_FILE_TEST_EXISTS))
		return TRUE;
	dir = g_dir_open (path, 0, error);
	if (dir == NULL)
		return FALSE;
	while ((locale = g_dir_read_name (dir)) != NULL) {
		g_autofree gchar *fn = NULL;
		fn = g_build_filename (path, locale, "LC_MESSAGES", NULL);
		if (!g_file_test (fn, G_FILE_TEST_EXISTS))
			continue;
		if (!as_app_builder_search_locale_gettext (ctx, locale, fn, flags, error))
			return FALSE;
	}
	return TRUE;
}

/**
 * as_app_builder_entry_sort_cb:
 **/
static gint
as_app_builder_entry_sort_cb (gconstpointer a, gconstpointer b)
{
	return g_strcmp0 (((AsAppBuilderEntry *) a)->locale,
			  ((AsAppBuilderEntry *) b)->locale);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AsAppBuilderContext, as_app_builder_ctx_free)

/**
 * as_app_builder_search_translations:
 * @app: an #AsApp
 * @prefix: a prefix to search, e.g. "/usr"
 * @min_percentage: minimum percentage to add language
 * @flags: #AsAppBuilderFlags, e.g. %AS_APP_BUILDER_FLAG_USE_FALLBACKS
 * @cancellable: a #GCancellable or %NULL
 * @error: a #GError or %NULL
 *
 * Searches a prefix for languages, and using a heuristic adds <language>
 * tags to the specified application.
 *
 * If there are no #AsTranslation objects set on the #AsApp then all domains
 * are matched, which may include more languages than you intended to.
 *
 * @min_percentage sets the minimum percentage to add a language tag.
 * The usual value would be 25% and any language less complete than
 * this will not be added.
 *
 * The purpose of this functionality is to avoid blowing up the size
 * of the AppStream metadata with a lot of extra data detailing
 * languages with very few translated strings.
 *
 * Returns: %TRUE for success
 *
 * Since: 0.5.8
 **/
gboolean
as_app_builder_search_translations (AsApp *app,
				    const gchar *prefix,
				    guint min_percentage,
				    AsAppBuilderFlags flags,
				    GCancellable *cancellable,
				    GError **error)
{
	AsAppBuilderEntry *e;
	GList *l;
	g_autoptr(AsAppBuilderContext) ctx = NULL;

	ctx = as_app_builder_ctx_new ();
	ctx->translations = as_app_get_translations (app);

	/* search for QT .qm files */
	if (!as_app_builder_search_translations_qt (ctx, prefix, flags, error))
		return FALSE;

	/* search for gettext .mo files */
	if (!as_app_builder_search_translations_gettext (ctx, prefix, flags, error))
		return FALSE;

	/* calculate percentages */
	for (l = ctx->data; l != NULL; l = l->next) {
		e = l->data;
		e->percentage = MIN (e->nstrings * 100 / ctx->max_nstrings, 100);
	}

	/* sort */
	ctx->data = g_list_sort (ctx->data, as_app_builder_entry_sort_cb);

	/* add results */
	for (l = ctx->data; l != NULL; l = l->next) {
		e = l->data;
		if (e->percentage < min_percentage)
			continue;
		as_app_add_language (app, e->percentage, e->locale);
	}
	return TRUE;
}