/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  Nautilus
 *
 *  Copyright (C) 1999, 2000 Red Hat, Inc.
 *  Copyright (C) 1999, 2000, 2001 Eazel, Inc.
 *
 *  Nautilus is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  Nautilus is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Authors: Elliot Lee <sopwith@redhat.com>
 *           John Sullivan <sullivan@eazel.com>
 *           Darin Adler <darin@bentspoon.com>
 */

#include <config.h>
#include "nautilus-window-manage-views.h"

#include "nautilus-actions.h"
#include "nautilus-application.h"
#include "nautilus-location-bar.h"
#include "nautilus-search-bar.h"
#include "nautilus-pathbar.h"
#include "nautilus-main.h"
#include "nautilus-window-private.h"
#include "nautilus-trash-bar.h"
#include "nautilus-zoom-control.h"
#include <eel/eel-accessibility.h>
#include <eel/eel-debug.h>
#include <eel/eel-gdk-extensions.h>
#include <eel/eel-glib-extensions.h>
#include <eel/eel-gtk-extensions.h>
#include <eel/eel-stock-dialogs.h>
#include <eel/eel-string.h>
#include <eel/eel-vfs-extensions.h>
#include <gtk/gtkmain.h>
#include <gtk/gtksignal.h>
#include <gdk/gdkx.h>
#include <glib/gi18n.h>
#include <libgnomeui/gnome-dialog-util.h>
#include <libgnomeui/gnome-icon-theme.h>
#include <libgnomevfs/gnome-vfs-async-ops.h>
#include <libgnomevfs/gnome-vfs-uri.h>
#include <libgnomevfs/gnome-vfs-utils.h>
#include <libnautilus-extension/nautilus-location-widget-provider.h>
#include <libnautilus-private/nautilus-debug-log.h>
#include <libnautilus-private/nautilus-file-attributes.h>
#include <libnautilus-private/nautilus-file-utilities.h>
#include <libnautilus-private/nautilus-file.h>
#include <libnautilus-private/nautilus-global-preferences.h>
#include <libnautilus-private/nautilus-metadata.h>
#include <libnautilus-private/nautilus-mime-actions.h>
#include <libnautilus-private/nautilus-module.h>
#include <libnautilus-private/nautilus-monitor.h>
#include <libnautilus-private/nautilus-search-directory.h>
#include <libnautilus-private/nautilus-trash-directory.h>
#include <libnautilus-private/nautilus-view-factory.h>
#include <libnautilus-private/nautilus-window-info.h>

/* FIXME bugzilla.gnome.org 41243: 
 * We should use inheritance instead of these special cases
 * for the desktop window.
 */
#include "nautilus-desktop-window.h"

/* This number controls a maximum character count for a URL that is
 * displayed as part of a dialog. It's fairly arbitrary -- big enough
 * to allow most "normal" URIs to display in full, but small enough to
 * prevent the dialog from getting insanely wide.
 */
#define MAX_URI_IN_DIALOG_LENGTH 60

static void connect_view                              (NautilusWindow             *window,
						       NautilusView               *view);
static void disconnect_view                           (NautilusWindow             *window,
						       NautilusView               *view);
static void begin_location_change                     (NautilusWindow             *window,
						       const char                 *location,
						       GList                      *new_selection,
						       NautilusLocationChangeType  type,
						       guint                       distance,
						       const char                 *scroll_pos);
static void free_location_change                      (NautilusWindow             *window);
static void end_location_change                       (NautilusWindow             *window);
static void cancel_location_change                    (NautilusWindow             *window);
static void got_file_info_for_view_selection_callback (NautilusFile               *file,
						       gpointer                    callback_data);
static void create_content_view                       (NautilusWindow             *window,
						       const char                 *view_id);
static void display_view_selection_failure            (NautilusWindow             *window,
						       NautilusFile               *file,
						       const char                 *location);
static void load_new_location                         (NautilusWindow             *window,
						       const char                 *location,
						       GList                      *selection,
						       gboolean                    tell_current_content_view,
						       gboolean                    tell_new_content_view);
static void location_has_really_changed               (NautilusWindow             *window);
static void update_for_new_location                   (NautilusWindow             *window);
static void zoom_parameters_changed_callback          (NautilusView               *view,
						       NautilusWindow             *window);
static void update_extra_location_widgets_visibility  (NautilusWindow             *window);
static void remove_extra_location_widgets             (NautilusWindow             *window);

void
nautilus_window_report_selection_changed (NautilusWindowInfo *window)
{
	g_signal_emit_by_name (window, "selection_changed");
}

/* set_displayed_location:
 */
static void
set_displayed_location (NautilusWindow *window, const char *location)
{
        char *bookmark_uri;
        gboolean recreate;
        
        if (window->current_location_bookmark == NULL || location == NULL) {
                recreate = TRUE;
        } else {
                bookmark_uri = nautilus_bookmark_get_uri (window->current_location_bookmark);
                recreate = !gnome_vfs_uris_match (bookmark_uri, location);
                g_free (bookmark_uri);
        }
        
        if (recreate) {
                /* We've changed locations, must recreate bookmark for current location. */
                if (window->last_location_bookmark != NULL)  {
                        g_object_unref (window->last_location_bookmark);
                }
                window->last_location_bookmark = window->current_location_bookmark;
                window->current_location_bookmark = (location == NULL) ? NULL
                        : nautilus_bookmark_new (location, location);
        }
        nautilus_window_update_title (window);
	nautilus_window_update_icon (window);
}

static void
check_bookmark_location_matches (NautilusBookmark *bookmark, const char *uri)
{
	char *bookmark_uri;

	bookmark_uri = nautilus_bookmark_get_uri (bookmark);
	if (!gnome_vfs_uris_match (uri, bookmark_uri)) {
		g_warning ("bookmark uri is %s, but expected %s", bookmark_uri, uri);
	}
	g_free (bookmark_uri);
}

/* Debugging function used to verify that the last_location_bookmark
 * is in the state we expect when we're about to use it to update the
 * Back or Forward list.
 */
static void
check_last_bookmark_location_matches_window (NautilusWindow *window)
{
	check_bookmark_location_matches (window->last_location_bookmark,
                                         window->details->location);
}

static void
handle_go_back (NautilusNavigationWindow *window, const char *location)
{
        guint i;
        GList *link;
        NautilusBookmark *bookmark;

        g_return_if_fail (NAUTILUS_IS_NAVIGATION_WINDOW (window));

        /* Going back. Move items from the back list to the forward list. */
        g_assert (g_list_length (window->back_list) > NAUTILUS_WINDOW (window)->details->location_change_distance);
        check_bookmark_location_matches (NAUTILUS_BOOKMARK (g_list_nth_data (window->back_list,
                                                                             NAUTILUS_WINDOW (window)->details->location_change_distance)),
                                         location);
        g_assert (NAUTILUS_WINDOW (window)->details->location != NULL);
        
        /* Move current location to Forward list */

        check_last_bookmark_location_matches_window (NAUTILUS_WINDOW (window));

        /* Use the first bookmark in the history list rather than creating a new one. */
        window->forward_list = g_list_prepend (window->forward_list,
                                               NAUTILUS_WINDOW (window)->last_location_bookmark);
        g_object_ref (window->forward_list->data);
                                
        /* Move extra links from Back to Forward list */
        for (i = 0; i < NAUTILUS_WINDOW (window)->details->location_change_distance; ++i) {
        	bookmark = NAUTILUS_BOOKMARK (window->back_list->data);
                window->back_list = g_list_remove (window->back_list, bookmark);
                window->forward_list = g_list_prepend (window->forward_list, bookmark);
        }
        
        /* One bookmark falls out of back/forward lists and becomes viewed location */
        link = window->back_list;
        window->back_list = g_list_remove_link (window->back_list, link);
        g_object_unref (link->data);
        g_list_free_1 (link);
}

