/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nautilus-icon-factory.c: Class for obtaining icons for files and other objects.
 
   Copyright (C) 1999, 2000 Red Hat Inc.
   Copyright (C) 1999, 2000, 2001 Eazel, Inc.
  
   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
  
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
  
   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
  
   Authors: John Sullivan <sullivan@eazel.com>,
            Darin Adler <darin@bentspoon.com>,
	    Andy Hertzfeld <andy@eazel.com>
*/

#include <config.h>
#include "nautilus-icon-factory.h"

#include "nautilus-default-file-icon.h"
#include "nautilus-directory-notify.h"
#include "nautilus-file-attributes.h"
#include "nautilus-file-private.h"
#include "nautilus-file-utilities.h"
#include "nautilus-global-preferences.h"
#include "nautilus-icon-factory-private.h"
#include "nautilus-lib-self-check-functions.h"
#include "nautilus-link.h"
#include "nautilus-thumbnails.h"
#include "nautilus-trash-monitor.h"
#include <eel/eel-debug.h>
#include <eel/eel-gdk-extensions.h>
#include <eel/eel-gdk-pixbuf-extensions.h>
#include <eel/eel-glib-extensions.h>
#include <eel/eel-gtk-macros.h>
#include <eel/eel-pango-extensions.h>
#include <eel/eel-string.h>
#include <eel/eel-vfs-extensions.h>
#include <gtk/gtksettings.h>
#include <gtk/gtksignal.h>
#include <gtk/gtkiconfactory.h>
#include <gtk/gtkicontheme.h>
#include <glib/gi18n.h>
#include <libgnome/gnome-util.h>
#include <libgnome/gnome-macros.h>
#include <libgnomeui/gnome-icon-lookup.h>
#include <libgnomevfs/gnome-vfs-file-info.h>
#include <libgnomevfs/gnome-vfs-mime-handlers.h>
#include <libgnomevfs/gnome-vfs-mime-monitor.h>
#include <libgnomevfs/gnome-vfs-ops.h>
#include <libgnomevfs/gnome-vfs-types.h>
#include <libgnomevfs/gnome-vfs-utils.h>
#include <librsvg/rsvg.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CACHE_SELF_CHECKS 0

#define ICON_NAME_THUMBNAIL_LOADING     "gnome-fs-loading-icon"
#define ICON_NAME_TRASH_EMPTY		"user-trash"
#define ICON_NAME_TRASH_FULL		"user-trash-full"
#define ICON_NAME_HOME                  "user-home"

#define NAUTILUS_EMBLEM_NAME_PREFIX "emblem-"

/* This used to be called ICON_CACHE_MAX_ENTRIES, but it's misleading
 * to call it that, since we can have any number of entries in the
 * cache if the caller keeps the pixbuf around (we only get rid of
 * items from the cache after the caller unref's them).
*/
#define ICON_CACHE_COUNT                20

/* This is the number of milliseconds we wait before sweeping out
 * items from the cache.
 */
#define ICON_CACHE_SWEEP_TIMEOUT        (10 * 1000)

/* After a pixmap goes out of the recently used queue, and the pixbuf are not
 * referenced outside the cache this is the number of sweeps an object lives.
 */
#define ICON_MAX_AGE        10

/* This circular doubly-linked list structure is used to keep a list
 * of the most recently used items in the cache.
 */
typedef struct CircularList CircularList;
struct CircularList {
	CircularList *next;
	CircularList *prev;
};

/* The key to a hash table that holds CacheIcons. */
typedef struct {
	char *name; /* Icon name or absolute filename */
	char *modifier;
	guint nominal_size;
	gboolean force_nominal;
} CacheKey;

/* The value in the same table. */
typedef struct {
	guint ref_count;
	
	GdkPixbuf *pixbuf;
	GdkRectangle *embedded_text_rect;

	GdkPoint *attach_points;
	int n_attach_points;
	
	char *display_name;
	
	time_t mtime; /* Only used for absolute filenames */

	CircularList recently_used_node;
	int age; /* zero:ed on access, incremented each sweep */
} CacheIcon;

/* The icon factory.
 * These are just globals, but they're in an object so we can
 * connect signals and have multiple icon factories some day
 * if we want to.
 */
typedef struct {
	GObject object;

	/* A hash table that contains the icons. A circular list of
	 * the most recently used icons is kept around, and we don't
	 * let them go when we sweep the cache.
	 */
	GHashTable *icon_cache;

	/* frames to use for thumbnail icons */
	GdkPixbuf *thumbnail_frame;

	/* Used for icon themes according to the freedesktop icon spec. */
	GtkIconTheme *icon_theme;
	GnomeThumbnailFactory *thumbnail_factory;

	CircularList recently_used_dummy_head;
	guint recently_used_count;
        guint sweep_timer;

	CacheIcon *fallback_icon;
	GHashTable *image_mime_types;

	GList *async_thumbnail_load_handles;
} NautilusIconFactory;

#define NAUTILUS_ICON_FACTORY(obj) \
	GTK_CHECK_CAST (obj, nautilus_icon_factory_get_type (), NautilusIconFactory)

typedef struct {
	GObjectClass parent_class;
} NautilusIconFactoryClass;

enum {
	ICONS_CHANGED,
	LAST_SIGNAL
};
static guint signals[LAST_SIGNAL];


static int cached_thumbnail_limit;
static int cached_thumbnail_size;
static int show_image_thumbs;

/* forward declarations */

static GType      nautilus_icon_factory_get_type         (void);
static void       nautilus_icon_factory_class_init       (NautilusIconFactoryClass *class);
static void       nautilus_icon_factory_instance_init    (NautilusIconFactory      *factory);
static void       nautilus_icon_factory_finalize         (GObject                  *object);
static void       thumbnail_limit_changed_callback       (gpointer                  user_data);
static void       thumbnail_size_changed_callback       (gpointer                  user_data);
static void       show_thumbnails_changed_callback       (gpointer                  user_data);
static void       mime_type_data_changed_callback        (GnomeVFSMIMEMonitor	   *monitor,
							  gpointer                  user_data);
static guint      cache_key_hash                         (gconstpointer             p);
static gboolean   cache_key_equal                        (gconstpointer             a,
							  gconstpointer             b);
static void       cache_key_destroy                      (CacheKey                 *key);
static void       cache_icon_unref                       (CacheIcon                *icon);
static CacheIcon *cache_icon_new                         (GdkPixbuf                *pixbuf,
							  GtkIconInfo              *icon_info,
							  double                    scale_x,
							  double                    scale_y);
static CacheIcon *get_icon_from_cache                    (const char               *icon,
							  const char               *modifier,
							  guint                     nominal_size,
							  gboolean		    force_nominal);
static void nautilus_icon_factory_clear                  (gboolean                  clear_pathnames);

GNOME_CLASS_BOILERPLATE (NautilusIconFactory,
			 nautilus_icon_factory,
			 GObject, G_TYPE_OBJECT);

static NautilusIconFactory *global_icon_factory = NULL;

