/*
 * $Id$
 *
 * Copyright (c) 2001-2005, Raphael Manfredi, Richard Eckart
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

/**
 * @ingroup gtk
 * @file
 *
 * GUI filtering functions.
 *
 * @author Raphael Manfredi
 * @author Richard Eckart
 * @date 2001-2003
 *
 * @author Raphael Manfredi
 * @date 2005
 */

#include "common.h"

RCSID("$Id$")

#include "gtk/gui.h"
#include "gtk/gtkcolumnchooser.h"
#include "gtk/search.h"
#include "gtk/statusbar.h"
#include "gtk/bitzi.h"			/* Bitzi GTK functions */
#include "gtk/columns.h"
#include "gtk/misc.h"
#include "gtk/settings.h"
#include "search_cb.h"

#include "if/gui_property.h"
#include "if/gui_property_priv.h"
#include "if/gnet_property.h"
#include "if/bridge/ui2c.h"

#include "if/core/search.h"
#include "if/core/sockets.h"

#include "lib/atoms.h"
#include "lib/cq.h"
#include "lib/glib-missing.h"
#include "lib/iso3166.h"
#include "lib/vendors.h"
#include "lib/utf8.h"
#include "lib/override.h"		/* Must be the last header included */

static gint search_details_selected_row = -1;

static record_t *selected_record; 

gchar * 
search_details_get_text(GtkWidget *widget)
{
	gchar *text = NULL;

	if (
		search_details_selected_row >= 0 &&
		gtk_clist_get_text(GTK_CLIST(widget), search_details_selected_row, 1,
			&text)
	) {
		return g_strdup(text);
	} else {
		return NULL;
	}
}

/***
 *** Private functions
 ***/

/* Display XML data from the result if any */
static void
search_set_xml_metadata(const record_t *rc)
{
	GtkText *xml;

	xml = GTK_TEXT(gui_main_window_lookup("text_result_info_xml"));
	gtk_text_freeze(xml);
	gtk_text_set_point(xml, 0);
	gtk_text_forward_delete(xml, gtk_text_get_length(xml));
	if (rc) {
		gchar *text;
	
		text = rc->xml ? search_xml_indent(rc->xml) : NULL;
		gtk_text_set_point(xml, 0);
		gtk_text_insert(xml, NULL, NULL, NULL,
			text ? lazy_utf8_to_ui_string(text) : "", (-1));
		G_FREE_NULL(text);
	}
	gtk_text_thaw(xml);
}

static GtkCList *clist_search_details;

void
search_gui_clear_details(void)
{
	if (clist_search_details) {
		gtk_clist_clear(clist_search_details);
	}
}

void
search_gui_append_detail(const gchar *name, const gchar *value)
{
 	const gchar *titles[2];

	g_return_if_fail(clist_search_details);

	titles[0] = name;
	titles[1] = EMPTY_STRING(value);
    gtk_clist_append(clist_search_details, (gchar **) titles);
}
	
/**
 *	Activates/deactivates buttons and popups based on what is selected
 */
/**
 * Set or clear (when rc == NULL) the information about the record.
 */
static void
search_gui_refresh_details(const record_t *rc)
{
	if (NULL == clist_search_details) {
		static const gchar name[] = "clist_search_details";
		clist_search_details = GTK_CLIST(gui_main_window_lookup(name));
		gtk_clist_set_column_auto_resize(clist_search_details, 0, TRUE);
	}
	g_return_if_fail(clist_search_details);

    gtk_clist_freeze(clist_search_details);
	search_gui_set_details(rc);
    gtk_clist_thaw(clist_search_details);
	search_set_xml_metadata(rc);
}

record_t *
search_gui_get_record(GtkCTree *ctree, GtkCTreeNode *node)
{
	gui_record_t *grc;

	/*
     * Rows with NULL data can appear when inserting new rows
     * because the selection is resynced and the row data cannot
     * be set until insertion and therefore also selection syncing
     * is done.
     *      -- Richard, 20/06/2002
     *
     * Can this really happen???
     *      -- Richard, 18/04/2004
     */
	grc = node ? gtk_ctree_node_get_row_data(ctree, node) : NULL;
	return grc ? grc->shared_record : NULL;
}