static void
handle_go_forward (NautilusNavigationWindow *window, const char *location)
{
        guint i;
        GList *link;
        NautilusBookmark *bookmark;

        g_return_if_fail (NAUTILUS_IS_NAVIGATION_WINDOW (window));

        /* Going forward. Move items from the forward list to the back list. */
        g_assert (g_list_length (window->forward_list) > NAUTILUS_WINDOW (window)->details->location_change_distance);
        check_bookmark_location_matches (NAUTILUS_BOOKMARK (g_list_nth_data (window->forward_list,
                                                                             NAUTILUS_WINDOW (window)->details->location_change_distance)),
                                         location);
        g_assert (NAUTILUS_WINDOW (window)->details->location != NULL);
                                
        /* Move current location to Back list */

        check_last_bookmark_location_matches_window (NAUTILUS_WINDOW (window));
        
        /* Use the first bookmark in the history list rather than creating a new one. */
        window->back_list = g_list_prepend (window->back_list,
                                            NAUTILUS_WINDOW (window)->last_location_bookmark);
        g_object_ref (window->back_list->data);
        
        /* Move extra links from Forward to Back list */
        for (i = 0; i < NAUTILUS_WINDOW (window)->details->location_change_distance; ++i) {
        	bookmark = NAUTILUS_BOOKMARK (window->forward_list->data);
                window->forward_list = g_list_remove (window->forward_list, bookmark);
                window->back_list = g_list_prepend (window->back_list, bookmark);
        }
        
        /* One bookmark falls out of back/forward lists and becomes viewed location */
        link = window->forward_list;
        window->forward_list = g_list_remove_link (window->forward_list, link);
        g_object_unref (link->data);
        g_list_free_1 (link);
}

static void
handle_go_elsewhere (NautilusWindow *window, const char *location)
{
#if !NEW_UI_COMPLETE
        if (NAUTILUS_IS_NAVIGATION_WINDOW (window)) {        
                /* Clobber the entire forward list, and move displayed location to back list */
                nautilus_navigation_window_clear_forward_list (NAUTILUS_NAVIGATION_WINDOW (window));
                
                if (window->details->location != NULL) {
                        /* If we're returning to the same uri somehow, don't put this uri on back list. 
                         * This also avoids a problem where set_displayed_location
                         * didn't update last_location_bookmark since the uri didn't change.
                         */
                        if (!gnome_vfs_uris_match (window->details->location, location)) {
                                /* Store bookmark for current location in back list, unless there is no current location */
                                check_last_bookmark_location_matches_window (window);
                                /* Use the first bookmark in the history list rather than creating a new one. */
                                NAUTILUS_NAVIGATION_WINDOW (window)->back_list = g_list_prepend (NAUTILUS_NAVIGATION_WINDOW (window)->back_list,
                                                                                                 window->last_location_bookmark);
                                g_object_ref (NAUTILUS_NAVIGATION_WINDOW (window)->back_list->data);
                        }
                }       
        }
#endif
}

static void
update_up_button (NautilusWindow *window)
{
        gboolean allowed;
        GnomeVFSURI *new_uri;

        allowed = FALSE;
        if (window->details->location != NULL) {
                new_uri = gnome_vfs_uri_new (window->details->location);
                if (new_uri != NULL) {
                        allowed = gnome_vfs_uri_has_parent (new_uri);
                        gnome_vfs_uri_unref (new_uri);
                }
        }

        nautilus_window_allow_up (window, allowed);
}

static void
viewed_file_changed_callback (NautilusFile *file,
                              NautilusWindow *window)
{
        char *new_location;
	gboolean is_in_trash, was_in_trash;

        g_assert (NAUTILUS_IS_FILE (file));
	g_assert (NAUTILUS_IS_WINDOW (window));
	g_assert (window->details->viewed_file == file);

        if (!nautilus_file_is_not_yet_confirmed (file)) {
                window->details->viewed_file_seen = TRUE;
        }

	was_in_trash = window->details->viewed_file_in_trash;

	window->details->viewed_file_in_trash = is_in_trash = nautilus_file_is_in_trash (file);

	/* Close window if the file it's viewing has been deleted or moved to trash. */
	if (nautilus_file_is_gone (file) || (is_in_trash && !was_in_trash)) {
                /* Don't close the window in the case where the
                 * file was never seen in the first place.
                 */
                if (window->details->viewed_file_seen) {
                        /* Detecting a file is gone may happen in the
                         * middle of a pending location change, we
                         * need to cancel it before closing the window
                         * or things break.
                         */
                        /* FIXME: It makes no sense that this call is
                         * needed. When the window is destroyed, it
                         * calls nautilus_window_manage_views_destroy,
                         * which calls free_location_change, which
                         * should be sufficient. Also, if this was
                         * really needed, wouldn't it be needed for
                         * all other nautilus_window_close callers?
                         */
                        end_location_change (window);

			if (NAUTILUS_IS_NAVIGATION_WINDOW (window)) {
				/* auto-show existing parent URI. */
				char *uri, *go_to_uri;
				NautilusFile *parent_file;

				parent_file = nautilus_file_get_parent (file);
				uri = nautilus_file_get_uri (parent_file);

				go_to_uri = nautilus_find_existing_uri_in_hierarchy (uri);
				if (go_to_uri != NULL) {
					/* the path bar URI will be set to go_to_uri immediately
					 * in begin_location_change, but we don't want the
					 * inexistant children to show up anymore */
					nautilus_path_bar_clear_buttons (NAUTILUS_PATH_BAR (NAUTILUS_NAVIGATION_WINDOW (window)->path_bar));
					nautilus_window_go_to (NAUTILUS_WINDOW (window), go_to_uri);
					g_free (go_to_uri);
				} else {
					nautilus_window_go_home (NAUTILUS_WINDOW (window));
				}

				g_free (uri);
				nautilus_file_unref (parent_file);
			} else {
				nautilus_window_close (window);
			}
                }
	} else {
                new_location = nautilus_file_get_uri (file);

                /* FIXME: We need to graft the fragment part of the
                 * old location onto the new renamed location or we'll
                 * lose the fragment part of the location altogether.
                 * If we did that, then we woudn't need to ignore
                 * fragments in this comparison.
                 */
                /* If the file was renamed, update location and/or
                 * title. Ignore fragments in this comparison, because
                 * NautilusFile omits the fragment part.
                 */
                if (!eel_uris_match_ignore_fragments (new_location,
                                                           window->details->location)) {
                        g_free (window->details->location);
                        window->details->location = new_location;
			
                        /* Check if we can go up. */
                        update_up_button (window);
#if !NEW_UI_COMPLETE
                        if (NAUTILUS_IS_NAVIGATION_WINDOW (window)) {
                                /* Change the location bar and path bar to match the current location. */
                                nautilus_navigation_bar_set_location
                                        (NAUTILUS_NAVIGATION_BAR (NAUTILUS_NAVIGATION_WINDOW (window)->navigation_bar),
                                         window->details->location);
				nautilus_path_bar_set_path (NAUTILUS_PATH_BAR (NAUTILUS_NAVIGATION_WINDOW (window)->path_bar),
					    window->details->location);
                        }                  
                        if (NAUTILUS_IS_SPATIAL_WINDOW (window)) {
                                /* Change the location button to match the current location. */
                                nautilus_spatial_window_set_location_button
                                        (NAUTILUS_SPATIAL_WINDOW (window),
                                         window->details->location);
                        }                  
#endif

                } else {
                        g_free (new_location);
                }

                nautilus_window_update_title (window);
		nautilus_window_update_icon (window);
        }
}