static void
destroy_icon_factory (void)
{
	eel_preferences_remove_callback (NAUTILUS_PREFERENCES_IMAGE_FILE_THUMBNAIL_LIMIT,
					 thumbnail_limit_changed_callback,
					 NULL);
	eel_preferences_remove_callback (NAUTILUS_PREFERENCES_ICON_VIEW_THUMBNAIL_SIZE,
					 thumbnail_size_changed_callback,
					 NULL);
	eel_preferences_remove_callback (NAUTILUS_PREFERENCES_SHOW_IMAGE_FILE_THUMBNAILS,
					 show_thumbnails_changed_callback,
					 NULL);
	g_object_unref (global_icon_factory);
}

/* Return a pointer to the single global icon factory. */
static NautilusIconFactory *
get_icon_factory (void)
{
        if (global_icon_factory == NULL) {
		nautilus_global_preferences_init ();

		global_icon_factory = NAUTILUS_ICON_FACTORY
			(g_object_new (nautilus_icon_factory_get_type (), NULL));

		thumbnail_limit_changed_callback (NULL);
		eel_preferences_add_callback (NAUTILUS_PREFERENCES_IMAGE_FILE_THUMBNAIL_LIMIT,
					      thumbnail_limit_changed_callback,
					      NULL);

		thumbnail_size_changed_callback (NULL);
		eel_preferences_add_callback (NAUTILUS_PREFERENCES_ICON_VIEW_THUMBNAIL_SIZE,
					      thumbnail_size_changed_callback,
					      NULL);

		show_thumbnails_changed_callback (NULL);
		eel_preferences_add_callback (NAUTILUS_PREFERENCES_SHOW_IMAGE_FILE_THUMBNAILS,
					      show_thumbnails_changed_callback,
					      NULL);

		g_signal_connect (gnome_vfs_mime_monitor_get (),
				  "data_changed",
				  G_CALLBACK (mime_type_data_changed_callback),
				  NULL);

		eel_debug_call_at_shutdown (destroy_icon_factory);
        }
        return global_icon_factory;
}

GObject *
nautilus_icon_factory_get (void)
{
	return G_OBJECT (get_icon_factory ());
}

static void
icon_theme_changed_callback (GnomeIconTheme *icon_theme,
			     gpointer user_data)
{
	NautilusIconFactory *factory;

	nautilus_icon_factory_clear (FALSE);

	factory = user_data;

	g_signal_emit (factory,
		       signals[ICONS_CHANGED], 0);
}

GtkIconTheme *
nautilus_icon_factory_get_icon_theme (void)
{
	NautilusIconFactory *factory;

	factory = get_icon_factory ();

	return g_object_ref (factory->icon_theme);
}

GnomeThumbnailFactory *
nautilus_icon_factory_get_thumbnail_factory (void)
{
	NautilusIconFactory *factory;

	factory = get_icon_factory ();

	return g_object_ref (factory->thumbnail_factory);
}


static void
check_recently_used_list (void)
{
#if CACHE_SELF_CHECKS
	NautilusIconFactory *factory;
	CircularList *head, *node, *next;
	guint count;

	factory = get_icon_factory ();

	head = &factory->recently_used_dummy_head;
	
	count = 0;
	
	node = head;
	while (1) {
		next = node->next;
		g_assert (next != NULL);
		g_assert (next->prev == node);

		if (next == head) {
			break;
		}

		count += 1;

		node = next;
	}

	g_assert (count == factory->recently_used_count);
#endif
}


/* load the thumbnail frame */
static void
load_thumbnail_frame (NautilusIconFactory *factory)
{
	char *image_path;
	
	image_path = nautilus_pixmap_file ("thumbnail_frame.png");
	if (factory->thumbnail_frame != NULL) {
		g_object_unref (factory->thumbnail_frame);
	}
	if (image_path != NULL) {
		factory->thumbnail_frame = gdk_pixbuf_new_from_file (image_path, NULL);
	}
	g_free (image_path);
}

typedef struct {
	NautilusFile *file;
	char *modifier;
	guint nominal_size;
	gboolean force_nominal;
} AsnycThumbnailLoadFuncData;

static void
async_thumbnail_load_func (NautilusThumbnailAsyncLoadHandle *handle,
			   const char *path,
			   GdkPixbuf  *pixbuf,
			   double scale_x,
			   double scale_y,
			   gpointer user_data)
{
	NautilusIconFactory *factory;
	GHashTable *hash_table;
	CacheKey *key;
	CacheIcon *cached_icon;
	struct stat statbuf;
	AsnycThumbnailLoadFuncData *data = user_data;

	factory = get_icon_factory ();
	hash_table = factory->icon_cache;

	nautilus_file_set_is_thumbnailing (data->file, FALSE);
	factory->async_thumbnail_load_handles = 
		g_list_remove (factory->async_thumbnail_load_handles, handle);

	if (stat (path, &statbuf) != 0 ||
	    !S_ISREG (statbuf.st_mode)) {
		g_message ("NautilusIconFactory: Failed to determine mtime for %s. Aborting thumbnailing request.", path);
		goto out;
	}

	if (!gdk_pixbuf_get_has_alpha (pixbuf)) {
		/* we don't own the pixbuf, but nautilus_thumbnail_frame_image() assumes so and unrefs it. */
		g_object_ref (pixbuf);

		nautilus_thumbnail_frame_image (&pixbuf);
		/* at this point, we own a pixbuf, which is the framed version of the passed-in pixbuf. */
	}

	cached_icon = cache_icon_new (pixbuf, NULL, scale_x, scale_y);
	cached_icon->mtime = statbuf.st_mtime;

	if (!gdk_pixbuf_get_has_alpha (pixbuf)) {
		g_object_unref (pixbuf);
	}

	if (cached_icon != NULL) {
		key = g_new (CacheKey, 1);
		key->name = g_strdup (path);
		key->modifier = g_strdup (data->modifier);
		key->nominal_size = data->nominal_size;
		key->force_nominal = data->force_nominal;

		g_hash_table_insert (hash_table, key, cached_icon);

		nautilus_file_changed (data->file);
	}

out:
	nautilus_file_unref (data->file);
	g_free (data->modifier);
	g_free (data);
}



static void
nautilus_icon_factory_instance_init (NautilusIconFactory *factory)
{
	GdkPixbuf *pixbuf;
	guint i;
	static const char *types [] = {
		"image/x-bmp", "image/x-ico", "image/jpeg", "image/gif",
		"image/png", "image/pnm", "image/ras", "image/tga",
		"image/tiff", "image/wbmp", "image/bmp", "image/x-xbitmap",
		"image/x-xpixmap"
        };

	
        factory->icon_cache = g_hash_table_new_full (cache_key_hash,
						     cache_key_equal,
						     (GDestroyNotify)cache_key_destroy,
						     (GDestroyNotify)cache_icon_unref);
	
	factory->icon_theme = gtk_icon_theme_get_default ();
	g_signal_connect_object (factory->icon_theme,
				 "changed",
				 G_CALLBACK (icon_theme_changed_callback),
				 factory, 0);


	factory->thumbnail_factory = gnome_thumbnail_factory_new (GNOME_THUMBNAIL_SIZE_NORMAL);
	load_thumbnail_frame (factory);

	/* Empty out the recently-used list. */
	factory->recently_used_dummy_head.next = &factory->recently_used_dummy_head;
	factory->recently_used_dummy_head.prev = &factory->recently_used_dummy_head;
	
	pixbuf = gdk_pixbuf_new_from_data (nautilus_default_file_icon,
					   GDK_COLORSPACE_RGB,
					   TRUE,
					   8,
					   nautilus_default_file_icon_width,
					   nautilus_default_file_icon_height,
					   nautilus_default_file_icon_width * 4, /* stride */
					   NULL, /* don't destroy data */
					   NULL);
	
	factory->fallback_icon = cache_icon_new (pixbuf, NULL, 1.0, 1.0);

	g_object_unref(pixbuf);

	factory->image_mime_types = g_hash_table_new (g_str_hash, g_str_equal);
	for (i = 0; i < G_N_ELEMENTS (types); i++) {
		g_hash_table_insert (factory->image_mime_types,
				     (gpointer) types [i],
				     GUINT_TO_POINTER (1));
	}
}