/**
 * Autoselects all searches matching given node in given tree, if the
 * unexpanded root of the tree is selected.  Otherwise, select only the
 * node on which they clicked.
 *
 * @return the amount of entries selected.
 */
static gint
search_cb_autoselect(GtkCTree *ctree, GtkCTreeNode *node)
{
    guint32 sel_sources = 0;

	g_return_val_if_fail(ctree, 0);
	g_return_val_if_fail(node, 0);

	gtk_signal_handler_block_by_func(GTK_OBJECT(ctree),
		GTK_SIGNAL_FUNC(on_ctree_search_results_select_row), NULL);

	/*
	 * If the selected node is expanded, select it only.
	 */

	gtk_ctree_select(ctree, node);
	sel_sources++;		/* We already selected the parent (folded) node */

	if (!GTK_CTREE_ROW(node)->expanded) {
		GtkCTreeNode *child;

		/*
		 * Node is not expanded.  Select all its children.
		 */

		for (
			child = GTK_CTREE_ROW(node)->children;
			child != NULL;
			child = GTK_CTREE_NODE_SIBLING(child)
		) {
			gtk_ctree_select(ctree, child);
			sel_sources++;
		}

		if (sel_sources > 1)
			statusbar_gui_message(15,
				"auto selected %d sources by urn:sha1", sel_sources);
	}

	gtk_signal_handler_unblock_by_func(GTK_OBJECT(ctree),
		GTK_SIGNAL_FUNC(on_ctree_search_results_select_row), NULL);

	return sel_sources;
}

/***
 *** Glade callbacks
 ***/


/**
 *	When a search string is entered, activate the search button
 */
void
on_entry_search_changed(GtkEditable *editable, gpointer unused_udata)
{
	gchar *s = STRTRACK(gtk_editable_get_chars(editable, 0, -1));

	(void) unused_udata;
	
	g_strstrip(s);
	gtk_widget_set_sensitive(gui_main_window_lookup("button_search"),
		s[0] != '\0');
	G_FREE_NULL(s);

    gui_prop_set_boolean_val(PROP_SEARCHBAR_VISIBLE, TRUE);
}


gboolean
on_clist_search_results_key_press_event(GtkWidget *unused_widget,
	GdkEventKey *event, gpointer unused_udata)
{
    g_assert(event != NULL);

	(void) unused_widget;
	(void) unused_udata;

    switch (event->keyval) {
    case GDK_Return:
        search_gui_download_files();
        return TRUE;
	case GDK_Delete:
        search_gui_discard_files();
		return TRUE;
    default:
        return FALSE;
    }
}


/**
 *	Handles showing the popup in the event of right-clicks and downloading
 *	for double-clicks
 */
gboolean
on_clist_search_results_button_press_event(GtkWidget *widget,
	GdkEventButton *event, gpointer unused_udata)
{
	(void) unused_udata;

	switch (event->button) {
	case 1:
        /* left click section */
		if (event->type == GDK_2BUTTON_PRESS) {
			gui_signal_stop_emit_by_name(widget, "button_press_event");
			return FALSE;
		}
		if (event->type == GDK_BUTTON_PRESS) {
			static guint click_time = 0;

			search_gui_set_cursor_position(event->x, event->y);

			if ((event->time - click_time) <= 250) {
				gint row = 0;
				gint column = 0;

				/*
				 * 2 clicks within 250 msec == doubleclick.
				 * Surpress further events
				 */
				gui_signal_stop_emit_by_name(widget, "button_press_event");

				if (gtk_clist_get_selection_info(GTK_CLIST(widget), event->x,
					event->y, &row, &column)) {

					search_gui_download_files();
                    return TRUE;
				}
			} else {
				click_time = event->time;
				return FALSE;
			}
		}
		return FALSE;

	case 3:
        /* right click section (popup menu) */
    	if (search_gui_get_current_search()) {
			GtkMenuItem *item;

        	search_gui_refresh_popup();

			item = GTK_MENU_ITEM(
					gui_popup_search_lookup("popup_search_toggle_tabs"));
        	gtk_label_set(GTK_LABEL(item->item.bin.child),
				GUI_PROPERTY(search_results_show_tabs)
					? _("Show search list")
					: _("Show tabs"));
			gtk_menu_popup(GTK_MENU(gui_popup_search()), NULL, NULL, NULL, NULL,
                     event->button, event->time);
        }
		return TRUE;
	}

	return FALSE;
}