static void
update_history (NautilusWindow *window,
                NautilusLocationChangeType type,
                const char *new_location)
{
        switch (type) {
        case NAUTILUS_LOCATION_CHANGE_STANDARD:
        case NAUTILUS_LOCATION_CHANGE_FALLBACK:
                nautilus_window_add_current_location_to_history_list (window);
                handle_go_elsewhere (window, new_location);
                return;
        case NAUTILUS_LOCATION_CHANGE_RELOAD:
                /* for reload there is no work to do */
                return;
        case NAUTILUS_LOCATION_CHANGE_BACK:
                nautilus_window_add_current_location_to_history_list (window);
                handle_go_back (NAUTILUS_NAVIGATION_WINDOW (window), 
                                new_location);
                return;
        case NAUTILUS_LOCATION_CHANGE_FORWARD:
                nautilus_window_add_current_location_to_history_list (window);
                handle_go_forward (NAUTILUS_NAVIGATION_WINDOW (window), 
                                   new_location);
                return;
        case NAUTILUS_LOCATION_CHANGE_REDIRECT:
                /* for the redirect case, the caller can do the updating */
                return;
        }
	g_return_if_fail (FALSE);
}

static void
cancel_viewed_file_changed_callback (NautilusWindow *window)
{
        NautilusFile *file;

        file = window->details->viewed_file;
        if (file != NULL) {
                g_signal_handlers_disconnect_by_func (G_OBJECT (file),
                                                      G_CALLBACK (viewed_file_changed_callback),
                                                      window);
                nautilus_file_monitor_remove (file, &window->details->viewed_file);
        }
}

static void
new_window_show_callback (GtkWidget *widget,
                          gpointer user_data)
{
        NautilusWindow *window;
        
        window = NAUTILUS_WINDOW (user_data);
        
        nautilus_window_close (window);

        g_signal_handlers_disconnect_by_func (widget, 
                                              G_CALLBACK (new_window_show_callback),
                                              user_data);
}


void
nautilus_window_open_location_full (NautilusWindow *window,
				    const char *location,
				    NautilusWindowOpenMode mode,
				    NautilusWindowOpenFlags flags,
				    GList *new_selection)
{
        NautilusWindow *target_window;
        gboolean do_load_location = TRUE;
	char *old_location;
        
        target_window = NULL;

	old_location = nautilus_window_get_location (window);
	nautilus_debug_log (FALSE, NAUTILUS_DEBUG_LOG_DOMAIN_USER,
			    "window %p open location: old=\"%s\", new=\"%s\"",
			    window,
			    old_location ? old_location : "(none)",
			    location);

	switch (mode) {
        case NAUTILUS_WINDOW_OPEN_ACCORDING_TO_MODE :
		if (eel_preferences_get_boolean (NAUTILUS_PREFERENCES_ALWAYS_USE_BROWSER)) {
			target_window = window;
			if (NAUTILUS_IS_SPATIAL_WINDOW (window)) {
				if (!NAUTILUS_SPATIAL_WINDOW (window)->affect_spatial_window_on_next_location_change) {
					target_window = nautilus_application_create_navigation_window 
						(window->application,
						 NULL,
						 gtk_window_get_screen (GTK_WINDOW (window)));
				} else {
					NAUTILUS_SPATIAL_WINDOW (window)->affect_spatial_window_on_next_location_change = FALSE;
				}
			} else if ((flags & NAUTILUS_WINDOW_OPEN_FLAG_NEW_WINDOW) != 0) {
				target_window = nautilus_application_create_navigation_window 
					(window->application,
					 NULL,
					 gtk_window_get_screen (GTK_WINDOW (window)));
			}
		} else if (NAUTILUS_IS_SPATIAL_WINDOW (window)) {
                        if (!NAUTILUS_SPATIAL_WINDOW (window)->affect_spatial_window_on_next_location_change) {
                                target_window = nautilus_application_present_spatial_window_with_selection (
                                        window->application,
					window,
					NULL,
                                        location,
					new_selection,
                                        gtk_window_get_screen (GTK_WINDOW (window)));
                                do_load_location = FALSE;
                        } else {
                                NAUTILUS_SPATIAL_WINDOW (window)->affect_spatial_window_on_next_location_change = FALSE;
                                target_window = window;
                        }
		} else if (flags & NAUTILUS_WINDOW_OPEN_FLAG_NEW_WINDOW) {
			target_window = nautilus_application_create_navigation_window 
				(window->application,
				 NULL,
				 gtk_window_get_screen (GTK_WINDOW (window)));
		} else {
                        target_window = window;
                }       
                break;
        case NAUTILUS_WINDOW_OPEN_IN_SPATIAL :
                target_window = nautilus_application_present_spatial_window (
                        window->application,
			window,
			NULL,
                        location,
                        gtk_window_get_screen (GTK_WINDOW (window)));
                break;
        case NAUTILUS_WINDOW_OPEN_IN_NAVIGATION :
                target_window = nautilus_application_create_navigation_window 
                        (window->application,
			 NULL,
                         gtk_window_get_screen (GTK_WINDOW (window)));
                break;
        default :
                g_warning ("Unknown open location mode");
		g_free (old_location);
                return;
        }

        g_assert (target_window != NULL);

        if ((flags & NAUTILUS_WINDOW_OPEN_FLAG_CLOSE_BEHIND) != 0) {
                if (NAUTILUS_IS_SPATIAL_WINDOW (window) && !NAUTILUS_IS_DESKTOP_WINDOW (window)) {
                        if (GTK_WIDGET_VISIBLE (target_window)) {
                                nautilus_window_close (window);
                        } else {
                                g_signal_connect_object (target_window,
                                                         "show",
                                                         G_CALLBACK (new_window_show_callback),
                                                         window,
                                                         G_CONNECT_AFTER);
                        }
                }
        }

        if ((!do_load_location) ||
	    (target_window == window && !eel_strcmp (old_location, location))) {
		g_free (old_location);
                return;
        }
	g_free (old_location);

        if (!eel_is_valid_uri (location))
                g_warning ("Possibly invalid new URI '%s'\n"
                           "This can cause subtle evils like #48423", location);

        begin_location_change (target_window, location, new_selection,
                               NAUTILUS_LOCATION_CHANGE_STANDARD, 0, NULL);
}

void
nautilus_window_open_location (NautilusWindow *window,
                               const char *location,
                               gboolean close_behind)
{
	NautilusWindowOpenFlags flags;

	flags = 0;
	if (close_behind) {
		flags = NAUTILUS_WINDOW_OPEN_FLAG_CLOSE_BEHIND;
	}
	
	nautilus_window_open_location_full (window, location,
					    NAUTILUS_WINDOW_OPEN_ACCORDING_TO_MODE,
					    flags, NULL);
}

void
nautilus_window_open_location_with_selection (NautilusWindow *window,
					      const char *location,
					      GList *selection,
					      gboolean close_behind)
{
	NautilusWindowOpenFlags flags;

	flags = 0;
	if (close_behind) {
		flags = NAUTILUS_WINDOW_OPEN_FLAG_CLOSE_BEHIND;
	}
	nautilus_window_open_location_full (window, location, 
					    NAUTILUS_WINDOW_OPEN_ACCORDING_TO_MODE,
					    flags, selection);
}					      

char *
nautilus_window_get_view_label (NautilusWindow *window)
{
	const NautilusViewInfo *info;

	info = nautilus_view_factory_lookup (nautilus_window_get_content_view_id (window));

	return g_strdup (info->label);
}

char *
nautilus_window_get_view_error_label (NautilusWindow *window)
{
	const NautilusViewInfo *info;

	info = nautilus_view_factory_lookup (nautilus_window_get_content_view_id (window));

	return g_strdup (info->error_label);
}

char *
nautilus_window_get_view_startup_error_label (NautilusWindow *window)
{
	const NautilusViewInfo *info;

	info = nautilus_view_factory_lookup (nautilus_window_get_content_view_id (window));

	return g_strdup (info->startup_error_label);
}

static void
report_current_content_view_failure_to_user (NautilusWindow *window,
                                     	     NautilusView *view)
{
	char *message;

	message = nautilus_window_get_view_startup_error_label (window);
	eel_show_error_dialog (message,
			       _("You can choose another view or go to a different location."),
			       GTK_WINDOW (window));
	g_free (message);
}

static void
report_nascent_content_view_failure_to_user (NautilusWindow *window,
                                     	     NautilusView *view)
{
	char *message;

	message = nautilus_window_get_view_error_label (window);
	eel_show_error_dialog (message,
			       _("The location cannot be displayed with this viewer."),
			       GTK_WINDOW (window));
	g_free (message);
}