static void
nautilus_icon_factory_class_init (NautilusIconFactoryClass *class)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (class);

	signals[ICONS_CHANGED]
		= g_signal_new ("icons_changed",
		                G_TYPE_FROM_CLASS (object_class),
		                G_SIGNAL_RUN_LAST,
		                0,
		                NULL, NULL,
		                g_cclosure_marshal_VOID__VOID,
		                G_TYPE_NONE, 0);

	object_class->finalize = nautilus_icon_factory_finalize;
}

static void
cache_key_destroy (CacheKey *key)
{
	g_free (key->name);
	g_free (key->modifier);
	g_free (key);
}

static CacheIcon *
cache_icon_new (GdkPixbuf *pixbuf,
		GtkIconInfo *info,
		double scale_x, double scale_y)
{
	CacheIcon *icon;
	GdkRectangle rect;
	int i;

	/* Grab the pixbuf since we are keeping it. */
	g_object_ref (pixbuf);

	/* Make the icon. */
	icon = g_new0 (CacheIcon, 1);
	icon->ref_count = 1;
	icon->pixbuf = pixbuf;
	icon->mtime = 0;

	if (info) {
		icon->display_name = g_strdup (gtk_icon_info_get_display_name (info));
		
		if (gtk_icon_info_get_embedded_rect (info, &rect)) {
			rect.x *= scale_x;
			rect.width *= scale_x;
			rect.y *= scale_y;
			rect.height *= scale_y;
			icon->embedded_text_rect = g_memdup (&rect, sizeof (rect));
			
		}

		if (gtk_icon_info_get_attach_points (info,
						     &icon->attach_points,
						     &icon->n_attach_points)) {
			for (i = 0; i < icon->n_attach_points; i++) {
				icon->attach_points[i].x *= scale_x;
				icon->attach_points[i].y *= scale_x;
			}
		}
		
	}
	
	return icon;
}

static  void
cache_icon_ref (CacheIcon *icon)
{
	g_assert (icon != NULL);
	g_assert (icon->ref_count >= 1);

	icon->ref_count++;
}

static void
cache_icon_unref (CacheIcon *icon)
{
	CircularList *node;
        NautilusIconFactory *factory;
	
	g_assert (icon != NULL);
	g_assert (icon->ref_count >= 1);

	if (icon->ref_count > 1) {
		icon->ref_count--;
		return;
	}
	
	icon->ref_count = 0;

	factory = get_icon_factory ();
	
	check_recently_used_list ();

	/* If it's in the recently used list, free it from there */      
	node = &icon->recently_used_node;
	if (node->next != NULL) {
#if CACHE_SELF_CHECKS
		g_assert (factory->recently_used_count >= 1);
		
		g_assert (node->next->prev == node);
		g_assert (node->prev->next == node);
		g_assert (node->next != node);
		g_assert (node->prev != node);
#endif
		node->next->prev = node->prev;
		node->prev->next = node->next;

		node->next = NULL;
		node->prev = NULL;

		factory->recently_used_count -= 1;
	}
	
	check_recently_used_list ();
	
	g_object_unref (icon->pixbuf);
	
	g_free (icon->display_name);
	g_free (icon->embedded_text_rect);
	g_free (icon->attach_points);

	g_free (icon);
}


static gboolean
nautilus_icon_factory_possibly_free_cached_icon (gpointer key,
						 gpointer value,
						 gpointer user_data)
{
        CacheIcon *icon;
	
	icon = value;

	/* Don't free a cache entry that is in the recently used list. */
        if (icon->recently_used_node.next != NULL) {
                return FALSE;
	}

	/* Don't free a cache entry if the pixbuf is still in use. */
	if (G_OBJECT (icon->pixbuf)->ref_count > 1) {
		return FALSE;
	}

	icon->age++;
	
	if (icon->age > ICON_MAX_AGE) {
		return TRUE;
	}

	/* Free the item. */
        return TRUE;
}


/* Sweep the cache, freeing any icons that are not in use and are
 * also not recently used.
 */
static gboolean
nautilus_icon_factory_sweep (gpointer user_data)
{
        NautilusIconFactory *factory;

	factory = user_data;

	g_hash_table_foreach_remove (factory->icon_cache,
				     nautilus_icon_factory_possibly_free_cached_icon,
				     NULL);

	factory->sweep_timer = 0;
	return FALSE;
}

/* Schedule a timer to do a sweep. */
static void
nautilus_icon_factory_schedule_sweep (NautilusIconFactory *factory)
{
        if (factory->sweep_timer != 0) {
                return;
	}

        factory->sweep_timer = g_timeout_add (ICON_CACHE_SWEEP_TIMEOUT,
					      nautilus_icon_factory_sweep,
					      factory);
}

/* Move this item to the head of the recently-used list,
 * bumping the last item off that list if necessary.
 */
static void
mark_recently_used (CircularList *node)
{
	NautilusIconFactory *factory;
	CircularList *head, *last_node;

	check_recently_used_list ();

	factory = get_icon_factory ();
	head = &factory->recently_used_dummy_head;

	/* Move the node to the start of the list. */
	if (node->prev != head) {
		if (node->next != NULL) {
			/* Remove the node from its current position in the list. */
			node->next->prev = node->prev;
			node->prev->next = node->next;
		} else {
			/* Node was not already in the list, so add it.
			 * If the list is already full, remove the last node.
			 */
			if (factory->recently_used_count < ICON_CACHE_COUNT) {
				factory->recently_used_count += 1;
			} else {
				/* Remove the last node. */
				last_node = head->prev;

#if CACHE_SELF_CHECKS
				g_assert (last_node != head);
				g_assert (last_node != node);
#endif
				
				head->prev = last_node->prev;
				last_node->prev->next = head;

				last_node->prev = NULL;
				last_node->next = NULL;
			}
		}
		
		/* Insert the node at the head of the list. */
		node->prev = head;
		node->next = head->next;
		node->next->prev = node;
		head->next = node;
	}

	check_recently_used_list ();
}

static gboolean
remove_all (gpointer key, gpointer value, gpointer user_data)
{
	/* Tell the caller to remove the hash table entry. */
        return TRUE;
}

static gboolean
remove_non_pathnames (gpointer _key, gpointer value, gpointer user_data)
{
	CacheKey *key = _key;
	
	if (key->name && key->name[0] == '/') {
		return FALSE;
	}
	    
        return TRUE; /* Tell the caller to remove the hash table entry. */
}

/* Reset the cache to the default state.
   Clear pathnames can be set to FALSE which means we only clear icon names, not
   absolute pathnames. This is useful to avoid throwing away all loaded thumbnails. */