/**
 *	Sort search according to selected column
 */
void
on_clist_search_results_click_column(GtkCList *clist, gint column,
	gpointer unused_udata)
{
    search_t *search;

	(void) unused_udata;
    g_assert(clist != NULL);

    search = search_gui_get_current_search();
	if (search == NULL)
		return;

    /* rotate or initialize search order */
	if (column == search->sort_col) {
        switch (search->sort_order) {
        case SORT_ASC:
            search->sort_order = SORT_DESC;
           	break;
        case SORT_DESC:
            search->sort_order = SORT_NONE;
            break;
        case SORT_NONE:
            search->sort_order = SORT_ASC;
        }
	} else {
		search->sort_col = column;
		search->sort_order = SORT_ASC;
	}

	search_gui_sort_column(search, column); /* Sort column, draw arrow */
}

gboolean
on_clist_search_details_key_press_event(GtkWidget *widget,
	GdkEventKey *event, gpointer unused_udata)
{
	(void) unused_udata;

	switch (event->keyval) {
	guint modifier;
	case GDK_c:
		modifier = gtk_accelerator_get_default_mod_mask() & event->state;
		if (GDK_CONTROL_MASK == modifier) {
			char *text;

			text = search_details_get_text(widget);
			clipboard_set_text(widget, text);
			G_FREE_NULL(text);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

void
on_clist_search_details_select_row(GtkCList *unused_clist,
	gint row, gint unused_column, GdkEventButton *unused_event,
	gpointer unused_udata)
{
	(void) unused_clist;
	(void) unused_column;
	(void) unused_event;
	(void) unused_udata;
	search_details_selected_row = row;
}

void
on_clist_search_details_unselect_row(GtkCList *unused_clist,
	gint unused_row, gint unused_column, GdkEventButton *unused_event,
	gpointer unused_udata)
{
	(void) unused_clist;
	(void) unused_row;
	(void) unused_column;
	(void) unused_event;
	(void) unused_udata;
	search_details_selected_row = -1;
}

static cevent_t *row_selected_ev;

#define ROW_SELECT_TIMEOUT	100 /* milliseconds */

static gint
gui_record_cmp(gconstpointer p, gconstpointer q)
{
	const gui_record_t *a = p, *b = q;
	return a->shared_record != b->shared_record;
}

static void
row_selected_expire(cqueue_t *unused_cq, gpointer unused_udata)
{
	search_t *search;

	(void) unused_cq;
	(void) unused_udata;

	row_selected_ev = NULL;

    search = search_gui_get_current_search();
	if (search) {
    	search_gui_refresh_popup();
		search_gui_refresh_details(selected_record);
		if (selected_record) {
			GtkCTreeNode *node;
			gui_record_t grc;
			
			grc.shared_record = selected_record;
        	node = gtk_ctree_find_by_row_data_custom(GTK_CTREE(search->tree),
					gtk_ctree_node_nth(GTK_CTREE(search->tree), 0),
					&grc, gui_record_cmp);
			search_cb_autoselect(GTK_CTREE(search->tree), GTK_CTREE_NODE(node));
		}
	} else {
		search_gui_clear_details();
	}
}

static void
selected_row_changed(GtkCTree *ctree, GtkCTreeNode *node)
{
	if (selected_record) {
		search_gui_unref_record(selected_record);
	}
	selected_record = search_gui_get_record(ctree, GTK_CTREE_NODE(node));
	if (selected_record) {
		search_gui_ref_record(selected_record);
	}

	if (row_selected_ev) {
		cq_resched(callout_queue, row_selected_ev, ROW_SELECT_TIMEOUT);
	} else {
		row_selected_ev = cq_insert(callout_queue, ROW_SELECT_TIMEOUT,
							row_selected_expire, NULL);
	}
}

/**
 *	This function is called when the user selects a row in the
 *	search results pane. Autoselection takes place here.
 */
void
on_ctree_search_results_select_row(GtkCTree *ctree,
	GList *node, gint unused_column, gpointer unused_udata)
{
	(void) unused_column;
	(void) unused_udata;

	selected_row_changed(ctree, GTK_CTREE_NODE(node));
}

void
on_ctree_search_results_unselect_row(GtkCTree *ctree, GList *unused_node,
	gint unused_column, gpointer unused_udata)
{
	(void) unused_node;
	(void) unused_column;
	(void) unused_udata;

	selected_row_changed(ctree, NULL);
}

/***
 *** Search results popup
 ***/

/**
 * Request host browsing for the selected entries.
 */
void
search_gui_browse_selected(void)
{
    search_t *search;
	GtkCTree *ctree;
	GList *selected;
	GList *l;;

    search = search_gui_get_current_search();
    g_assert(search != NULL);

    ctree = GTK_CTREE(search->tree);
	selected = GTK_CLIST(ctree)->selection;

	if (selected == NULL) {
        statusbar_gui_message(15, "*** No search result selected! ***");
		return;
	}

	for (l = selected; l != NULL; l = g_list_next(l)) {
		GtkCTreeNode *node = l->data;
		gui_record_t *grc;
		results_set_t *rs;
		record_t *rc;
		guint32 flags = 0;

		if (node == NULL)
			break;

		grc = gtk_ctree_node_get_row_data(ctree, node);
		rc = grc->shared_record;

		if (!rc)
			continue;

		rs = rc->results_set;
		flags |= 0 != (rs->status & ST_FIREWALL) ? SOCK_F_PUSH : 0;
		flags |= 0 != (rs->status & ST_TLS) ? SOCK_F_TLS : 0;

		(void) search_gui_new_browse_host(
				rs->hostname, rs->addr, rs->port,
				rs->guid, rs->proxies, flags);
	}
}

/**
 *	Given a GList of GtkCTreeNodes, return a new list pointing to the shared
 *	record contained by the row data.
 *	List will have to be freed later on.
 */
GSList *
search_cb_collect_ctree_data(GtkCTree *ctree,
	GList *node_list, GCompareFunc cfn)
{
	GSList *data_list = NULL;
	gui_record_t *grc;
	record_t *rc;

	for (/* empty */; node_list != NULL; node_list = g_list_next(node_list)) {

		if (node_list->data != NULL) {
			grc = gtk_ctree_node_get_row_data(ctree, node_list->data);
			rc = grc->shared_record;
			if (!cfn || NULL == g_slist_find_custom(data_list, rc, cfn))
				data_list = g_slist_prepend(data_list, rc);
		}
	}

	return g_slist_reverse(data_list);
}

static void
add_filter(filter_t *filter, GFunc filter_add_func, GCompareFunc cfn)
{
    GList *node_list;
    GSList *data_list = NULL;
    search_t *search;

    search = search_gui_get_current_search();
    g_assert(search != NULL);

    gtk_clist_freeze(GTK_CLIST(search->tree));

	node_list = g_list_copy(GTK_CLIST(search->tree)->selection);
	data_list = search_cb_collect_ctree_data(GTK_CTREE(search->tree),
					node_list, cfn);

    g_slist_foreach(data_list, filter_add_func, filter);

    gtk_clist_thaw(GTK_CLIST(search->tree));
	g_slist_free(data_list);
	g_list_free(node_list);
}

static void
search_add_filter(GFunc filter_add_func, GCompareFunc cfn)
{
    search_t *search;
	
    search = search_gui_get_current_search();
    g_assert(search != NULL);
	
	add_filter(search->filter, filter_add_func, cfn);
}

static void
global_add_filter(GFunc filter_add_func, GCompareFunc cfn)
{
	add_filter(filter_get_global_pre(), filter_add_func, cfn);
}

/**
 *	For all selected results, create a filter based on name
 */
void
on_popup_search_drop_name_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

    search_add_filter((GFunc) filter_add_drop_name_rule, gui_record_name_eq);
}


/**
 *	For all selected results, create a filter based on sha1
 */
void
on_popup_search_drop_sha1_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;
	
    search_add_filter((GFunc) filter_add_drop_sha1_rule, gui_record_sha1_eq);
}


/**
 *	For all selected results, create a filter based on host
 */
void
on_popup_search_drop_host_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

    search_add_filter((GFunc) filter_add_drop_host_rule, gui_record_host_eq);
}