const char *
nautilus_window_get_content_view_id (NautilusWindow *window)
{
        if (window->content_view == NULL) {
                return NULL;
        }
	return nautilus_view_get_view_id (window->content_view);
}

gboolean
nautilus_window_content_view_matches_iid (NautilusWindow *window, 
					  const char *iid)
{
        if (window->content_view == NULL) {
                return FALSE;
        }
	return eel_strcmp (nautilus_view_get_view_id (window->content_view),
                           iid) == 0;
}


/*
 * begin_location_change
 * 
 * Change a window's location.
 * @window: The NautilusWindow whose location should be changed.
 * @location: A url specifying the location to load
 * @new_selection: The initial selection to present after loading the location
 * @type: Which type of location change is this? Standard, back, forward, or reload?
 * @distance: If type is back or forward, the index into the back or forward chain. If
 * type is standard or reload, this is ignored, and must be 0.
 * @scroll_pos: The file to scroll to when the location is loaded.
 *
 * This is the core function for changing the location of a window. Every change to the
 * location begins here.
 */
static void
begin_location_change (NautilusWindow *window,
                       const char *location,
		       GList *new_selection,
                       NautilusLocationChangeType type,
                       guint distance,
                       const char *scroll_pos)
{
        NautilusDirectory *directory;
        NautilusFile *file;
	gboolean force_reload;
        char *current_pos;

        g_assert (NAUTILUS_IS_WINDOW (window));
        g_assert (location != NULL);
        g_assert (type == NAUTILUS_LOCATION_CHANGE_BACK
                  || type == NAUTILUS_LOCATION_CHANGE_FORWARD
                  || distance == 0);

        g_object_ref (window);

        end_location_change (window);
        
        nautilus_window_allow_stop (window, TRUE);
        nautilus_window_set_status (window, " ");

	g_assert (window->details->pending_location == NULL);
	g_assert (window->details->pending_selection == NULL);
	
        window->details->pending_location = g_strdup (location);
        window->details->location_change_type = type;
        window->details->location_change_distance = distance;
        window->details->pending_selection = eel_g_str_list_copy (new_selection);

        
        window->details->pending_scroll_to = g_strdup (scroll_pos);
        
        directory = nautilus_directory_get (location);

	/* The code to force a reload is here because if we do it
	 * after determining an initial view (in the components), then
	 * we end up fetching things twice.
	 */
	if (type == NAUTILUS_LOCATION_CHANGE_RELOAD) {
		force_reload = TRUE;
	} else if (!nautilus_monitor_active ()) {
		force_reload = TRUE;
	} else {
		force_reload = !nautilus_directory_is_local (directory);
	}

	if (force_reload) {
		nautilus_directory_force_reload (directory);
		file = nautilus_directory_get_corresponding_file (directory);
		nautilus_file_invalidate_all_attributes (file);
		nautilus_file_unref (file);
	}

        nautilus_directory_unref (directory);

        /* Set current_bookmark scroll pos */
        if (window->current_location_bookmark != NULL &&
            window->content_view != NULL) {
                current_pos = nautilus_view_get_first_visible_file (window->content_view);
                nautilus_bookmark_set_scroll_pos (window->current_location_bookmark, current_pos);
                g_free (current_pos);
        }

	/* Get the info needed for view selection */
	
        window->details->determine_view_file = nautilus_file_get (location);

	g_assert (window->details->determine_view_file != NULL);

	/* if the currently viewed file is marked gone while loading the new location,
	 * this ensures that the window isn't destroyed */
        cancel_viewed_file_changed_callback (window);

	nautilus_file_call_when_ready (window->details->determine_view_file,
				       NAUTILUS_FILE_ATTRIBUTE_IS_DIRECTORY |
				       NAUTILUS_FILE_ATTRIBUTE_MIME_TYPE |
				       NAUTILUS_FILE_ATTRIBUTE_METADATA,
                                       got_file_info_for_view_selection_callback,
				       window);

        g_object_unref (window);
}

static void
setup_new_window (NautilusWindow *window, NautilusFile *file)
{
	char *show_hidden_file_setting;
	char *geometry_string;
	char *scroll_string;
	gboolean maximized, sticky, above;
	
	if (NAUTILUS_IS_SPATIAL_WINDOW (window) && !NAUTILUS_IS_DESKTOP_WINDOW (window)) {
		/* load show hidden state */
		show_hidden_file_setting = nautilus_file_get_metadata 
			(file, NAUTILUS_METADATA_KEY_WINDOW_SHOW_HIDDEN_FILES,
			 NULL);
		if (show_hidden_file_setting != NULL) {
			if (strcmp (show_hidden_file_setting, "1") == 0) {
				NAUTILUS_WINDOW (window)->details->show_hidden_files_mode = NAUTILUS_WINDOW_SHOW_HIDDEN_FILES_ENABLE;	
			} else {
				NAUTILUS_WINDOW (window)->details->show_hidden_files_mode = NAUTILUS_WINDOW_SHOW_HIDDEN_FILES_DISABLE;
			}
		} else {
			NAUTILUS_WINDOW (window)->details->show_hidden_files_mode = NAUTILUS_WINDOW_SHOW_HIDDEN_FILES_DEFAULT;
		}
		g_free (show_hidden_file_setting);
		
		/* load the saved window geometry */
		maximized = nautilus_file_get_boolean_metadata
			(file, NAUTILUS_METADATA_KEY_WINDOW_MAXIMIZED, FALSE);
		if (maximized) {
			gtk_window_maximize (GTK_WINDOW (window));
		} else {
			gtk_window_unmaximize (GTK_WINDOW (window));
		}

		sticky = nautilus_file_get_boolean_metadata
			(file, NAUTILUS_METADATA_KEY_WINDOW_STICKY, FALSE);
		if (sticky) {
			gtk_window_stick (GTK_WINDOW (window));
		} else {
			gtk_window_unstick (GTK_WINDOW (window));
		}

		above = nautilus_file_get_boolean_metadata
			(file, NAUTILUS_METADATA_KEY_WINDOW_KEEP_ABOVE, FALSE);
		if (above) {
			gtk_window_set_keep_above (GTK_WINDOW (window), TRUE);
		} else {
			gtk_window_set_keep_above (GTK_WINDOW (window), FALSE);
		}

		geometry_string = nautilus_file_get_metadata 
			(file, NAUTILUS_METADATA_KEY_WINDOW_GEOMETRY, NULL);
                if (geometry_string != NULL) {
                        eel_gtk_window_set_initial_geometry_from_string 
                                (GTK_WINDOW (window), 
                                 geometry_string,
                                 NAUTILUS_SPATIAL_WINDOW_MIN_WIDTH, 
                                 NAUTILUS_SPATIAL_WINDOW_MIN_HEIGHT,
				 FALSE);
                }
                g_free (geometry_string);

		if (window->details->pending_selection == NULL) {
			/* If there is no pending selection, then load the saved scroll position. */
			scroll_string = nautilus_file_get_metadata 
				(file, NAUTILUS_METADATA_KEY_WINDOW_SCROLL_POSITION,
				 NULL);
		} else {
			/* If there is a pending selection, we want to scroll to an item in
			 * the pending selection list. */
			scroll_string = g_strdup (window->details->pending_selection->data);
		}

		/* scroll_string might be NULL if there was no saved scroll position. */
		if (scroll_string != NULL) {
			window->details->pending_scroll_to = scroll_string;
		}
        }
}