static void
nautilus_icon_factory_clear (gboolean clear_pathnames)
{
	NautilusIconFactory *factory;
	CircularList *head;
	
	factory = get_icon_factory ();

        g_hash_table_foreach_remove (factory->icon_cache,
				     clear_pathnames ? remove_all : remove_non_pathnames,
                                     NULL);
	
	/* Empty out the recently-used list. */
	head = &factory->recently_used_dummy_head;

	if (clear_pathnames) {
		/* fallback_icon hangs around, but we don't know if it
		 * was ever inserted in the list
		 */
		g_assert (factory->recently_used_count == 0 ||
			  factory->recently_used_count == 1);
		if (factory->recently_used_count == 1) {
			/* make sure this one is the fallback_icon */
			g_assert (head->next == head->prev);
			g_assert (&factory->fallback_icon->recently_used_node == head->next);
		}
	}
		
}

static void
cancel_thumbnail_read_foreach (gpointer data,
			       gpointer user_data)
{
	NautilusThumbnailAsyncLoadHandle *handle = data;
	nautilus_thumbnail_load_image_cancel (handle);
}

static void
nautilus_icon_factory_finalize (GObject *object)
{
	NautilusIconFactory *factory;

	factory = NAUTILUS_ICON_FACTORY (object);

	g_list_foreach (factory->async_thumbnail_load_handles, cancel_thumbnail_read_foreach, NULL);
	g_list_free (factory->async_thumbnail_load_handles);

	if (factory->icon_cache) {
		g_hash_table_destroy (factory->icon_cache);
		factory->icon_cache = NULL;
	}
	
	if (factory->thumbnail_frame != NULL) {
		g_object_unref (factory->thumbnail_frame);
		factory->thumbnail_frame = NULL;
	}

	if (factory->fallback_icon) {
		g_assert (factory->fallback_icon->ref_count == 1);
		cache_icon_unref (factory->fallback_icon);
	}

	if (factory->image_mime_types) {
		g_hash_table_destroy (factory->image_mime_types);
		factory->image_mime_types = NULL;
	}
	
	EEL_CALL_PARENT (G_OBJECT_CLASS, finalize, (object));
}

static void
thumbnail_limit_changed_callback (gpointer user_data)
{
	cached_thumbnail_limit = eel_preferences_get_integer (NAUTILUS_PREFERENCES_IMAGE_FILE_THUMBNAIL_LIMIT);

	/* Tell the world that icons might have changed. We could invent a narrower-scope
	 * signal to mean only "thumbnails might have changed" if this ends up being slow
	 * for some reason.
	 */
	nautilus_icon_factory_clear (TRUE);
	g_signal_emit (global_icon_factory,
			 signals[ICONS_CHANGED], 0);
}

static void
thumbnail_size_changed_callback (gpointer user_data)
{
	cached_thumbnail_size = eel_preferences_get_integer (NAUTILUS_PREFERENCES_ICON_VIEW_THUMBNAIL_SIZE);

	/* Tell the world that icons might have changed. We could invent a narrower-scope
	 * signal to mean only "thumbnails might have changed" if this ends up being slow
	 * for some reason.
	 */
	nautilus_icon_factory_clear (TRUE);
	g_signal_emit (global_icon_factory,
			 signals[ICONS_CHANGED], 0);
}

static void
show_thumbnails_changed_callback (gpointer user_data)
{
	show_image_thumbs = eel_preferences_get_enum (NAUTILUS_PREFERENCES_SHOW_IMAGE_FILE_THUMBNAILS);

	nautilus_icon_factory_clear (TRUE);
	/* If the user disabled thumbnailing, remove all outstanding thumbnails */ 
	if (show_image_thumbs == NAUTILUS_SPEED_TRADEOFF_NEVER) {
		nautilus_thumbnail_remove_all_from_queue ();
	}
	g_signal_emit (global_icon_factory,
		       signals[ICONS_CHANGED], 0);
}

static void       
mime_type_data_changed_callback (GnomeVFSMIMEMonitor *monitor, gpointer user_data)
{
	g_assert (monitor != NULL);
	g_assert (user_data == NULL);

	/* We don't know which data changed, so we have to assume that
	 * any or all icons might have changed.
	 */
	nautilus_icon_factory_clear (FALSE);
	g_signal_emit (get_icon_factory (), 
			 signals[ICONS_CHANGED], 0);
}				 

static char *
nautilus_remove_icon_file_name_suffix (const char *icon_name)
{
	guint i;
	const char *suffix;
	static const char *icon_file_name_suffixes[] = { ".svg", ".svgz", ".png", ".jpg", ".xpm" };

	for (i = 0; i < G_N_ELEMENTS (icon_file_name_suffixes); i++) {
		suffix = icon_file_name_suffixes[i];
		if (eel_str_has_suffix (icon_name, suffix)) {
			return eel_str_strip_trailing_str (icon_name, suffix);
		}
	}
	return g_strdup (icon_name);
}

static char *
image_uri_to_name_or_uri (const char *image_uri)
{
	char *icon_path;

	icon_path = gnome_vfs_get_local_path_from_uri (image_uri);
	if (icon_path == NULL && image_uri[0] == '/') {
		icon_path = g_strdup (image_uri);
	}
	if (icon_path != NULL) {
		return icon_path;
	} else if (strpbrk (image_uri, ":/") == NULL) {
		return nautilus_remove_icon_file_name_suffix (image_uri);
	}
	return NULL;
}

static gboolean
mimetype_limited_by_size (const char *mime_type)
{
	NautilusIconFactory *factory;

	factory = get_icon_factory();

        if (g_hash_table_lookup (factory->image_mime_types, mime_type)) {
                return TRUE;
	}

        return FALSE;
}

static gboolean
should_show_thumbnail (NautilusFile *file, const char *mime_type)
{
	if (mimetype_limited_by_size (mime_type) &&
	    nautilus_file_get_size (file) > (unsigned int)cached_thumbnail_limit) {
		return FALSE;
	}
	
	if (show_image_thumbs == NAUTILUS_SPEED_TRADEOFF_ALWAYS) {
		return TRUE;
	} else if (show_image_thumbs == NAUTILUS_SPEED_TRADEOFF_NEVER) {
		return FALSE;
	} else {
		/* only local files */
		return nautilus_file_is_local (file);
	}

	return FALSE;
}

static char *
get_special_icon_for_file (NautilusFile *file)
{
	char *uri, *ret;

	if (file == NULL) {
		return NULL;
	}

	if (nautilus_file_is_home (file)) {
		return ICON_NAME_HOME;
	}

	ret = NULL;
	uri = nautilus_file_get_uri (file);

	if (strcmp (uri, "burn:///") == 0) {
		ret = "nautilus-cd-burner";
	} else if (strcmp (uri, "computer:///") == 0) {
		ret = "gnome-fs-client";
	} else if ((strcmp (uri, "network:///") == 0)
		   || (strcmp (uri, "smb:///") == 0)) {
		ret = "gnome-fs-network";
	} else if (strcmp (uri, EEL_TRASH_URI) == 0) {
		if (nautilus_trash_monitor_is_empty ()) {
			ret = ICON_NAME_TRASH_EMPTY;
		} else {
			ret = ICON_NAME_TRASH_FULL;
		}
	} else if (eel_uri_is_search (uri)) {
		/* FIXME: We really need a better icon than this */
		ret = "gnome-searchtool";
	}

	g_free (uri);

	return ret;
}