/**
 *	For all selected results, create a global filter based on name
 */
void
on_popup_search_drop_name_global_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

    global_add_filter((GFunc) filter_add_drop_name_rule, gui_record_name_eq);
}

/**
 *	For all selected results, create a global filter based on sha1
 */
void
on_popup_search_drop_sha1_global_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

    global_add_filter((GFunc) filter_add_drop_sha1_rule, gui_record_sha1_eq);
}

/**
 *	For all selected results, create a global filter based on host
 */
void on_popup_search_drop_host_global_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

    global_add_filter((GFunc) filter_add_drop_host_rule, gui_record_host_eq);
}

/**
 *	Please add comment
 */
void
on_popup_search_config_cols_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
    GtkWidget * cc;
    search_t *search;

	(void) unused_menuitem;
	(void) unused_udata;

    search = search_gui_get_current_search();
    g_return_if_fail(search != NULL);
    g_assert(search->tree != NULL);

    cc = gtk_column_chooser_new(GTK_WIDGET(search->tree));
    gtk_menu_popup(GTK_MENU(cc), NULL, NULL, NULL, NULL, 1,
		gtk_get_current_event_time());

    /* GtkColumnChooser takes care of cleaning up itself */
}

/**
 * Queue a bitzi queries from the search context menu
 */