static void
got_file_info_for_view_selection_callback (NautilusFile *file,
					   gpointer callback_data)
{
        GnomeVFSResult vfs_result_code;
	char *view_id;
	char *mimetype;
	NautilusWindow *window;
	NautilusFile *viewed_file;
	char *location;
	char *home_uri;

	window = callback_data;
	
        g_assert (window->details->determine_view_file == file);
        window->details->determine_view_file = NULL;

	location = window->details->pending_location;
	
	view_id = NULL;
	
	vfs_result_code = nautilus_file_get_file_info_result (file);
        if (vfs_result_code == GNOME_VFS_OK
            || vfs_result_code == GNOME_VFS_ERROR_NOT_SUPPORTED
            || vfs_result_code == GNOME_VFS_ERROR_INVALID_URI) {
		/* We got the information we need, now pick what view to use: */

		mimetype = nautilus_file_get_mime_type (file);

		/* If fallback, don't use view from metadata */
		if (window->details->location_change_type != NAUTILUS_LOCATION_CHANGE_FALLBACK) {
			/* Look in metadata for view */
			view_id = nautilus_file_get_metadata 
				(file, NAUTILUS_METADATA_KEY_DEFAULT_COMPONENT, NULL);
			if (view_id != NULL && 
			    !nautilus_view_factory_view_supports_uri (view_id,
								      location,
								      nautilus_file_get_file_type (file),
								      mimetype)) {
				g_free (view_id);
				view_id = NULL;
			}
		}

		/* Otherwise, use default */
		if (view_id == NULL) {
			view_id = nautilus_global_preferences_get_default_folder_viewer_preference_as_iid ();

			if (view_id != NULL && 
			    !nautilus_view_factory_view_supports_uri (view_id,
								      location,
								      nautilus_file_get_file_type (file),
								      mimetype)) {
				g_free (view_id);
				view_id = NULL;
			}
		}
		
		g_free (mimetype);
	}

	if (view_id != NULL) {
                if (!GTK_WIDGET_VISIBLE (window)) {
			/* We now have the metadata to set up the window position, etc */
			setup_new_window (window, file);
		}
		create_content_view (window, view_id);
		g_free (view_id);
	} else {
		display_view_selection_failure (window, file,
						location);

		if (!GTK_WIDGET_VISIBLE (GTK_WIDGET (window))) {
			/* Destroy never-had-a-chance-to-be-seen window. This case
			 * happens when a new window cannot display its initial URI. 
			 */
			/* if this is the only window, we don't want to quit, so we redirect it to home */
			if (nautilus_application_get_n_windows () <= 1) {
				g_assert (nautilus_application_get_n_windows () == 1);

				/* Make sure we re-use this window */
				if (NAUTILUS_IS_SPATIAL_WINDOW (window)) {
					NAUTILUS_SPATIAL_WINDOW (window)->affect_spatial_window_on_next_location_change = TRUE;
				}
				/* the user could have typed in a home directory that doesn't exist,
				   in which case going home would cause an infinite loop, so we
				   better test for that */
				
				if (!gnome_vfs_uris_match (location, "file:///")) {
					home_uri = nautilus_get_home_directory_uri ();
					if (!gnome_vfs_uris_match (home_uri, location)) {	
						nautilus_window_go_home (NAUTILUS_WINDOW (window));
					} else {
						/* the last fallback is to go to a known place that can't be deleted! */
						nautilus_window_go_to (NAUTILUS_WINDOW (window), "file:///");
					}
					g_free (home_uri);
				} else {
					gtk_object_destroy (GTK_OBJECT (window));
				}
			} else {
				/* Since this is a window, destroying it will also unref it. */
				gtk_object_destroy (GTK_OBJECT (window));
			}
		} else {
			/* Clean up state of already-showing window */
			end_location_change (window);

			/* We disconnected this, so we need to re-connect it */
			viewed_file = nautilus_file_get (window->details->location);
			nautilus_window_set_viewed_file (window, viewed_file);
			nautilus_file_monitor_add (viewed_file, &window->details->viewed_file, 0);
			g_signal_connect_object (viewed_file, "changed",
						 G_CALLBACK (viewed_file_changed_callback), window, 0);
			nautilus_file_unref (viewed_file);
			
			/* Leave the location bar showing the bad location that the user
			 * typed (or maybe achieved by dragging or something). Many times
			 * the mistake will just be an easily-correctable typo. The user
			 * can choose "Refresh" to get the original URI back in the location bar.
			 */
		}
	}
	
	nautilus_file_unref (file);
}

/* Load a view into the window, either reusing the old one or creating
 * a new one. This happens when you want to load a new location, or just
 * switch to a different view.
 * If pending_location is set we're loading a new location and
 * pending_location/selection will be used. If not, we're just switching
 * view, and the current location will be used.
 */
static void
create_content_view (NautilusWindow *window,
		     const char *view_id)
{
        NautilusView *view;
	GList *selection;
	GtkAction *action;

 	/* FIXME bugzilla.gnome.org 41243: 
	 * We should use inheritance instead of these special cases
	 * for the desktop window.
	 */
        if (NAUTILUS_IS_DESKTOP_WINDOW (window)) {
        	/* We force the desktop to use a desktop_icon_view. It's simpler
        	 * to fix it here than trying to make it pick the right view in
        	 * the first place.
        	 */
		view_id = NAUTILUS_DESKTOP_ICON_VIEW_IID;
	} 

	action = gtk_action_group_get_action (window->details->main_action_group,
					      NAUTILUS_ACTION_ZOOM_IN);
	gtk_action_set_sensitive (action, FALSE);
	action = gtk_action_group_get_action (window->details->main_action_group,
					      NAUTILUS_ACTION_ZOOM_OUT);
	gtk_action_set_sensitive (action, FALSE);
	action = gtk_action_group_get_action (window->details->main_action_group,
					      NAUTILUS_ACTION_ZOOM_NORMAL);
	gtk_action_set_sensitive (action, FALSE);
        
        if (window->content_view != NULL &&
	    eel_strcmp (nautilus_view_get_view_id (window->content_view),
			view_id) == 0) {
                /* reuse existing content view */
                view = window->content_view;
                window->new_content_view = view;
        	g_object_ref (view);
        } else {
                /* create a new content view */
		view = nautilus_view_factory_create (view_id,
						     NAUTILUS_WINDOW_INFO (window));

                eel_accessibility_set_name (view, _("Content View"));
                eel_accessibility_set_description (view, _("View of the current folder"));
                
                connect_view (window, view);
		
                window->new_content_view = view;
        }

	/* Actually load the pending location and selection: */

        if (window->details->pending_location != NULL) {
		load_new_location (window,
				   window->details->pending_location,
				   window->details->pending_selection,
				   FALSE,
				   TRUE);

		eel_g_list_free_deep (window->details->pending_selection);
		window->details->pending_selection = NULL;
	} else if (window->details->location != NULL) {
		selection = nautilus_view_get_selection (window->content_view);
		load_new_location (window,
				   window->details->location,
				   selection,
				   FALSE,
				   TRUE);
		eel_g_list_free_deep (selection);
	} else {
		/* Something is busted, there was no location to load.
		   Just load the homedir. */
		nautilus_window_go_home (NAUTILUS_WINDOW (window));
		
	}
}

static void
load_new_location (NautilusWindow *window,
		   const char *location,
		   GList *selection,
		   gboolean tell_current_content_view,
		   gboolean tell_new_content_view)
{
	GList *selection_copy;
	NautilusView *view;
        
	g_assert (NAUTILUS_IS_WINDOW (window));
	g_assert (location != NULL);

	selection_copy = eel_g_str_list_copy (selection);

	view = NULL;
	
	/* Note, these may recurse into report_load_underway */
        if (window->content_view != NULL && tell_current_content_view) {
		view = window->content_view;
		nautilus_view_load_location (window->content_view, location);
        }
	
        if (window->new_content_view != NULL && tell_new_content_view &&
	    (!tell_current_content_view ||
	     window->new_content_view != window->content_view) ) {
		view = window->new_content_view;
		nautilus_view_load_location (window->new_content_view, location);
        }
	if (view != NULL) {
		/* window->new_content_view might have changed here if
		   report_load_underway was called from load_location */
		nautilus_view_set_selection (view, selection_copy);
	}
	
        eel_g_list_free_deep (selection_copy);
}

