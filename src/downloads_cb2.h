/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Richard Eckart
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

#ifndef _downloads_cb2_h_
#define _downloads_cb2_h_

#include "gui.h"

gboolean on_treeview_downloads_button_press_event(
	GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_treeview_downloads_select_row(
	GtkTreeView *tree_view, gpointer user_data);

gboolean on_treeview_downloads_queue_button_press_event(
	GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_treeview_downloads_queue_select_row(
	GtkTreeView *tree_view, gpointer user_data);

/***
 *** downloads panel
 ***/

/* active downloads */
void on_button_downloads_abort_clicked(GtkButton *button, gpointer user_data); 
void on_button_downloads_clear_stopped_clicked(
	GtkButton *button, gpointer user_data);
void on_button_downloads_resume_clicked(GtkButton *button, gpointer user_data); 

/* queued downloads */
void on_togglebutton_queue_freeze_toggled(
	GtkToggleButton *togglebutton, gpointer user_data);
void on_entry_queue_regex_activate(GtkEditable *editable, gpointer user_data); 


/***
 *** popup-downloads
 ***/

void on_popup_downloads_push_activate(
	GtkMenuItem *menuitem, gpointer user_data);
void on_popup_downloads_abort_named_activate(
	GtkMenuItem *menuitem, gpointer user_data); 
void on_popup_downloads_abort_host_activate(
	GtkMenuItem *menuitem, gpointer user_data); 
void on_popup_downloads_abort_sha1_activate(
	GtkMenuItem *menuitem, gpointer user_data); 
void on_popup_downloads_remove_file_activate(
	GtkMenuItem *menuitem, gpointer user_data); 
void on_popup_downloads_search_again_activate(
	GtkMenuItem *menuitem, gpointer user_data); 
void on_popup_downloads_queue_activate(
	GtkMenuItem *menuitem, gpointer user_data);
void on_popup_downloads_copy_url_activate(
	GtkMenuItem *menuitem, gpointer user_data);
void on_popup_downloads_connect_activate(
	GtkMenuItem *menuitem, gpointer user_data);
void on_popup_downloads_selection_get(GtkWidget *widget,
	GtkSelectionData * data, guint info, guint eventtime, gpointer user_data);
gint on_popup_downloads_selection_clear_event(
	GtkWidget *widget, GdkEventSelection *event);

/***
 *** popup-queue
 ***/

void on_popup_queue_start_now_activate(
	GtkMenuItem *menuitem, gpointer user_data);
void on_popup_queue_freeze_activate(
	GtkMenuItem *menuitem, gpointer user_data);
void on_popup_queue_search_again_activate(
	GtkMenuItem *menuitem, gpointer user_data);
void on_popup_queue_abort_host_activate(
	GtkMenuItem * menuitem, gpointer user_data); 
void on_popup_queue_abort_named_activate(
	GtkMenuItem * menuitem, gpointer user_data);
void on_popup_queue_abort_sha1_activate(
	GtkMenuItem * menuitem, gpointer user_data);
void on_popup_queue_abort_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_popup_queue_copy_url_activate(GtkMenuItem *menuitem, gpointer user_data);
void on_popup_queue_connect_activate(GtkMenuItem *menuitem, gpointer user_data);

#endif /* _downloads_cb2_h_ */