void
on_popup_search_metadata_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
    GList *node_list;
	GSList *data_list;
    search_t *search;
	guint32 bitzi_debug;

	(void) unused_menuitem;
	(void) unused_udata;

    search = search_gui_get_current_search();
    g_assert(search != NULL);

    gtk_clist_freeze(GTK_CLIST(search->tree));

	node_list = g_list_copy(GTK_CLIST(search->tree)->selection);
	data_list = search_cb_collect_ctree_data(GTK_CTREE(search->tree),
					node_list, gui_record_sha1_eq);

	/* Make sure the column is actually visible. */
	{
		static const gint min_width = 80;
		GtkCList *clist = GTK_CLIST(search->tree);

    	gtk_clist_set_column_visibility(clist, c_sr_meta, TRUE);
		if (clist->column[c_sr_meta].width < min_width)
    		gtk_clist_set_column_width(clist, c_sr_meta, min_width);
	}
	
	/* Queue up our requests */
    gnet_prop_get_guint32_val(PROP_BITZI_DEBUG, &bitzi_debug);
	if (bitzi_debug)
		g_message("on_popup_search_metadata_activate: %d items, %p",
			  g_slist_position(data_list, g_slist_last(data_list)) + 1,
			  cast_to_gconstpointer(data_list));

	G_SLIST_FOREACH(data_list, search_gui_queue_bitzi_by_sha1);

	gtk_clist_thaw(GTK_CLIST(search->tree));
	g_slist_free(data_list);
	g_list_free(node_list);
}

void
on_popup_search_copy_magnet_activate(GtkMenuItem *unused_item,
	gpointer unused_udata)
{
	search_t *search;

	(void) unused_item;
	(void) unused_udata;

	search = search_gui_get_current_search();
	g_return_if_fail(search);

	if (selected_record) {
		char *magnet = search_gui_get_magnet(search, selected_record);
		clipboard_set_text(gui_main_window(), magnet);
		G_FREE_NULL(magnet);
	}
}

void
search_gui_callbacks_shutdown(void)
{
	/*
 	 *	Remove delayed callbacks
 	 */
	cq_cancel(callout_queue, &row_selected_ev);
	search_gui_clear_details();
	if (selected_record) {
		search_gui_unref_record(selected_record);
		selected_record = NULL;
	}
}

/* -*- mode: cc-mode; tab-width:4; -*- */
/* vi: set ts=4 sw=4 cindent: */