/* A view started to load the location its viewing, either due to
 * a load_location request, or some internal reason. Expect
 * a matching load_compete later
 */
void
nautilus_window_report_load_underway (NautilusWindow *window,
				      NautilusView *view)
{
        g_assert (NAUTILUS_IS_WINDOW (window));

        if (view == window->new_content_view) {
                location_has_really_changed (window);
        } else if (view == window->content_view) {
                nautilus_window_allow_stop (window, TRUE);
        } else {
		g_warning ("Got load_underway report from unknown view");
	}
}

/* This is called when we have decided we can actually change to the new view/location situation. */
static void
location_has_really_changed (NautilusWindow *window)
{
	GtkWidget *widget;
	char *location_copy;

	if (window->new_content_view != NULL) {
		widget = nautilus_view_get_widget (window->new_content_view);
		/* Switch to the new content view. */
		if (widget->parent == NULL) {
			disconnect_view (window, window->content_view);
			nautilus_window_set_content_view_widget (window, window->new_content_view);
		}
		g_object_unref (window->new_content_view);
		window->new_content_view = NULL;
	}

	location_copy = g_strdup (window->details->pending_location);
        if (window->details->pending_location != NULL) {
                /* Tell the window we are finished. */
                update_for_new_location (window);
	}

        free_location_change (window);

	if (location_copy != NULL) {
		g_signal_emit_by_name (window, "loading_uri",
				       location_copy);
		g_free (location_copy);
	}
}

static void
add_extension_extra_widgets (NautilusWindow *window, const char *uri)
{
	GList *providers, *l;
	GtkWidget *widget;
	
	providers = nautilus_module_get_extensions_for_type (NAUTILUS_TYPE_LOCATION_WIDGET_PROVIDER);

	for (l = providers; l != NULL; l = l->next) {
		NautilusLocationWidgetProvider *provider;
		
		provider = NAUTILUS_LOCATION_WIDGET_PROVIDER (l->data);
		widget = nautilus_location_widget_provider_get_widget (provider, uri, GTK_WIDGET (window));
		if (widget != NULL) {
			nautilus_window_add_extra_location_widget (window, widget);
		}
	}

	nautilus_module_extension_list_free (providers);
}

static void
nautilus_window_show_trash_bar (NautilusWindow *window)
{
	GtkWidget *bar;

	g_assert (NAUTILUS_IS_WINDOW (window));

	bar = nautilus_trash_bar_new ();
	gtk_widget_show (bar);

	nautilus_window_add_extra_location_widget (window, bar);
}

/* Handle the changes for the NautilusWindow itself. */
static void
update_for_new_location (NautilusWindow *window)
{
        char *new_location;
        NautilusFile *file;
	NautilusDirectory *directory;
	gboolean location_really_changed;
        
        new_location = window->details->pending_location;
        window->details->pending_location = NULL;

	set_displayed_location (window, new_location);

        update_history (window, window->details->location_change_type, new_location);
                
	location_really_changed = eel_strcmp (window->details->location, new_location) != 0;
		
        /* Set the new location. */
        g_free (window->details->location);
        window->details->location = new_location;
        
        /* Create a NautilusFile for this location, so we can catch it
         * if it goes away.
         */
        cancel_viewed_file_changed_callback (window);
        file = nautilus_file_get (window->details->location);
        nautilus_window_set_viewed_file (window, file);
        window->details->viewed_file_seen = !nautilus_file_is_not_yet_confirmed (file);
        window->details->viewed_file_in_trash = nautilus_file_is_in_trash (file);
        nautilus_file_monitor_add (file, &window->details->viewed_file, 0);
        g_signal_connect_object (file, "changed",
                                 G_CALLBACK (viewed_file_changed_callback), window, 0);
        nautilus_file_unref (file);
        
        /* Check if we can go up. */
        update_up_button (window);
	
	/* Set up the initial zoom levels */
	zoom_parameters_changed_callback (window->content_view,
					  window);

        /* Set up the content view menu for this new location. */
        nautilus_window_load_view_as_menus (window);
	
	/* Load menus from nautilus extensions for this location */
	nautilus_window_load_extension_menus (window);

	if (location_really_changed) {
		remove_extra_location_widgets (window);
		
		directory = nautilus_directory_get (window->details->location);
		if (NAUTILUS_IS_SEARCH_DIRECTORY (directory)) {
			nautilus_window_set_search_mode (window, TRUE, NAUTILUS_SEARCH_DIRECTORY (directory));
		} else {
			nautilus_window_set_search_mode (window, FALSE, NULL);
		}

		if (NAUTILUS_IS_TRASH_DIRECTORY (directory)) {
			nautilus_window_show_trash_bar (window);
		}

		nautilus_directory_unref (directory);

		add_extension_extra_widgets (window, window->details->location);
		
		update_extra_location_widgets_visibility (window);
	}

#if !NEW_UI_COMPLETE
        if (NAUTILUS_IS_NAVIGATION_WINDOW (window)) {
                /* Check if the back and forward buttons need enabling or disabling. */
                nautilus_navigation_window_allow_back (NAUTILUS_NAVIGATION_WINDOW (window), NAUTILUS_NAVIGATION_WINDOW (window)->back_list != NULL);
                nautilus_navigation_window_allow_forward (NAUTILUS_NAVIGATION_WINDOW (window), NAUTILUS_NAVIGATION_WINDOW (window)->forward_list != NULL);

                /* Change the location bar and path bar to match the current location. */
                nautilus_navigation_bar_set_location (NAUTILUS_NAVIGATION_BAR (NAUTILUS_NAVIGATION_WINDOW (window)->navigation_bar),
                                                      window->details->location);
		nautilus_path_bar_set_path (NAUTILUS_PATH_BAR (NAUTILUS_NAVIGATION_WINDOW (window)->path_bar),
					    window->details->location);
		nautilus_navigation_window_load_extension_toolbar_items (NAUTILUS_NAVIGATION_WINDOW (window));
        }
        
	if (NAUTILUS_IS_SPATIAL_WINDOW (window)) {
		/* Change the location button to match the current location. */
		nautilus_spatial_window_set_location_button
			(NAUTILUS_SPATIAL_WINDOW (window),
			 window->details->location);
	}                  
#endif
}

/* A location load previously announced by load_underway
 * has been finished */
void
nautilus_window_report_load_complete (NautilusWindow *window,
				      NautilusView *view)
{
        g_assert (NAUTILUS_IS_WINDOW (window));

	/* Only handle this if we're expecting it.
	 * Don't handle it if its from an old view we've switched from */
        if (view == window->content_view) {
                if (window->details->pending_scroll_to != NULL) {
                        nautilus_view_scroll_to_file (window->content_view,
						      window->details->pending_scroll_to);
                }
                end_location_change (window);
        }
}

static void
end_location_change (NautilusWindow *window)
{
	char *location;

	location = nautilus_window_get_location (window);
	if (location) {
		nautilus_debug_log (FALSE, NAUTILUS_DEBUG_LOG_DOMAIN_USER,
				    "finished loading window %p: %s", window, location);
		g_free (location);
	}

        nautilus_window_allow_stop (window, FALSE);

        /* Now we can free pending_scroll_to, since the load_complete
         * callback already has been emitted.
         */
        g_free (window->details->pending_scroll_to);
        window->details->pending_scroll_to = NULL;

        free_location_change (window);
}

static void
free_location_change (NautilusWindow *window)
{
        g_free (window->details->pending_location);
        window->details->pending_location = NULL;

	eel_g_list_free_deep (window->details->pending_selection);
	window->details->pending_selection = NULL;
	
        /* Don't free pending_scroll_to, since thats needed until
         * the load_complete callback.
         */

        if (window->details->determine_view_file != NULL) {
		nautilus_file_cancel_call_when_ready
			(window->details->determine_view_file,
			 got_file_info_for_view_selection_callback, window);
                window->details->determine_view_file = NULL;
        }

        if (window->new_content_view != NULL) {
                disconnect_view (window, window->new_content_view);
        	g_object_unref (window->new_content_view);
                window->new_content_view = NULL;
        }
}