static gint
gtk_icon_size_to_nominal_size (GtkIconSize icon_size)
{
	gint ret;

	g_assert (gtk_icon_size_lookup (icon_size, &ret, NULL));

	return ret;
}

/* key routine to get the icon for a file */
char *
nautilus_icon_factory_get_icon_for_file (NautilusFile *file, gboolean embedd_text)
{
 	char *custom_uri, *file_uri, *icon_name, *mime_type, *custom_icon, *special_icon;
	NautilusIconFactory *factory;
	GnomeIconLookupResultFlags lookup_result;
	GnomeVFSFileInfo *file_info;
	GnomeThumbnailFactory *thumb_factory;
	gboolean show_thumb;
	GnomeIconLookupFlags lookup_flags;
	
	if (file == NULL) {
		return NULL;
	}

	factory = get_icon_factory ();
	
	custom_icon = NULL;
 
	/* Custom icon set by user, taken from metadata */
 	custom_uri = nautilus_file_get_custom_icon (file);
	if (custom_uri) {
		custom_icon = image_uri_to_name_or_uri (custom_uri);
	}
 	g_free (custom_uri);

	/* Icon for "special files" (burn, computer, network, smb, trash) */
	special_icon = get_special_icon_for_file (file);
	if (special_icon != NULL) {
		return g_strdup (special_icon);
	}

	file_uri = nautilus_file_get_uri (file);

	mime_type = nautilus_file_get_mime_type (file);
	
	file_info = nautilus_file_peek_vfs_file_info (file);
	
	show_thumb = should_show_thumbnail (file, mime_type);	
	
	if (show_thumb) {
		thumb_factory = factory->thumbnail_factory;
	} else {
		thumb_factory = NULL;
	}

	lookup_flags = GNOME_ICON_LOOKUP_FLAGS_SHOW_SMALL_IMAGES_AS_THEMSELVES;
	if (embedd_text) {
		lookup_flags |= GNOME_ICON_LOOKUP_FLAGS_EMBEDDING_TEXT;
	}
	icon_name = gnome_icon_lookup (factory->icon_theme,
				       thumb_factory,
				       file_uri,
				       custom_icon,
				       nautilus_file_peek_vfs_file_info (file),
				       mime_type,
				       lookup_flags,
				       &lookup_result);


	/* Create thumbnails if we can, and if the looked up icon isn't a thumbnail
	   or an absolute pathname (custom icon or image as itself) */
	if (show_thumb &&
	    !(lookup_result & GNOME_ICON_LOOKUP_RESULT_FLAGS_THUMBNAIL) &&
	    icon_name[0] != '/' && file_info &&
	    gnome_thumbnail_factory_can_thumbnail (factory->thumbnail_factory,
						   file_uri,
						   mime_type,
						   file_info->mtime)) {
		nautilus_create_thumbnail (file);
		g_free (icon_name);
		icon_name = g_strdup (ICON_NAME_THUMBNAIL_LOADING);
	}
	
        g_free (file_uri);
        g_free (custom_icon);
	g_free (mime_type);
	
	return icon_name;
}

/**
 * nautilus_icon_factory_get_required_file_attributes
 * 
 * Get the file attributes required to obtain a file's icon.
 */
NautilusFileAttributes 
nautilus_icon_factory_get_required_file_attributes (void)
{
	return NAUTILUS_FILE_ATTRIBUTE_CUSTOM_ICON |
		NAUTILUS_FILE_ATTRIBUTE_MIME_TYPE;
}


/**
 * nautilus_icon_factory_is_icon_ready_for_file
 * 
 * Check whether a NautilusFile has enough information to report
 * what its icon should be.
 * 
 * @file: The NautilusFile in question.
 */
gboolean
nautilus_icon_factory_is_icon_ready_for_file (NautilusFile *file)
{
	NautilusFileAttributes attributes;
	gboolean result;

	attributes = nautilus_icon_factory_get_required_file_attributes ();
	result = nautilus_file_check_if_ready (file, attributes) ||
		 (get_special_icon_for_file (file) != NULL);

	return result;
}

char *
nautilus_icon_factory_get_emblem_icon_by_name (const char *emblem_name)
{
	char *name_with_prefix;

	name_with_prefix = g_strconcat (NAUTILUS_EMBLEM_NAME_PREFIX, emblem_name, NULL);

	return name_with_prefix;
}

guint
nautilus_icon_factory_get_emblem_size_for_icon_size (guint size)
{
	if (size >= 96)
		return 48;
	if (size >= 64)
		return 32;
	if (size >= 48)
		return 24;
	if (size >= 32)
		return 16;
	
	return 0; /* no emblems for smaller sizes */
}

GList *
nautilus_icon_factory_get_emblem_icons_for_file (NautilusFile *file,
						 EelStringList *exclude)
{
	GList *icons, *emblem_names, *node;
	char *uri, *name;
	char *icon;
	gboolean file_is_trash;

	icons = NULL;

	emblem_names = nautilus_file_get_emblem_names (file);
	for (node = emblem_names; node != NULL; node = node->next) {
		name = node->data;
		if (strcmp (name, NAUTILUS_FILE_EMBLEM_NAME_TRASH) == 0) {
			/* Leave out the trash emblem for the trash itself, since
			 * putting a trash emblem on a trash icon is gilding the
			 * lily.
			 */
			uri = nautilus_file_get_uri (file);
			file_is_trash = strcmp (uri, EEL_TRASH_URI) == 0;
			g_free (uri);
			if (file_is_trash) {
				continue;
			}
		}
		if (eel_string_list_contains (exclude, name)) {
			continue;
		}
		icon = nautilus_icon_factory_get_emblem_icon_by_name (name);
		icons = g_list_prepend (icons, icon);
	}
	eel_g_list_free_deep (emblem_names);

	return g_list_reverse (icons);
}

guint
nautilus_icon_factory_get_larger_icon_size (guint size)
{
	if (size < NAUTILUS_ICON_SIZE_SMALLEST) {
		return NAUTILUS_ICON_SIZE_SMALLEST;
	}
	if (size < NAUTILUS_ICON_SIZE_SMALLER) {
		return NAUTILUS_ICON_SIZE_SMALLER;
	}
	if (size < NAUTILUS_ICON_SIZE_SMALL) {
		return NAUTILUS_ICON_SIZE_SMALL;
	}
	if (size < NAUTILUS_ICON_SIZE_STANDARD) {
		return NAUTILUS_ICON_SIZE_STANDARD;
	}
	if (size < NAUTILUS_ICON_SIZE_LARGE) {
		return NAUTILUS_ICON_SIZE_LARGE;
	}
	if (size < NAUTILUS_ICON_SIZE_LARGER) {
		return NAUTILUS_ICON_SIZE_LARGER;
	}
	return NAUTILUS_ICON_SIZE_LARGEST;
}

guint
nautilus_icon_factory_get_smaller_icon_size (guint size)
{
	if (size > NAUTILUS_ICON_SIZE_LARGEST) {
		return NAUTILUS_ICON_SIZE_LARGEST;
	}
	if (size > NAUTILUS_ICON_SIZE_LARGER) {
		return NAUTILUS_ICON_SIZE_LARGER;
	}
	if (size > NAUTILUS_ICON_SIZE_LARGE) {
		return NAUTILUS_ICON_SIZE_LARGE;
	}
	if (size > NAUTILUS_ICON_SIZE_STANDARD) {
		return NAUTILUS_ICON_SIZE_STANDARD;
	}
	if (size > NAUTILUS_ICON_SIZE_SMALL) {
		return NAUTILUS_ICON_SIZE_SMALL;
	}
	if (size > NAUTILUS_ICON_SIZE_SMALLER) {
		return NAUTILUS_ICON_SIZE_SMALLER;
	}
	return NAUTILUS_ICON_SIZE_SMALLEST;
}



/* This loads an SVG image, scaling it to the appropriate size. */
static GdkPixbuf *
load_pixbuf_svg (const char *path,
		 guint size_in_pixels,
		 guint base_size,
		 double *scale_x,
		 double *scale_y)
{
	double zoom;
	int width, height;
	GdkPixbuf *pixbuf;

	if (base_size != 0) {
		zoom = (double)size_in_pixels / base_size;

		pixbuf = rsvg_pixbuf_from_file_at_zoom_with_max (path, zoom, zoom, size_in_pixels, size_in_pixels, NULL);
	} else {
		pixbuf = rsvg_pixbuf_from_file_at_max_size (path,
							    size_in_pixels,
							    size_in_pixels,
							    NULL);
	}

	if (pixbuf == NULL) {
		return NULL;
	}

	width = gdk_pixbuf_get_width (pixbuf);
	height = gdk_pixbuf_get_height (pixbuf);
	*scale_x = width / 1000.0;
	*scale_y = height / 1000.0;

	return pixbuf;
}

static gboolean
path_represents_svg_image (const char *path) 
{
	/* Synchronous mime sniffing is a really bad idea here
	 * since it's only useful for people adding custom icons,
	 * and if they're doing that, they can behave themselves
	 * and use a .svg extension.
	 */
	return path != NULL && (g_str_has_suffix (path, ".svg") || g_str_has_suffix (path, ".svgz"));
}

static GdkPixbuf *
load_icon_file (const char    *filename,
		guint          base_size,
		guint          nominal_size,
		gboolean       force_nominal,
		double        *scale_x,
		double        *scale_y)
{
	GdkPixbuf *pixbuf;
	gboolean add_frame;

	add_frame = FALSE;

	*scale_x = 1.0;
	*scale_y = 1.0;
	
	if (path_represents_svg_image (filename)) {
		pixbuf = load_pixbuf_svg (filename,
					  nominal_size,
					  force_nominal ? 0 : base_size,
					  scale_x, scale_y);
	} else {
		int original_size;
		gboolean is_thumbnail;

		/* FIXME: Maybe we shouldn't have to load the file each time
		 * Not sure if that is important */
		pixbuf = nautilus_thumbnail_load_image (filename,
							base_size,
							nominal_size,
							force_nominal,
							scale_x,
							scale_y);

		if (pixbuf == NULL) {
			return NULL;
		}

		is_thumbnail = strstr (filename, "/.thumbnails/") != NULL;

		original_size = ceil (MAX (gdk_pixbuf_get_width (pixbuf) / *scale_x, gdk_pixbuf_get_height (pixbuf) / *scale_y));

		if ((is_thumbnail || (!force_nominal && base_size == 0 && original_size > cached_thumbnail_size))
		     && !gdk_pixbuf_get_has_alpha (pixbuf)) {
			add_frame = TRUE;
		}
	}

	if (add_frame) {
		nautilus_thumbnail_frame_image(&pixbuf);
	}

	return pixbuf;
}

static CacheIcon *
create_normal_cache_icon (const char *icon,
			  const char *modifier,
			  guint       nominal_size,
			  gboolean    force_nominal)
{
	NautilusIconFactory *factory;
	const char *filename;
	char *name_with_modifier;
	GtkIconInfo *info;
	CacheIcon *cache_icon;
	GdkPixbuf *pixbuf;
	int base_size;
	struct stat statbuf;
	time_t mtime;
	double scale_x, scale_y;
		
	factory = get_icon_factory ();

	info = NULL;
	filename = NULL;

	mtime = 0;
	
	base_size = 0;
	if (icon[0] == '/') {
		/* FIXME: maybe we should add modifier to the filename
		 *        before the extension */
		if (stat (icon, &statbuf) == 0 &&
		    S_ISREG (statbuf.st_mode)) {
			filename = icon;
			mtime = statbuf.st_mtime;
		}
	} else {
		if (modifier) {
			name_with_modifier = g_strconcat (icon, "-", modifier, NULL);
		} else {
			name_with_modifier = (char *)icon;
		}

		info = gtk_icon_theme_lookup_icon (factory->icon_theme,
						   name_with_modifier,
						   nominal_size,
						   GTK_ICON_LOOKUP_FORCE_SVG);
		if (name_with_modifier != icon) {
			g_free (name_with_modifier);
		}
		
		if (info == NULL) {
			return NULL;
		}
		
		gtk_icon_info_set_raw_coordinates (info, TRUE);
		base_size = gtk_icon_info_get_base_size (info);
		filename = gtk_icon_info_get_filename (info);
	}

	/* If e.g. the absolute filename doesn't exist */
	if (filename == NULL) {
		return NULL;
	}
	
	pixbuf = load_icon_file (filename,
				 base_size,
				 nominal_size,
				 force_nominal,
				 &scale_x, &scale_y);
	if (pixbuf == NULL) {
		if (info) {
			gtk_icon_info_free (info);
		}
		return NULL;
	}
	
	cache_icon = cache_icon_new (pixbuf, info, scale_x, scale_y);
	cache_icon->mtime = mtime;

	if (info) {
		gtk_icon_info_free (info);
	}
	g_object_unref (pixbuf);
	
	return cache_icon;
}

static CacheIcon *
lookup_icon_from_cache (const char *icon,
			const char *modifier,
			guint       nominal_size,
			gboolean    force_nominal)
{
	NautilusIconFactory *factory;
	GHashTable *hash_table;
	CacheKey lookup_key, *key;
	CacheIcon *value;

	lookup_key.name = (char *)icon;
	lookup_key.modifier = (char *)modifier;
	lookup_key.nominal_size = nominal_size;
	lookup_key.force_nominal = force_nominal;

	factory = get_icon_factory ();
	hash_table = factory->icon_cache;

	if (g_hash_table_lookup_extended (hash_table, &lookup_key,
					  (gpointer *) &key, (gpointer *) &value)) {
		/* Found it in the table. */
		g_assert (key != NULL);
		g_assert (value != NULL);
	} else {
		key = NULL;
		value = NULL;
	}

	return value;
}
			

/* Get the icon, handling the caching.
 * If @picky is true, then only an unscaled icon is acceptable.
 * Also, if @picky is true, the icon must be a custom icon if
 * @custom is true or a standard icon is @custom is false.
 * If @force_nominal is #TRUE, the returned icon will be guaranteed
 * to be smaller than the nominal size
 */