static void
cancel_location_change (NautilusWindow *window)
{
	GList *selection;
	
        if (window->details->pending_location != NULL
            && window->details->location != NULL
            && window->content_view != NULL) {

                /* No need to tell the new view - either it is the
                 * same as the old view, in which case it will already
                 * be told, or it is the very pending change we wish
                 * to cancel.
                 */
		selection = nautilus_view_get_selection (window->new_content_view);
                load_new_location (window,
				   window->details->location,
				   selection,
				   TRUE,
				   FALSE);
		eel_g_list_free_deep (selection);
				   
        }

        end_location_change (window);
}

void
nautilus_window_report_view_failed (NautilusWindow *window,
				    NautilusView *view)
{
	gboolean do_close_window;
	char *fallback_load_location;
        g_warning ("A view failed. The UI will handle this with a dialog but this should be debugged.");


	do_close_window = FALSE;
	fallback_load_location = NULL;
	
	if (view == window->content_view) {
                disconnect_view (window, window->content_view);			
                nautilus_window_set_content_view_widget (window, NULL);
			
                report_current_content_view_failure_to_user (window, view);
        } else {
		/* Only report error on first try */
		if (window->details->location_change_type != NAUTILUS_LOCATION_CHANGE_FALLBACK) {
			report_nascent_content_view_failure_to_user (window, view);

			fallback_load_location = g_strdup (window->details->pending_location);
		} else {
			if (!GTK_WIDGET_VISIBLE (window)) {
				do_close_window = TRUE;
			}
		}
        }
        
        cancel_location_change (window);

	if (fallback_load_location != NULL) {
		/* We loose the pending selection change here, but who cares... */
		begin_location_change (window, fallback_load_location, NULL,
				       NAUTILUS_LOCATION_CHANGE_FALLBACK, 0, NULL);
		g_free (fallback_load_location);
	}

	if (do_close_window) {
		gtk_widget_destroy (GTK_WIDGET (window));
	}
}

static void
display_view_selection_failure (NautilusWindow *window, NautilusFile *file,
			       const char *location)
{
	GnomeVFSResult result_code;
	char *full_uri_for_display;
	char *uri_for_display;
	char *error_message;
	char *detail_message;
	char *scheme_string;
	const char *host_name;
	GtkDialog *dialog;
	GnomeVFSURI *vfs_uri;

	result_code = nautilus_file_get_file_info_result (file);
	
	/* Some sort of failure occurred. How 'bout we tell the user? */
	full_uri_for_display = eel_format_uri_for_display (location);
	/* Truncate the URI so it doesn't get insanely wide. Note that even
	 * though the dialog uses wrapped text, if the URI doesn't contain
	 * white space then the text-wrapping code is too stupid to wrap it.
	 */
	uri_for_display = eel_str_middle_truncate
		(full_uri_for_display, MAX_URI_IN_DIALOG_LENGTH);
	g_free (full_uri_for_display);
	        
	switch (result_code) {
	case GNOME_VFS_OK:
		if (nautilus_file_is_directory (file)) {
			error_message = g_strdup_printf
				(_("Couldn't display \"%s\"."),
				 uri_for_display);
			detail_message = g_strdup 
				(_("Nautilus has no installed viewer capable of displaying the folder."));
		} else {
			error_message = g_strdup_printf
				(_("Couldn't display \"%s\"."),
				 uri_for_display);
			detail_message = g_strdup 
				(_("The location is not a folder."));
		}
		break;
	case GNOME_VFS_ERROR_NOT_FOUND:
		error_message = g_strdup_printf
			(_("Couldn't find \"%s\"."), 
			 uri_for_display);
		detail_message = g_strdup 
			(_("Please check the spelling and try again."));
		break;
		
	case GNOME_VFS_ERROR_INVALID_URI:
		error_message = g_strdup_printf
			(_("\"%s\" is not a valid location."),
			 uri_for_display);
		detail_message = g_strdup 
			(_("Please check the spelling and try again."));
		break;
		
        case GNOME_VFS_ERROR_NOT_SUPPORTED:
		/* Can't create a vfs_uri and get the method from that, because 
		 * gnome_vfs_uri_new might return NULL.
		 */
		scheme_string = eel_str_get_prefix (location, ":");
		g_assert (scheme_string != NULL);  /* Shouldn't have gotten this error unless there's a : separator. */
		error_message = g_strdup_printf (_("Couldn't display \"%s\"."),
						 uri_for_display);
		detail_message = g_strdup_printf (_("Nautilus cannot handle %s: locations."),
						  scheme_string);
		g_free (scheme_string);
		break;
		
	case GNOME_VFS_ERROR_LOGIN_FAILED:
		error_message = g_strdup_printf (_("Couldn't display \"%s\"."),
						 uri_for_display);
		detail_message = g_strdup (_("The attempt to log in failed."));		
		break;
		
	case GNOME_VFS_ERROR_ACCESS_DENIED:
		error_message = g_strdup_printf (_("Couldn't display \"%s\"."),
						 uri_for_display);
		detail_message = g_strdup (_("Access was denied."));
		break;
		
	case GNOME_VFS_ERROR_HOST_NOT_FOUND:
		/* This case can be hit for user-typed strings like "foo" due to
		 * the code that guesses web addresses when there's no initial "/".
		 * But this case is also hit for legitimate web addresses when
		 * the proxy is set up wrong.
		 */
		vfs_uri = gnome_vfs_uri_new (location);
		host_name = gnome_vfs_uri_get_host_name (vfs_uri);
		error_message = g_strdup_printf (_("Couldn't display \"%s\", because no host \"%s\" could be found."),
						 uri_for_display,
						 host_name ? host_name : "");
		detail_message = g_strdup (_("Check that the spelling is correct and that your proxy settings are correct."));
		gnome_vfs_uri_unref (vfs_uri);
		break;
		
	case GNOME_VFS_ERROR_HOST_HAS_NO_ADDRESS:
		error_message = g_strdup_printf (_("Couldn't display \"%s\"."),
						 uri_for_display);
		detail_message = g_strdup (_("Check that your proxy settings are correct."));
		break;
		
	case GNOME_VFS_ERROR_NO_MASTER_BROWSER:
		error_message = g_strdup_printf
			(_("Couldn't display \"%s\", because Nautilus cannot contact the SMB master browser."),
			 uri_for_display);
		detail_message = g_strdup
			(_("Check that an SMB server is running in the local network."));
		break;

	case GNOME_VFS_ERROR_CANCELLED:
		g_free (uri_for_display);
		return;

	case GNOME_VFS_ERROR_SERVICE_NOT_AVAILABLE:
		error_message = g_strdup_printf (_("Couldn't display \"%s\"."),
						 uri_for_display);
		detail_message = g_strdup (_("Check if the service is available."));
		break;

	default:
		error_message = g_strdup_printf (_("Nautilus cannot display \"%s\"."),
						 uri_for_display);
		detail_message = g_strdup (_("Please select another viewer and try again."));
	}
	
	dialog = eel_show_error_dialog (error_message, detail_message, NULL);
	
	g_free (uri_for_display);
	g_free (error_message);
	g_free (detail_message);
}


void
nautilus_window_stop_loading (NautilusWindow *window)
{
	nautilus_view_stop_loading (window->content_view);
	
	if (window->new_content_view != NULL) {
		nautilus_view_stop_loading (window->new_content_view);
	}

        cancel_location_change (window);
}

void
nautilus_window_set_content_view (NautilusWindow *window,
                                  const char *id)
{
	NautilusFile *file;
	char *location;
	
	g_return_if_fail (NAUTILUS_IS_WINDOW (window));
        g_return_if_fail (window->details->location != NULL);
	g_return_if_fail (id != NULL);

	location = nautilus_window_get_location (window);
	nautilus_debug_log (FALSE, NAUTILUS_DEBUG_LOG_DOMAIN_USER,
			    "change view of window %p: \"%s\" to \"%s\"",
			    window, location, id);
	g_free (location);

        if (nautilus_window_content_view_matches_iid (window, id)) {
        	return;
        }

        end_location_change (window);

	file = nautilus_file_get (window->details->location);
	nautilus_file_set_metadata 
		(file, NAUTILUS_METADATA_KEY_DEFAULT_COMPONENT, NULL, id);
        nautilus_file_unref (file);
        
        nautilus_window_allow_stop (window, TRUE);

        if (nautilus_view_get_selection_count (window->content_view) == 0) {
                /* If there is no selection, queue a scroll to the same icon that
                 * is currently visible */
                window->details->pending_scroll_to = nautilus_view_get_first_visible_file (window->content_view);
        }
	window->details->location_change_type = NAUTILUS_LOCATION_CHANGE_RELOAD;
	
        create_content_view (window, id);
}

static void
zoom_level_changed_callback (NautilusView *view,
                             NautilusWindow *window)
{
	GtkAction *action;
	gboolean supports_zooming;
	
        g_assert (NAUTILUS_IS_WINDOW (window));

        /* This is called each time the component successfully completed
         * a zooming operation.
         */

	supports_zooming = nautilus_view_supports_zooming (view);

	action = gtk_action_group_get_action (window->details->main_action_group,
					      NAUTILUS_ACTION_ZOOM_IN);
	gtk_action_set_visible (action, supports_zooming);
	gtk_action_set_sensitive (action,
				  nautilus_view_can_zoom_in (view));
	
	action = gtk_action_group_get_action (window->details->main_action_group,
					      NAUTILUS_ACTION_ZOOM_OUT);
	gtk_action_set_visible (action, supports_zooming);
	gtk_action_set_sensitive (action,
				  nautilus_view_can_zoom_out (view));

	action = gtk_action_group_get_action (window->details->main_action_group,
					      NAUTILUS_ACTION_ZOOM_NORMAL);
	gtk_action_set_visible (action, supports_zooming);
	gtk_action_set_sensitive (action, supports_zooming);
}

static void
zoom_parameters_changed_callback (NautilusView *view,
                                  NautilusWindow *window)
{
        float zoom_level;
	GtkAction *action;

        g_assert (NAUTILUS_IS_WINDOW (window));

        /* The initial zoom level of a component is allowed to be 0.0 if
         * there is no file loaded yet. In this case we need to set the
         * commands insensitive but display the zoom control nevertheless
         * (the component is just temporarily unable to zoom, but the
         *  zoom control will "do the right thing" here).
         */
        zoom_level = nautilus_view_get_zoom_level (view);
        if (zoom_level == 0.0) {
		action = gtk_action_group_get_action (window->details->main_action_group,
						      NAUTILUS_ACTION_ZOOM_IN);
		gtk_action_set_sensitive (action, FALSE);
		action = gtk_action_group_get_action (window->details->main_action_group,
						      NAUTILUS_ACTION_ZOOM_OUT);
		gtk_action_set_sensitive (action, FALSE);
		action = gtk_action_group_get_action (window->details->main_action_group,
						      NAUTILUS_ACTION_ZOOM_NORMAL);
		gtk_action_set_sensitive (action, FALSE);

                /* Don't attempt to set 0.0 as zoom level. */
                return;
        }

        /* "zoom_parameters_changed" always implies "zoom_level_changed",
         * but you won't get both signals, so we need to pass it down.
         */
        zoom_level_changed_callback (view, window);
}

static void
title_changed_callback (NautilusView *view,
                        NautilusWindow *window)
{
        g_assert (NAUTILUS_IS_WINDOW (window));

        nautilus_window_update_title (window);
	nautilus_window_update_icon (window);
}

static void
connect_view (NautilusWindow             *window,
	      NautilusView               *view)
{
	g_signal_connect (view, "title_changed",
			  G_CALLBACK (title_changed_callback), window);
	g_signal_connect (view, "zoom_level_changed",
			  G_CALLBACK (zoom_level_changed_callback), window);
	g_signal_connect (view, "zoom_parameters_changed",
			  G_CALLBACK (zoom_parameters_changed_callback), window);
}

static void
disconnect_view (NautilusWindow             *window,
		 NautilusView               *view)
{
	if (view == NULL) {
		return;
	}
	
	g_signal_handlers_disconnect_by_func (view, title_changed_callback, window);
	g_signal_handlers_disconnect_by_func (view, zoom_level_changed_callback, window);
	g_signal_handlers_disconnect_by_func (view, zoom_parameters_changed_callback, window);
}

void
nautilus_window_manage_views_destroy (NautilusWindow *window)
{
	/* Disconnect view signals here so they don't trigger when
	 * views are destroyed.
         */

	if (window->content_view != NULL) {
		disconnect_view (window, window->content_view);
	}
	if (window->new_content_view != NULL) {
		disconnect_view (window, window->new_content_view);
	}
}

void
nautilus_window_manage_views_finalize (NautilusWindow *window)
{
        free_location_change (window);
        cancel_viewed_file_changed_callback (window);
}

void
nautilus_navigation_window_back_or_forward (NautilusNavigationWindow *window, 
                                            gboolean back, guint distance)
{
	GList *list;
	char *uri;
        char *scroll_pos;
        guint len;
        NautilusBookmark *bookmark;
	
	list = back ? window->back_list : window->forward_list;

        len = (guint) g_list_length (list);

        /* If we can't move in the direction at all, just return. */
        if (len == 0)
                return;

        /* If the distance to move is off the end of the list, go to the end
           of the list. */
        if (distance >= len)
                distance = len - 1;

        bookmark = g_list_nth_data (list, distance);
	uri = nautilus_bookmark_get_uri (bookmark);
        scroll_pos = nautilus_bookmark_get_scroll_pos (bookmark);
	begin_location_change
		(NAUTILUS_WINDOW (window),
		 uri, NULL,
		 back ? NAUTILUS_LOCATION_CHANGE_BACK : NAUTILUS_LOCATION_CHANGE_FORWARD,
		 distance,
                 scroll_pos);

	g_free (uri);
        g_free (scroll_pos);
}

/* reload the contents of the window */
void
nautilus_window_reload (NautilusWindow *window)
{
	char *location;
        char *current_pos;
	GList *selection;
	
        g_return_if_fail (NAUTILUS_IS_WINDOW (window));

	if (window->details->location == NULL) {
		return;
	}
	
	/* window->details->location can be free'd during the processing
	 * of begin_location_change, so make a copy
	 */
	location = g_strdup (window->details->location);
	current_pos = NULL;
	selection = NULL;
	if (window->content_view != NULL) {
		current_pos = nautilus_view_get_first_visible_file (window->content_view);
		selection = nautilus_view_get_selection (window->content_view);
	}
	begin_location_change
		(window, location, selection,
		 NAUTILUS_LOCATION_CHANGE_RELOAD, 0, current_pos);
        g_free (current_pos);
	g_free (location);
	eel_g_list_free_deep (selection);
}

static void
remove_all (GtkWidget *widget,
	    gpointer data)
{
	GtkContainer *container;
	container = GTK_CONTAINER (data);

	gtk_container_remove (container, widget);
}

static void
remove_extra_location_widgets (NautilusWindow *window)
{
	gtk_container_foreach (GTK_CONTAINER (window->details->extra_location_widgets),
			       remove_all,
			       window->details->extra_location_widgets);
}

void
nautilus_window_add_extra_location_widget (NautilusWindow *window,
					   GtkWidget *widget)
{
	gtk_box_pack_start (GTK_BOX (window->details->extra_location_widgets),
			    widget, TRUE, TRUE, 0);
}

static void
update_extra_location_widgets_visibility (NautilusWindow *window)
{
	GList *children;
	
	children = gtk_container_get_children (GTK_CONTAINER (window->details->extra_location_widgets));
	
	if (children != NULL) {
		gtk_widget_show (window->details->extra_location_widgets);
	} else {
		gtk_widget_hide (window->details->extra_location_widgets);
	}
	g_list_free (children);
}