static CacheIcon *
get_icon_from_cache (const char *icon,
		     const char *modifier,
		     guint       nominal_size,
		     gboolean    force_nominal)
{
	NautilusIconFactory *factory;
	GHashTable *hash_table;
	CacheKey *key;
	CacheIcon *cached_icon;
	struct stat statbuf;
	
	g_return_val_if_fail (icon != NULL, NULL);
	
	factory = get_icon_factory ();
	hash_table = factory->icon_cache;

	/* Check to see if it's already in the table. */
	cached_icon = lookup_icon_from_cache (icon, modifier, nominal_size, force_nominal);

	/* Make sure that thumbnails and image-as-itself icons gets
	   reloaded when they change: */
	if (cached_icon && icon[0] == '/') {
		if (stat (icon, &statbuf) != 0 ||
		    !S_ISREG (statbuf.st_mode) ||
		    statbuf.st_mtime != cached_icon->mtime) {
			cached_icon = NULL;
		}
	}

	if (cached_icon == NULL) {
		/* Not in the table, so load the image. */
		
		/*
		g_print ("cache miss for %s:%s:%s:%d\n",
			 icon, modifier?modifier:"", embedded_text?"<tl>":"", nominal_size);
		*/
		
		cached_icon = create_normal_cache_icon (icon,
							modifier,
							nominal_size,
							force_nominal);
		/* Try to fallback without modifier */
		if (cached_icon == NULL && modifier != NULL) {
			cached_icon = create_normal_cache_icon (icon,
								NULL,
								nominal_size,
								force_nominal);
		}
		
		if (cached_icon == NULL) {
			cached_icon = factory->fallback_icon;
			cache_icon_ref (cached_icon);
		}
		
		/* Create the key and icon for the hash table. */
		key = g_new (CacheKey, 1);
		key->name = g_strdup (icon);
		key->modifier = g_strdup (modifier);
		key->nominal_size = nominal_size;
		key->force_nominal = force_nominal;

		g_hash_table_insert (hash_table, key, cached_icon);
	}

	/* Hand back a ref to the caller. */
	cache_icon_ref (cached_icon);

	/* Since this item was used, keep it in the cache longer. */
	mark_recently_used (&cached_icon->recently_used_node);
	cached_icon->age = 0;
	
	/* Come back later and sweep the cache. */
	nautilus_icon_factory_schedule_sweep (factory);
	
        return cached_icon;
}

GdkPixbuf *
nautilus_icon_factory_get_pixbuf_for_icon (const char                  *icon,
					   const char                  *modifier,
					   guint                        nominal_size,
					   NautilusEmblemAttachPoints  *attach_points,
					   GdkRectangle                *embedded_text_rect,
					   gboolean                     force_size,
					   gboolean                     wants_default,
					   char                       **display_name)
{
	NautilusIconFactory *factory;
	CacheIcon *cached_icon;
	GdkPixbuf *pixbuf;
	int i;
	
	factory = get_icon_factory ();
	cached_icon = get_icon_from_cache (icon,
					   modifier,
					   nominal_size,
					   force_size);

	if (attach_points != NULL) {
		if (cached_icon->attach_points != NULL) {
			attach_points->num_points = MIN (cached_icon->n_attach_points,
							 MAX_ATTACH_POINTS);
			for (i = 0; i < attach_points->num_points; i++) {
				attach_points->points[i].x = cached_icon->attach_points[i].x;
				attach_points->points[i].y = cached_icon->attach_points[i].y;
			}
		} else {
			attach_points->num_points = 0;
		}
	}
	if (embedded_text_rect) {
		if (cached_icon->embedded_text_rect != NULL) {
			*embedded_text_rect = *cached_icon->embedded_text_rect;
		} else {
			embedded_text_rect->x = 0;
			embedded_text_rect->y = 0;
			embedded_text_rect->width = 0;
			embedded_text_rect->height = 0;
		}
	}

	if (display_name != NULL) {
		*display_name = g_strdup (cached_icon->display_name);
	}
	
	/* if we don't want a default icon and one is returned, return NULL instead */
	if (!wants_default && cached_icon == factory->fallback_icon) {
		cache_icon_unref (cached_icon);
		return NULL;
	}
	
	pixbuf = cached_icon->pixbuf;
	g_object_ref (pixbuf);
	cache_icon_unref (cached_icon);

	return pixbuf;
}


GdkPixbuf *
nautilus_icon_factory_get_pixbuf_for_icon_with_stock_size (const char                  *icon,
							   const char                  *modifier,
							   GtkIconSize                  stock_size,
							   NautilusEmblemAttachPoints  *attach_points,
							   GdkRectangle                *embedded_text_rect,
							   gboolean                     wants_default,
							   char                       **display_name)
{
	return nautilus_icon_factory_get_pixbuf_for_icon (icon, modifier,
							  gtk_icon_size_to_nominal_size (stock_size),
							  attach_points, embedded_text_rect,
							  TRUE /* force_size*/, wants_default,
							  display_name);
}

static guint
cache_key_hash (gconstpointer p)
{
	const CacheKey *key;
	guint hash;

	key = p;

	hash =  g_str_hash (key->name) ^
		((key->nominal_size << 4) + (gint)key->force_nominal);
	
	if (key->modifier) {
		hash ^= g_str_hash (key->modifier);
	}

	return hash;
}

static gboolean
cache_key_equal (gconstpointer a, gconstpointer b)
{
	const CacheKey *key_a, *key_b;

	key_a = a;
	key_b = b;

	return eel_strcmp (key_a->name, key_b->name) == 0 &&
		key_a->nominal_size ==  key_b->nominal_size &&
		key_a->force_nominal == key_b->force_nominal &&
		eel_strcmp (key_a->modifier, key_b->modifier) == 0;
}

/* Return nominal icon size for given zoom level.
 * @zoom_level: zoom level for which to find matching icon size.
 * 
 * Return value: icon size between NAUTILUS_ICON_SIZE_SMALLEST and
 * NAUTILUS_ICON_SIZE_LARGEST, inclusive.
 */
guint
nautilus_get_icon_size_for_zoom_level (NautilusZoomLevel zoom_level)
{
	switch (zoom_level) {
	case NAUTILUS_ZOOM_LEVEL_SMALLEST:
		return NAUTILUS_ICON_SIZE_SMALLEST;
	case NAUTILUS_ZOOM_LEVEL_SMALLER:
		return NAUTILUS_ICON_SIZE_SMALLER;
	case NAUTILUS_ZOOM_LEVEL_SMALL:
		return NAUTILUS_ICON_SIZE_SMALL;
	case NAUTILUS_ZOOM_LEVEL_STANDARD:
		return NAUTILUS_ICON_SIZE_STANDARD;
	case NAUTILUS_ZOOM_LEVEL_LARGE:
		return NAUTILUS_ICON_SIZE_LARGE;
	case NAUTILUS_ZOOM_LEVEL_LARGER:
		return NAUTILUS_ICON_SIZE_LARGER;
	case NAUTILUS_ZOOM_LEVEL_LARGEST:
		return NAUTILUS_ICON_SIZE_LARGEST;
	}
	g_return_val_if_reached (NAUTILUS_ICON_SIZE_STANDARD);
}

float
nautilus_get_relative_icon_size_for_zoom_level (NautilusZoomLevel zoom_level)
{
	return (float)nautilus_get_icon_size_for_zoom_level (zoom_level) / NAUTILUS_ICON_SIZE_STANDARD;
}

/* Convenience cover for nautilus_icon_factory_get_icon_for_file
 * and nautilus_icon_factory_get_pixbuf_for_icon.
 *
 * If a file has an associated thumbnail, the thumb is loaded asynchronously,
 * a loading thumbnail image is returned
 * and the file will receive a "changed" event once the thumbnail has been loaded.
 *
 * The "file" parameter is only used for thumbnailing,
 * for the file change notification once the actual thumbnail
 * has been loaded.
 */
GdkPixbuf *
nautilus_icon_factory_get_pixbuf_for_file_with_icon (NautilusFile                *file,
						     const char                  *icon,
						     const char                  *modifier,
						     guint                        size_in_pixels,
						     NautilusEmblemAttachPoints  *attach_points,
						     GdkRectangle                *embedded_text_rect,
						     gboolean                     force_size,
						     gboolean                     wants_default,
						     char                       **display_name)
{
	GdkPixbuf *pixbuf;
	NautilusIconFactory *factory;
	gboolean is_thumbnail;

	factory = get_icon_factory ();

	is_thumbnail = strstr (icon, "/.thumbnails/") != NULL;

	if (is_thumbnail &&
	    !lookup_icon_from_cache (icon, modifier, size_in_pixels, force_size)) {
		AsnycThumbnailLoadFuncData *data;

		/* Asynchronous thumbnail loading.
 		 * 
		 * This heavily improves performance for folders containing lots of
		 * previously thumbnailed files.
		 *
		 * Note: We do not pass the additional thumbnail parameters (attach points etc.)
		 * to the thread as we don't need them for the cache. The API user may herself
		 * re-request the loaded thumbnail with the correct parameters, which will be set
		 * accordingly in nautilus_icon_factory_get_pixbuf_for_icon() on cache hit
		 * once it is filled.
		 */

		data = g_new (AsnycThumbnailLoadFuncData, 1);
		data->file = nautilus_file_ref (file);
		data->modifier = g_strdup (modifier);
		data->nominal_size = size_in_pixels;
		data->force_nominal = force_size;

		nautilus_file_set_is_thumbnailing (file, TRUE);

		factory->async_thumbnail_load_handles = g_list_prepend (
			factory->async_thumbnail_load_handles,
			nautilus_thumbnail_load_image_async (icon,
							     0, /* base_size */
							     size_in_pixels,
							     force_size,
							     async_thumbnail_load_func,
							     data));

		icon = ICON_NAME_THUMBNAIL_LOADING;
	}


	pixbuf = nautilus_icon_factory_get_pixbuf_for_icon (icon,
							    modifier, size_in_pixels,
							    attach_points, embedded_text_rect,
							    force_size,
							    wants_default, display_name);

	return pixbuf;
}

/*
 * like nautilus_icon_factory_get_pixbuf_for_file_with_icon() but does the icon lookup itself,
 * doesn't allow emblem and text rect fetching.
 */
GdkPixbuf *
nautilus_icon_factory_get_pixbuf_for_file (NautilusFile *file,
					   const char *modifier,
					   guint size_in_pixels,
					   gboolean force_size)
{
	GdkPixbuf *pixbuf;
	NautilusIconFactory *factory;
	char *icon;

	factory = get_icon_factory ();

	/* Get the pixbuf for this file. */
	icon = nautilus_icon_factory_get_icon_for_file (file, FALSE);
	if (icon == NULL) {
		return NULL;
	}

	pixbuf = nautilus_icon_factory_get_pixbuf_for_file_with_icon (file,
								      icon, modifier,
								      size_in_pixels,
								      NULL, NULL,
								      force_size,
								      TRUE, NULL);
	g_free (icon);

	return pixbuf;
}

GdkPixbuf *
nautilus_icon_factory_get_pixbuf_for_file_with_stock_size (NautilusFile *file,
							   const char   *modifier,
							   GtkIconSize   stock_size)
{
	return nautilus_icon_factory_get_pixbuf_for_file (file, modifier,
							  gtk_icon_size_to_nominal_size (stock_size),
							  TRUE /* force_size */);

}

/* Convenience routine for getting a pixbuf from an icon name. */
GdkPixbuf *
nautilus_icon_factory_get_pixbuf_from_name (const char *icon_name,
					    const char *modifier,
					    guint size_in_pixels,
					    gboolean force_size,
					    char **display_name)
{
	return nautilus_icon_factory_get_pixbuf_for_icon (icon_name, modifier,
							  size_in_pixels,
							  NULL, NULL,
							  force_size, TRUE,
							  display_name);
}

GdkPixbuf *
nautilus_icon_factory_get_pixbuf_from_name_with_stock_size (const char *icon_name,
							    const char *modifier,
							    GtkIconSize stock_size,
							    char **display_name)
{
	return nautilus_icon_factory_get_pixbuf_from_name (icon_name, modifier,
							   gtk_icon_size_to_nominal_size (stock_size),
							   TRUE, display_name);
}


GdkPixbuf *
nautilus_icon_factory_get_thumbnail_frame (void)
{
	return get_icon_factory ()->thumbnail_frame;
}

gboolean
nautilus_icon_factory_remove_from_cache (const char *icon_name,
					 const char *modifier,
					 guint size)
{
	GHashTable *hash_table;
	NautilusIconFactory *factory;
	CacheKey lookup_key;

	factory = get_icon_factory ();
	hash_table = factory->icon_cache;

	/* Check to see if it's already in the table. */
	lookup_key.name = (char *)icon_name;
	lookup_key.modifier = (char *)modifier;
	lookup_key.nominal_size = size;
	
	return g_hash_table_remove (hash_table, &lookup_key);
}
					 
#if ! defined (NAUTILUS_OMIT_SELF_CHECK)

void
nautilus_self_check_icon_factory (void)
{
	EEL_CHECK_INTEGER_RESULT (nautilus_get_icon_size_for_zoom_level (0), 16);
	EEL_CHECK_INTEGER_RESULT (nautilus_get_icon_size_for_zoom_level (1), 24);
	EEL_CHECK_INTEGER_RESULT (nautilus_get_icon_size_for_zoom_level (2), 32);
	EEL_CHECK_INTEGER_RESULT (nautilus_get_icon_size_for_zoom_level (3), 48);
	EEL_CHECK_INTEGER_RESULT (nautilus_get_icon_size_for_zoom_level (4), 72);
	EEL_CHECK_INTEGER_RESULT (nautilus_get_icon_size_for_zoom_level (5), 96);
	EEL_CHECK_INTEGER_RESULT (nautilus_get_icon_size_for_zoom_level (6), 192);

	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (0), 16);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (1), 16);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (15), 16);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (16), 24);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (23), 24);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (24), 32);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (31), 32);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (32), 48);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (47), 48);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (48), 72);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (71), 72);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (72), 96);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (95), 96);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (96), 192);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (191), 192);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (192), 192);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_larger_icon_size (0xFFFFFFFF), 192);

	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (0), 16);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (1), 16);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (11), 16);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (12), 16);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (24), 16);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (25), 24);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (32), 24);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (33), 32);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (48), 32);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (49), 48);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (72), 48);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (73), 72);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (96), 72);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (97), 96);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (192), 96);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (193), 192);
	EEL_CHECK_INTEGER_RESULT (nautilus_icon_factory_get_smaller_icon_size (0xFFFFFFFF), 192);
}

#endif /* ! NAUTILUS_OMIT_SELF_CHECK */