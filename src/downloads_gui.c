/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
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

#include "gui.h"

#ifdef USE_GTK1

RCSID("$Id$");

#include "downloads_gui.h"
#include "downloads_gui_common.h"
#include "downloads_cb.h"

#include "downloads.h" /* FIXME: remove this dependency */
#include "dmesh.h" /* FIXME: remove this dependency */
#include "http.h" /* FIXME: remove this dependency */
#include "pproxy.h" /* FIXME: remove this dependency */
#include "statusbar_gui.h"
#include "parq.h"

#include "override.h"		/* Must be the last header included */

static gchar tmpstr[4096];
static GHashTable *parents;			/* table of parent download iterators */
static GHashTable *parents_queue;	/* table of parent queued dl iterators */

/*
 * parents_gui_time
 *
 * I did not know how to attach meta information to the parent GUI structures,
 * so I created this hash table to record the last time we update the parent
 * information in the GUI, to avoid doing too costly lookups in the ctree when
 * the information is already accurate, with a granularity of a second.
 *
 *		--RAM, 03/01/2004.
 */
static GHashTable *parents_gui_time;	/* Time at which parent was updated */

/*
 * parents_children
 * parents_queue_children
 *
 * Keeps track of the amount of children a parent has, to avoid costly
 * calls to count_node_children.
 */
static GHashTable *parents_children;
static GHashTable *parents_queue_children;

static GtkCTree *ctree_downloads = NULL;
static GtkCTree *ctree_downloads_queue = NULL;

#define IO_STALLED		60		/* If nothing exchanged after that many secs */
#define DL_GUI_TREE_SPACE 5	 /* The space between a child node and a parent */

/***
 *** Private functions
 ***/
 
/*
 * add_parent_with_fi_handle
 *
 * Add the given tree node to the hashtable.
 * The key is an int ref on the fi_handle for a given download.
 *
 */
static inline void add_parent_with_fi_handle(
	GHashTable *ht, gpointer key, GtkCTreeNode *data)
{
	/*
	 * Since we're inserting an integer ref into the hash table, we need
	 * to make it an atom.
	 */

	g_hash_table_insert(ht, atom_int_get(key), data);
}

/*
 * remove_parent_with_fi_handle
 *
 * Removes the treenode matching the given fi_handle from the hash table
 * and frees the original key used to store it.
 */
static inline void remove_parent_with_fi_handle(
	GHashTable *ht, const gnet_fi_t *fi_handle)
{
	gpointer key;
	GtkCTreeNode *data = NULL;
	gpointer orig_key;
 
	key = (gpointer) fi_handle;

	if (
		g_hash_table_lookup_extended(ht, key,
			(gpointer) &orig_key, (gpointer) &data)
	) {
		g_hash_table_remove(ht, key);
		atom_int_free(orig_key);
	} else
		g_warning("remove_parent_with_fi_handle:can't find fi in hash table!");

	/*
	 * If we removed a `parent', we must also delete the corresponding
	 * entry in the table tracking the last GUI update time.
	 */

	if (
		ht == parents &&
		g_hash_table_lookup_extended(parents_gui_time, key,
			(gpointer) &orig_key, (gpointer) &data)
	) {
		g_hash_table_remove(parents_gui_time, key);
		atom_int_free(orig_key);
	}
}


/*
 * find_parent_with_fi_handle
 *
 * Returns the tree iterator corresponding to the given key, an atomized
 * fi_handle.
 *
 */
static inline GtkCTreeNode *find_parent_with_fi_handle(
	GHashTable *ht, gpointer key)
{
	return g_hash_table_lookup(ht, key);
}

/*
 * downloads_gui_free_parent
 *
 * Hash table iterator to free the allocated keys.
 */
gboolean downloads_gui_free_parent(gpointer key, gpointer value, gpointer x)
{
	atom_int_free(key);
	return TRUE;
}

/*
 * record_parent_gui_update
 *
 * Remember when we did the last GUI update of the parent.
 */
static inline void record_parent_gui_update(gpointer key, time_t when)
{
	gpointer orig_key;
	gpointer data;
	
	if (
		g_hash_table_lookup_extended(parents_gui_time, key,
			(gpointer) &orig_key, (gpointer) &data)
	)
		g_hash_table_insert(parents_gui_time, orig_key, GINT_TO_POINTER(when));
	else
		g_hash_table_insert(parents_gui_time, atom_int_get(key),
			GINT_TO_POINTER(when));
}

/*
 * get_last_parent_gui_update
 *
 * Returns the last time we updated the GUI of the parent.
 */
static inline time_t get_last_parent_gui_update(gpointer key)
{
	return (time_t) GPOINTER_TO_INT(g_hash_table_lookup(parents_gui_time, key));
}

/*
 * parent_gui_needs_update
 *
 * Returns whether the parent of download `d', if any, needs a GUI update.
 */
static inline gboolean parent_gui_needs_update(struct download *d, time_t now)
{
	gpointer key = &d->file_info->fi_handle;
	gpointer parent = find_parent_with_fi_handle(parents, key);

	if (parent == NULL)
		return FALSE;

	return get_last_parent_gui_update(key) != now;
}

/*
 * parent_children_add
 *
 * Add (arithmetically) `x' to the amount of children of the parent, identified
 * by its fileifo hande.
 *
 * The `ctree' is used to determine whether we're managing a parent from
 * the active downlods or the queue downlaods.
 *
 * Returns the new amount of children (use x=0 to get the current count).
 */
static gint parent_children_add(GtkCTree *ctree, gpointer key, gint x)
{
	GHashTable *ht = NULL;
	gpointer k;
	gpointer v;
	gint cnt;

	if (ctree == ctree_downloads)
		ht = parents_children;
	else if (ctree == ctree_downloads_queue)
		ht = parents_queue_children;
	else
		g_error("unknown ctree object");

	/*
	 * If nothing in the table already, we can only add a children.
	 */

	if (!g_hash_table_lookup_extended(ht, key, &k, &v)) {
		g_assert(x >= 0);
		if (x == 0)
			return 0;
		g_hash_table_insert(ht, atom_int_get(key), GINT_TO_POINTER(x));
		return x;
	}

	g_assert(*(gint *) k == *(gint *) key);

	cnt = GPOINTER_TO_INT(v);

	/*
	 * Update table entry, removing it when the children count reaches 0.
	 */

	if (x != 0) {
		cnt += x;
		g_assert(cnt >= 0);
		if (cnt > 0)
			g_hash_table_insert(ht, k, GINT_TO_POINTER(cnt));
		else {
			g_hash_table_remove(ht, k);
			atom_int_free(k);
		}
	}

	return cnt;
}


/* 
 * 	downloads_gui_collect_ctree_data
 *
 *	Given a GList of GtkCTreeNodes, return a new list pointing to the row data
 *	If unselect is TRUE, unselect all nodes in the list
 *  If children is TRUE, check and strip out header nodes.  Instead of adding
 *  the headers, add all of their children.
 *	List will have to be freed later on.
 *
 * FIXME: Worst case approaches O(n*n) ensuring no duplicate children are added
 * FIXME: There are a lot of glist "appends" in here => unneccesary O(n)
 */
GList *downloads_gui_collect_ctree_data(GtkCTree *ctree, GList *node_list, 
	gboolean unselect, gboolean add_children)
{
	GList *data_list = NULL, *dup_list = NULL;
	struct download *d, *dtemp;
	GtkCTreeNode *node, *parent;
	GtkCTreeRow *row;
	
	for (; node_list != NULL; node_list = g_list_next(node_list)) {
	
		if (node_list->data != NULL) {
			d = gtk_ctree_node_get_row_data(ctree, node_list->data);

			if (DL_GUI_IS_HEADER == d) { /* Is a parent */
				
				parent = GTK_CTREE_NODE(node_list->data);
				row = GTK_CTREE_ROW(parent);
				node = row->children;

				if (add_children) {
					/* Do not add parent, but add all children of parent */
					for (; NULL != node; row = GTK_CTREE_ROW(node), 
						node = row->sibling) {		
						dtemp = gtk_ctree_node_get_row_data(ctree, node);

						data_list = g_list_append(data_list, dtemp);
						dup_list = g_list_append(dup_list, dtemp);
					}
				} else {
					/* We only want to add one download struct to represent  all 
					 * the nodes under this parent node.  We choose the download
					 * struct of the first child.   
					 */
					dtemp = gtk_ctree_node_get_row_data(ctree, node);
					data_list = g_list_append(data_list, dtemp);
					dup_list = g_list_append(dup_list, dtemp);
				}
			} else {

				/* Make sure we are not adding a child twice (if the child and
				 * the parent was selected.  
				 */
				if (NULL == g_list_find(dup_list, d))
					data_list = g_list_append(data_list, d);
			}
			if (unselect)
				gtk_ctree_unselect(ctree, node_list->data);
		}
	}
	
	g_list_free(dup_list);	
	data_list = g_list_first(data_list);
	return data_list;
}

/*
 *	downloads_gui_any_status
 *
 *	Returns true if any of the active downloads in the same tree as the given 
 * 	download are in the specified status.
 */
static gboolean downloads_gui_any_status(
	struct download *d, download_status_t status)
{
	gpointer key;
	GtkCTreeNode *node, *parent;
	GtkCTreeRow *row;
    GtkCTree *ctree_downloads =
		GTK_CTREE(lookup_widget(main_window, "ctree_downloads"));

	if (!d->file_info)
		return FALSE;
	
	key = (gpointer) &d->file_info->fi_handle;
	parent = find_parent_with_fi_handle(parents, key);

	if (!parent)
		return FALSE;

	row = GTK_CTREE_ROW(parent);
	node = row->children;
			
	for (; node != NULL; row = GTK_CTREE_ROW(node), node = row->sibling) {		
		struct download *drecord;

		drecord = gtk_ctree_node_get_row_data(ctree_downloads, node);
		if (!drecord || DL_GUI_IS_HEADER == drecord)
			continue;
					
		if (drecord->status == status)
			return TRUE;
	}					

	return FALSE;
}

/*
 *	downloads_gui_all_aborted
 *
 *	Returns true if all the active downloads in the same tree as the given 
 * 	download are aborted (status is GTA_DL_ABORTED or GTA_DL_ERROR).
 */
gboolean downloads_gui_all_aborted(struct download *d)
{
	struct download *drecord = NULL;
	gpointer key;
	gboolean all_aborted = FALSE;
	GtkCTreeNode *node, *parent;
	GtkCTreeRow *row;
    GtkCTree *ctree_downloads = GTK_CTREE
            (lookup_widget(main_window, "ctree_downloads"));


	if (NULL != d->file_info) {
			
		key = &d->file_info->fi_handle;
		parent = find_parent_with_fi_handle(parents, key);

		if (NULL != parent) {

			all_aborted = TRUE;	

			row = GTK_CTREE_ROW(parent);
			node = row->children;
			
			for (; NULL != node; 
				row = GTK_CTREE_ROW(node), node = row->sibling) {		
				
				drecord = gtk_ctree_node_get_row_data(ctree_downloads, node);

				if ((NULL == drecord) || (-1 == GPOINTER_TO_INT(drecord)))
					continue;
					
				if ((GTA_DL_ABORTED != drecord->status) 
					&& (GTA_DL_ERROR != drecord->status)) {
					all_aborted = FALSE;
					break;
				}
			}					
		}
	}

	return all_aborted;
}


/*
 *	downloads_gui_update_parent_status
 *
 * 	Finds parent of given download in the active download tree and changes the
 *  status column to the given string.  Returns true if status is changed.
 */
gboolean downloads_gui_update_parent_status(
	struct download *d, time_t now, gchar *new_status)
{
	gpointer key;
	gboolean changed = FALSE;
	
	GdkColor *color;
	GtkCTreeNode *parent;
    GtkCTree *ctree_downloads = GTK_CTREE
            (lookup_widget(main_window, "ctree_downloads"));


    if (NULL != d->file_info) {

		key = &d->file_info->fi_handle;
		parent = find_parent_with_fi_handle(parents, key);

		if (NULL != parent) {

			changed = TRUE;
			gtk_ctree_node_set_text(ctree_downloads, parent,
				c_dl_status, new_status);

			if (0 == strcmp(new_status, "Push mode")) {
				color = &(gtk_widget_get_style(GTK_WIDGET(ctree_downloads))
					->fg[GTK_STATE_INSENSITIVE]);
	
				gtk_ctree_node_set_foreground(ctree_downloads, parent, color);		
			}
			record_parent_gui_update(key, now);
		}
	}

	return changed;
}


// FIXME: insteadof this download_gui should pull a listener on 
//        fileinfo status changes, but since the downloads gui
//        has to be overhauled for better fileinfo integration anyway,
//        I didn't do this now.
//     --BLUE, 10/1/2004
#if 0
void gui_update_download_hostcount(struct download *d)
{
	gpointer key;
	GtkCTreeNode *parent;
    GtkCTree *ctree_downloads = GTK_CTREE
            (lookup_widget(main_window, "ctree_downloads"));

    if (NULL != d->file_info) {

		key = &d->file_info->fi_handle;
		parent = find_parent_with_fi_handle(parents, key);

		if (NULL != parent) {
            guint32 n;

			n = count_node_children(ctree_downloads, parent);
			gm_snprintf(tmpstr, sizeof(tmpstr), 
                "%u hosts", n);

			gtk_ctree_node_set_text(ctree_downloads,  parent,
                c_dl_host, tmpstr);
        }
    }
}
#endif	/* 0 */

/*
 * downloads_gui_init
 *
 * Initialize local data structures.
 */
void downloads_gui_init(void)
{
    GtkCList *clist_queue;
    GtkCList *clist;

	ctree_downloads = GTK_CTREE
		(lookup_widget(main_window, "ctree_downloads"));
	ctree_downloads_queue = GTK_CTREE
		(lookup_widget(main_window, "ctree_downloads_queue"));

	clist = GTK_CLIST(ctree_downloads);
	clist_queue = GTK_CLIST(ctree_downloads_queue);

    gtk_clist_column_titles_passive(clist);
    gtk_clist_column_titles_passive(clist_queue);

    gtk_clist_set_column_justification(
        clist_queue, c_queue_size, GTK_JUSTIFY_RIGHT);
    gtk_clist_set_column_justification(
        clist, c_queue_size, GTK_JUSTIFY_RIGHT);

	parents = g_hash_table_new(g_int_hash, g_int_equal);
	parents_queue = g_hash_table_new(g_int_hash, g_int_equal);
	parents_gui_time = g_hash_table_new(g_int_hash, g_int_equal);
	parents_children = g_hash_table_new(g_int_hash, g_int_equal);
	parents_queue_children = g_hash_table_new(g_int_hash, g_int_equal);
}

/*
 * downloads_gui_shutdown
 *
 * Cleanup local data structures.
 */
void downloads_gui_shutdown(void)
{
	g_hash_table_foreach_remove(parents, downloads_gui_free_parent, NULL);
	g_hash_table_foreach_remove(parents_queue, downloads_gui_free_parent, NULL);
	g_hash_table_foreach_remove(parents_gui_time,
		downloads_gui_free_parent, NULL);
	g_hash_table_foreach_remove(parents_children,
		downloads_gui_free_parent, NULL);
	g_hash_table_foreach_remove(parents_queue_children,
		downloads_gui_free_parent, NULL);

	g_hash_table_destroy(parents);
	g_hash_table_destroy(parents_queue);
	g_hash_table_destroy(parents_gui_time);
	g_hash_table_destroy(parents_children);
	g_hash_table_destroy(parents_queue_children);
}

/*
 *	download_gui_add
 *
 *	Adds a download to the gui.  All parenting (grouping) is done here
 */
void download_gui_add(struct download *d)
{
	const gchar *UNKNOWN_SIZE_STR = "unknown size";
	const gchar *titles[6], *titles_parent[6];
	GtkCTreeNode *new_node, *parent;
	GdkColor *color;
	gchar vendor[256];
	const gchar *file_name, *filename;
	gchar *size, *host, *range, *server, *status;
	struct download *drecord;
	gpointer key;
	gint n;

	g_return_if_fail(d);
	
	if (DOWNLOAD_IS_VISIBLE(d)) {
		g_warning
			("download_gui_add() called on already visible download '%s' !",
			 d->file_name);
		return;
	}

	/*
	 * When `record_index' is URN_INDEX, the `file_name' is the URN, which
	 * is not something really readable.  Better display the target filename
	 * on disk in that case.
	 *		--RAM, 22/10/2002
	 */

	file_name = file_info_readable_filename(d->file_info);

	gm_snprintf(vendor, sizeof(vendor), "%s%s",
		(d->server->attrs & DLS_A_BANNING) ? "*" : "",
		download_vendor_str(d));

	color = &(gtk_widget_get_style(GTK_WIDGET(ctree_downloads))
				->fg[GTK_STATE_INSENSITIVE]);

	titles[c_queue_filename] = file_name;
    titles[c_queue_server] = vendor;
	titles[c_queue_status] = "";

	if (d->file_info->file_size_known)
		titles[c_queue_size] = short_size(d->file_info->size);
	else
		titles[c_queue_size] = UNKNOWN_SIZE_STR;				

	titles[c_queue_host] = download_get_hostname(d);


	if (DOWNLOAD_IS_QUEUED(d)) {
		if (NULL != d->file_info) {
			key = (gpointer) &d->file_info->fi_handle;
			parent = find_parent_with_fi_handle(parents_queue, key);
			if (NULL != parent) {
				/* 	There already is a download with that file_info
				 *	we need to figure out if there is a header entry yet
				 */
				drecord = gtk_ctree_node_get_row_data(ctree_downloads_queue, 
					parent);		

				if (DL_GUI_IS_HEADER != drecord)/*not a header entry*/
				{
					/* No header entry so we will create one */
					/* Copy the old parents info into a new node */
					
					filename = file_info_readable_filename(drecord->file_info);
					gtk_ctree_node_get_text(ctree_downloads_queue, parent,
						c_queue_host, &host);
					gtk_ctree_node_get_text(ctree_downloads_queue, parent,
						c_queue_size, &size);
					gtk_ctree_node_get_text(ctree_downloads_queue, parent,
						c_queue_server, &server);
					gtk_ctree_node_get_text(ctree_downloads_queue, parent,
						c_queue_status, &status);
					
					titles_parent[c_queue_filename] = filename;
			        titles_parent[c_queue_server] = server;
       				titles_parent[c_queue_status] = status;
					titles_parent[c_queue_size] = "\"";
        			titles_parent[c_queue_host] = host;
					
					new_node = gtk_ctree_insert_node(ctree_downloads_queue, 
						parent, NULL,
						(gchar **) titles_parent, /* Override const */
						DL_GUI_TREE_SPACE, NULL, NULL, NULL, NULL,
						FALSE, FALSE);

					parent_children_add(ctree_downloads_queue, key, 1);
	
					gtk_ctree_node_set_row_data(ctree_downloads_queue, new_node, 
						(gpointer) drecord);

					if (drecord->always_push)
						 gtk_ctree_node_set_foreground(ctree_downloads_queue, 
							new_node, color);

					/* Clear old values in parent, turn it into a header */
					gtk_ctree_node_set_text(ctree_downloads_queue, parent, 
						c_queue_filename, filename);
					gtk_ctree_node_set_text(ctree_downloads_queue, parent, 
						c_queue_size, size);
					gtk_ctree_node_set_text(ctree_downloads_queue, parent, 
						c_queue_server, "");
					gtk_ctree_node_set_text(ctree_downloads_queue, parent, 
						c_queue_status, "");
						
					gtk_ctree_node_set_row_data(ctree_downloads_queue, parent, 
						DL_GUI_IS_HEADER);						
				}

				/*
				 * Whether we just created the header node or one existed
				 * already, we proceed the same.  Namely, by adding the current 
				 * download `d' into a new child node and then updating the
				 * header entry
				 */

				/*
				 * It's a child node so we suppress some extraneous column
				 * text to make the gui more readable
				 */
				titles[c_queue_size] = "\"";

				new_node = gtk_ctree_insert_node(ctree_downloads_queue, 
						parent, NULL,
						(gchar **) titles, /* Override const */
						DL_GUI_TREE_SPACE, NULL, NULL, NULL, NULL,
						FALSE, FALSE);
	
				gtk_ctree_node_set_row_data(ctree_downloads_queue, new_node, 
						(gpointer) d);

				if (d->always_push)
					 gtk_ctree_node_set_foreground(ctree_downloads_queue, 
						new_node, color);
				
				n = parent_children_add(ctree_downloads_queue, key, 1);
				gm_snprintf(tmpstr, sizeof(tmpstr), "%u hosts", n);

				gtk_ctree_node_set_text(ctree_downloads_queue, parent, 
					c_queue_host, tmpstr);
					
			} else {
				/*  There are no other downloads with the same file_info
				 *  Add download as normal
				 *
				 *  Later when we remove this from the parents hash_table
				 *  the file_info atom will be destroyed.  We can leave it
				 *  for now.
				 */
				new_node = gtk_ctree_insert_node(ctree_downloads_queue, 
						NULL, NULL,
						(gchar **) titles, /* Override const */
						DL_GUI_TREE_SPACE, NULL, NULL, NULL, NULL,
						FALSE, FALSE);	
				gtk_ctree_node_set_row_data(ctree_downloads_queue, new_node, 
						(gpointer) d);
				if (d->always_push)
					 gtk_ctree_node_set_foreground(ctree_downloads_queue, 
						new_node, color);
				add_parent_with_fi_handle(parents_queue, key, new_node);
			}
		} 
	} else {					/* This is an active download */

		titles[c_dl_filename] = file_name;
		titles[c_dl_server] = vendor;
		titles[c_dl_status] = "";

		if (d->file_info->file_size_known)
			titles[c_dl_size] = short_size(d->file_info->size);
		else
			titles[c_dl_size] = UNKNOWN_SIZE_STR;				
		titles[c_dl_range] = "";
        titles[c_dl_host] = download_get_hostname(d);
		
		if (NULL != d->file_info) {
			key = (gpointer) &d->file_info->fi_handle;
			parent = find_parent_with_fi_handle(parents, key);
			if (NULL != parent) {
				/* 	There already is a download with that file_info
				 *	we need to figure out if there is a header entry yet
				 */
				drecord = gtk_ctree_node_get_row_data(ctree_downloads, 
					parent);		

				if (DL_GUI_IS_HEADER != drecord) {
					/* No header entry so we will create one */
					/* Copy the old parents info into a new node */
					
					filename = file_info_readable_filename(drecord->file_info);
					gtk_ctree_node_get_text(ctree_downloads, parent,
						c_dl_host, &host);
					gtk_ctree_node_get_text(ctree_downloads, parent,
						c_dl_size, &size);
					gtk_ctree_node_get_text(ctree_downloads, parent,
						c_dl_server, &server);
					gtk_ctree_node_get_text(ctree_downloads, parent,
						c_dl_status, &status);
					gtk_ctree_node_get_text(ctree_downloads, parent,
						c_dl_range, &range);
					
					titles_parent[c_dl_filename] = filename;
			        titles_parent[c_dl_server] = server;
       				titles_parent[c_dl_status] = status;
					titles_parent[c_dl_size] = "\"";
					titles_parent[c_dl_host] = host;
        			titles_parent[c_dl_range] = range;

					new_node = gtk_ctree_insert_node(ctree_downloads, 
						parent, NULL,
						(gchar **) titles_parent, /* Override const */
						DL_GUI_TREE_SPACE, NULL, NULL, NULL, NULL,
						FALSE, FALSE);

					parent_children_add(ctree_downloads, key, 1);
 
					gtk_ctree_node_set_row_data(ctree_downloads, new_node, 
						(gpointer) drecord);

					if (DOWNLOAD_IS_IN_PUSH_MODE(d))
						 gtk_ctree_node_set_foreground(ctree_downloads, 
							new_node, color);

					/* Clear old values in parent, turn it into a header */
					gtk_ctree_node_set_text(ctree_downloads, parent, 
						c_dl_filename, filename);
					gtk_ctree_node_set_text(ctree_downloads, parent, 
						c_dl_size, size);
					gtk_ctree_node_set_text(ctree_downloads, parent, 
						c_dl_server, "");
					gtk_ctree_node_set_text(ctree_downloads, parent, 
						c_dl_status, "");
					gtk_ctree_node_set_text(ctree_downloads, parent, 
						c_dl_range, "");
						
					gtk_ctree_node_set_row_data(ctree_downloads, parent, 
						DL_GUI_IS_HEADER);						
				}

				/*
				 * Whether we just created the header node or one existed
				 * already, we proceed the same.  Namely, by adding the current 
				 * download `d' into a new child node and then updating the
				 * header entry
				 */

				/* It's a child node so we suppress some extraneous column
				 * text to make the gui more readable
				 */
				titles[c_dl_size] = "\"";

				new_node = gtk_ctree_insert_node(ctree_downloads, 
						parent, NULL,
						(gchar **) titles, /* Override const */
						DL_GUI_TREE_SPACE, NULL, NULL, NULL, NULL,
						FALSE, FALSE);
	
				gtk_ctree_node_set_row_data(ctree_downloads, new_node, 
						(gpointer) d);

				if (DOWNLOAD_IS_IN_PUSH_MODE(d))
					 gtk_ctree_node_set_foreground(ctree_downloads, 
						new_node, color);
				
				n = parent_children_add(ctree_downloads, key, 1);
				gm_snprintf(tmpstr, sizeof(tmpstr), "%u hosts", n);

				gtk_ctree_node_set_text(ctree_downloads, parent, 
					c_queue_host, tmpstr);

			} else {
				/*  There are no other downloads with the same file_info
				 *  Add download as normal
				 *
				 *  Later when we remove this from the parents hash_table
				 *  the file_info atom will be destroyed.  We can leave it
				 *  for now.
				 */
				new_node = gtk_ctree_insert_node(ctree_downloads, 
						NULL, NULL,
						(gchar **) titles, /* Override const */
						DL_GUI_TREE_SPACE, NULL, NULL, NULL, NULL,
						FALSE, FALSE);	
				gtk_ctree_node_set_row_data(ctree_downloads, new_node, 
						(gpointer) d);
				if (DOWNLOAD_IS_IN_PUSH_MODE(d))
					 gtk_ctree_node_set_foreground(ctree_downloads, 
						new_node, color);
				add_parent_with_fi_handle(parents, key, new_node);
			}
		} 
	}

	d->visible = TRUE;
}


void gui_update_download_server(struct download *d)
{
	GtkCTreeNode *node;

	if (DL_GUI_IS_HEADER == d)
		return;			/* A header was sent here by mistake */ 		

	g_assert(d);
	g_assert(d->status != GTA_DL_QUEUED);
	g_assert(d->server);
	g_assert(download_vendor(d));

	/*
	 * Prefix vendor name with a '*' if they are considered as potentially
	 * banning us and we activated anti-banning features.
	 *		--RAM, 05/07/2003
	 */

	(void) gm_snprintf(tmpstr, sizeof(tmpstr), "%s%s",
		(d->server->attrs & DLS_A_BANNING) ? "*" : "",
		download_vendor(d));

	node = gtk_ctree_find_by_row_data(ctree_downloads, NULL, (gpointer) d);
	if (NULL != node)
		gtk_ctree_node_set_text(ctree_downloads, node, c_dl_server, tmpstr);
}

void gui_update_download_range(struct download *d)
{
	guint32 len;
	gchar *and_more = "";
	gint rw;
	GtkCTreeNode *node;

	g_assert(d);
	g_assert(d->status != GTA_DL_QUEUED);

	if (DL_GUI_IS_HEADER == d)
		return;			/* A header was sent here by mistake */ 		

	if (d->file_info->use_swarming) {
		len = d->size;
		if (d->range_end > d->skip + d->size)
			and_more = "+";
		if (d->flags & DL_F_SHRUNK_REPLY)		/* Chunk shrunk by server! */
			and_more = "-";
	} else
		len = d->range_end - d->skip;

	len += d->overlap_size;

	rw = gm_snprintf(tmpstr, sizeof(tmpstr), "%s%s",
		compact_size(len), and_more);

	if (d->skip)
		gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw, " @ %s",
			compact_size(d->skip));

	node = gtk_ctree_find_by_row_data(ctree_downloads, NULL, (gpointer) d);
	if (NULL != node)
		gtk_ctree_node_set_text(ctree_downloads, node, c_dl_range, tmpstr);
}

void gui_update_download_host(struct download *d)
{
	GtkCTreeNode *node;

	if (DL_GUI_IS_HEADER == d)
		return;			/* A header was sent here by mistake */ 		

	g_assert(d);
	g_assert(d->status != GTA_DL_QUEUED);

	node = gtk_ctree_find_by_row_data(ctree_downloads, NULL, (gpointer) d);
	if (NULL != node)
		gtk_ctree_node_set_text(ctree_downloads, node,
			c_dl_host, download_get_hostname(d));
}

void gui_update_download(struct download *d, gboolean force)
{
	const gchar *a = NULL;
	time_t now = time((time_t *) NULL);
    GdkColor *color;
	GtkCTreeNode *node, *parent;
	struct download *drecord;
	struct dl_file_info *fi;
	gpointer key;	
	gint active_src, tot_src;
	gfloat percent_done =0;
	guint32 s = 0;
	gfloat bs = 0;

	gint rw;
	extern gint sha1_eq(gconstpointer a, gconstpointer b);
    gint current_page;
	static GtkNotebook *notebook = NULL;
	static GtkNotebook *dl_notebook = NULL;
	gboolean looking = TRUE;

    if (d->last_gui_update == now && !force)
		return;

	if (DL_GUI_IS_HEADER == d)
		return;			/* A header was sent here by mistake */ 		

	/*
	 * Why update if no one's looking?
	 *
	 * We must update some of the download entries even if nobody is
	 * looking because we don't periodically update the GUI for all the
	 * states...
	 */

	if (notebook == NULL)
		notebook = GTK_NOTEBOOK(lookup_widget(main_window, "notebook_main"));

	if (dl_notebook == NULL)
		dl_notebook =
			GTK_NOTEBOOK(lookup_widget(main_window, "notebook_downloads"));

    current_page = gtk_notebook_get_current_page(notebook);
    if (current_page != nb_main_page_downloads)
        looking = FALSE;

	if (looking) {
		current_page = gtk_notebook_get_current_page(dl_notebook);
		if (current_page != nb_downloads_page_downloads)
			looking = FALSE;
	}

	if (!looking) {
		switch (d->status) {
		case GTA_DL_ACTIVE_QUEUED:
		case GTA_DL_RECEIVING:
		case GTA_DL_HEADERS:
		case GTA_DL_PUSH_SENT:
		case GTA_DL_CONNECTING:
		case GTA_DL_REQ_SENDING:
		case GTA_DL_REQ_SENT:
		case GTA_DL_FALLBACK:
		case GTA_DL_SINKING:
		case GTA_DL_TIMEOUT_WAIT:
		case GTA_DL_VERIFYING:
		case GTA_DL_MOVING:
			return;			/* This will be updated when they look */
		default:
			break;			/* Other states must always be updated */
		}
	}
	
    color = &(gtk_widget_get_style(GTK_WIDGET(ctree_downloads))
        ->fg[GTK_STATE_INSENSITIVE]);

	d->last_gui_update = now;
	fi = d->file_info;

	switch (d->status) {
	case GTA_DL_ACTIVE_QUEUED:	/* JA, 31 jan 2003 Active queueing */
		{
			gint elapsed = delta_time(now, d->last_update);

			rw = gm_snprintf(tmpstr, sizeof(tmpstr), "Queued");

			if (get_parq_dl_position(d) > 0) {

				rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
					" (slot %d",		/* ) */
					get_parq_dl_position(d));
				
				if (get_parq_dl_queue_length(d) > 0) {
					rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
						" / %d", get_parq_dl_queue_length(d));
				}

				if (get_parq_dl_eta(d)  > 0) {
					rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
						", ETA: %s",
						short_time((get_parq_dl_eta(d)  - elapsed)));
				}

				rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw, /* ( */ ")");
			}

			rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
					" retry in %ds",
					(gint) (get_parq_dl_retry_delay(d) - elapsed));

			if (
				parent_gui_needs_update(d, now) &&
				!downloads_gui_any_status(d, GTA_DL_RECEIVING)
			)
				downloads_gui_update_parent_status(d, now, "Queued");
		}
		a = tmpstr;
		break;
	case GTA_DL_QUEUED:
		a = d->remove_msg ? d->remove_msg : "";
		break;

	case GTA_DL_CONNECTING:
		a = "Connecting...";
		if (
			parent_gui_needs_update(d, now) &&
			!downloads_gui_any_status(d, GTA_DL_RECEIVING) &&
			!downloads_gui_any_status(d, GTA_DL_ACTIVE_QUEUED)
		)
			downloads_gui_update_parent_status(d, now, "Connecting...");
		break;

	case GTA_DL_PUSH_SENT:
	case GTA_DL_FALLBACK:
		{
			if (d->cproxy != NULL) {
				const struct cproxy *cp = d->cproxy;

				if (cp->done) {
					if (cp->sent)
						rw = gm_snprintf(tmpstr, sizeof(tmpstr),
							"Push sent%s", cp->directly ? " directly" : "");
					else
						rw = gm_snprintf(tmpstr, sizeof(tmpstr),
							"Failed to send push");
				} else
					rw = gm_snprintf(tmpstr, sizeof(tmpstr),
						"Sending push");
				
				rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw, " via %s",
						ip_port_to_gchar(cproxy_ip(cp), cproxy_port(cp)));

				if (!cp->done) {
					switch (cp->state) {
					case HTTP_AS_CONNECTING:	a = "Connecting"; break;
					case HTTP_AS_REQ_SENDING:	a = "Sending request"; break;
					case HTTP_AS_REQ_SENT:		a = "Request sent"; break;
					case HTTP_AS_HEADERS:		a = "Reading headers"; break;
					default:					a = "..."; break;
					}

					rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
						": %s", a);
				}

				a = tmpstr;
			} else {
				switch (d->status) {
				case GTA_DL_PUSH_SENT:
					a = "Push sent";
					if (
						parent_gui_needs_update(d, now) &&
						!downloads_gui_any_status(d, GTA_DL_RECEIVING) &&
						!downloads_gui_any_status(d, GTA_DL_ACTIVE_QUEUED) &&
						!downloads_gui_any_status(d, GTA_DL_CONNECTING)
						)
						downloads_gui_update_parent_status
							(d, now, "Push sent");
					break;
				case GTA_DL_FALLBACK:
					a = "Falling back to push";
					break;
				default:
					break;
				}
			}
		}
		break;

	case GTA_DL_REQ_SENDING:
		if (d->req != NULL) {
			http_buffer_t *r = d->req;
			gint pct = (http_buffer_read_base(r) - http_buffer_base(r))
				* 100 / http_buffer_length(r);
			gm_snprintf(tmpstr, sizeof(tmpstr), "Sending request (%d%%)", pct);
			a = tmpstr;
		} else
			a = "Sending request";
		break;

	case GTA_DL_REQ_SENT:
		a = "Request sent";
		break;

	case GTA_DL_HEADERS:
		a = "Receiving headers";
		break;

	case GTA_DL_ABORTED:
		a = d->unavailable ? "Aborted (Server down)" : "Aborted";

		/*
		 * If this download is aborted, it's possible all the downloads in this
	     * parent node (if there is one) are aborted too. If so, update parent
		 */

		if (downloads_gui_all_aborted(d))
			downloads_gui_update_parent_status(d, now, "Aborted");

		break;

	case GTA_DL_COMPLETED:
		if (d->last_update != d->start_date) {
			guint32 spent = d->last_update - d->start_date;

			gfloat rate = ((d->range_end - d->skip + d->overlap_size) /
				1024.0) / spent;
			gm_snprintf(tmpstr, sizeof(tmpstr), "%s (%.1f k/s) %s",
				FILE_INFO_COMPLETE(fi) ? "Completed" : "Chunk done",
				rate, short_time(spent));
		} else {
			gm_snprintf(tmpstr, sizeof(tmpstr), "%s (< 1s)",
				FILE_INFO_COMPLETE(fi) ? "Completed" : "Chunk done");
		}
		a = tmpstr;
		break;

	case GTA_DL_VERIFY_WAIT:
		g_assert(FILE_INFO_COMPLETE(fi));
		g_strlcpy(tmpstr, "Waiting for SHA1 checking...", sizeof(tmpstr));
		a = tmpstr;
		break;

	case GTA_DL_VERIFYING:
		g_assert(FILE_INFO_COMPLETE(fi));
		gm_snprintf(tmpstr, sizeof(tmpstr),
			"Computing SHA1 (%.02f%%)", fi->cha1_hashed * 100.0 / fi->size);
		a = tmpstr;
		break;

	case GTA_DL_VERIFIED:
	case GTA_DL_MOVE_WAIT:
	case GTA_DL_MOVING:
	case GTA_DL_DONE:
		g_assert(FILE_INFO_COMPLETE(fi));
		g_assert(fi->cha1_hashed <= fi->size);
		{
			gboolean sha1_ok = fi->cha1 &&
				(fi->sha1 == NULL || sha1_eq(fi->sha1, fi->cha1));

			rw = gm_snprintf(tmpstr, sizeof(tmpstr), "SHA1 %s %s",
				fi->sha1 == NULL ? "figure" : "check",
				fi->cha1 == NULL ?	"ERROR" :
				sha1_ok ?			"OK" :
									"FAILED");
			if (fi->cha1 && fi->cha1_hashed) {
				guint elapsed = fi->cha1_elapsed;
				rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
					" (%.1f k/s) %s",
					(gfloat) (fi->cha1_hashed >> 10) / (elapsed ? elapsed : 1),
					short_time(fi->cha1_elapsed));
			}

			switch (d->status) {
			case GTA_DL_MOVE_WAIT:
				g_strlcpy(&tmpstr[rw], "; Waiting for moving...",
					sizeof(tmpstr)-rw);
				break;
			case GTA_DL_MOVING:
				gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
					"; Moving (%.02f%%)", fi->copied * 100.0 / fi->size);
				break;
			case GTA_DL_DONE:
				if (fi->copy_elapsed) {
					gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
						"; Moved (%.1f k/s) %s",
						(gfloat) (fi->copied >> 10) / fi->copy_elapsed,
						short_time(fi->copy_elapsed));
				}
				break;
			default:
				break;
			}
		}
		a = tmpstr;
		break;

	case GTA_DL_RECEIVING:
		if (d->pos - d->skip > 0) {
			gfloat p = 0, pt = 0;
			gint bps;
			guint32 avg_bps;

			if (d->size)
                p = (d->pos - d->skip) * 100.0 / d->size;
            if (download_filesize(d))
                pt = download_filedone(d) * 100.0 / download_filesize(d);

			bps = bio_bps(d->bio);
			avg_bps = bio_avg_bps(d->bio);

			if (avg_bps <= 10 && d->last_update != d->start_date)
				avg_bps = (d->pos - d->skip) / (d->last_update - d->start_date);

			rw = 0;

			if (avg_bps) {
				guint32 remain = 0;
				guint32 s;
				gfloat bs;

                if (d->size > (d->pos - d->skip))
                    remain = d->size - (d->pos - d->skip);

                s = remain / avg_bps;
				bs = bps / 1024.0;

				rw = gm_snprintf(tmpstr, sizeof(tmpstr),
					"%.02f%% / %.02f%% ", p, pt);

				if (now - d->last_update > IO_STALLED)
					rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
						"(stalled) ");
				else
					rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
						"(%.1f k/s) ", bs);

				rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
					"[%d/%d] TR: %s", fi->recvcount, fi->lifecount,
					s ? short_time(s) : "-");

				if (fi->recv_last_rate) {
					s = (fi->size - fi->done) / fi->recv_last_rate;

					rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
						" / %s", short_time(s));

					if (fi->recvcount > 1) {
						bs = fi->recv_last_rate / 1024.0;

						rw += gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
							" (%.1f k/s)", bs);
					}
				}
			} else
				rw = gm_snprintf(tmpstr, sizeof(tmpstr), "%.02f%%%s", p,
					(now - d->last_update > IO_STALLED) ? " (stalled)" : "");

			/*
			 * If source is a partial source, show it.
			 */

			if (d->ranges != NULL)
				gm_snprintf(&tmpstr[rw], sizeof(tmpstr)-rw,
					" <PFS %.02f%%>", d->ranges_size * 100.0 / fi->size);

			a = tmpstr;
		} else
			a = "Connected";
		break;

	case GTA_DL_ERROR:
		a = d->remove_msg ? d->remove_msg : "Unknown Error";
		break;

	case GTA_DL_TIMEOUT_WAIT:
		{
			gint when = d->timeout_delay - (now - d->last_update);
			gm_snprintf(tmpstr, sizeof(tmpstr), "Retry in %ds", MAX(0, when));
		}
		a = tmpstr;
		break;
	case GTA_DL_SINKING:
		gm_snprintf(tmpstr, sizeof(tmpstr), "Sinking (%u bytes left)",
			d->sinkleft);
		a = tmpstr;
		break;
	default:
		gm_snprintf(tmpstr, sizeof(tmpstr), "UNKNOWN STATUS %u",
				   d->status);
		a = tmpstr;
	}

	if (d->status != GTA_DL_TIMEOUT_WAIT)
		d->last_gui_update = now;

    if (d->status == GTA_DL_QUEUED) {
		node = gtk_ctree_find_by_row_data(ctree_downloads_queue, 
			NULL, (gpointer) d);

		if (NULL != node) {
			gtk_ctree_node_set_text(ctree_downloads_queue, node, 
				c_queue_status, a);
        	if (d->always_push)
        	     gtk_ctree_node_set_foreground(ctree_downloads_queue, 
					node, color);
		}
		
		/*  Update header for downloads with multiple hosts */
		if (NULL != d->file_info) {
		
			key = &d->file_info->fi_handle;
			parent = find_parent_with_fi_handle(parents_queue, key);

			if (parent != NULL) {

				drecord = gtk_ctree_node_get_row_data(ctree_downloads_queue, 
					parent);		

				if (DL_GUI_IS_HEADER == drecord) {
					/* There is a header entry, we need to update it */
					
					/* Download is done */
					if (GTA_DL_DONE == d->status) {						

						gm_snprintf(tmpstr, sizeof(tmpstr),
							"Complete");
						gtk_ctree_node_set_text(ctree_downloads_queue, parent, 
							c_queue_status, tmpstr);
						
					} else {
						if ((GTA_DL_RECEIVING == d->status) && 
							(d->pos - d->skip > 0)) {

							percent_done = 0;
							s = 0;
							bs = 0;

	        			    if (download_filesize(d))
		                		percent_done = ((download_filedone(d) * 100.0) 
									/ download_filesize(d));

							active_src = fi->recvcount;
							tot_src = fi->lifecount;

							if (fi->recv_last_rate)
								s = (fi->size - fi->done) / fi->recv_last_rate;	
							bs = fi->recv_last_rate / 1024;

							if (s)
								gm_snprintf(tmpstr, sizeof(tmpstr),
						"%.02f%%  (%.1f k/s)  [%d/%d]  TR:  %s", percent_done,
									bs, active_src, tot_src, short_time(s));
							else
								gm_snprintf(tmpstr, sizeof(tmpstr),
						"%.02f%%  (%.1f k/s)  [%d/%d]  TR:  -", percent_done,
									bs, active_src, tot_src);
						
							gtk_ctree_node_set_text(ctree_downloads_queue, 
								parent, c_queue_status, tmpstr);
						}
					}
				}	
			}			
		}	
	} else {  /* Is an active downloads */

		node = gtk_ctree_find_by_row_data(ctree_downloads, NULL, (gpointer) d);

		/* Update status column */
		if (NULL != node) {
			gtk_ctree_node_set_text(ctree_downloads, node, c_dl_status, a);
    	    if (DOWNLOAD_IS_IN_PUSH_MODE(d))
        	     gtk_ctree_node_set_foreground(ctree_downloads, node, color);
		}
		
		/*  Update header for downloads with multiple hosts */
		if (NULL != d->file_info) {
		
			key = &d->file_info->fi_handle;
			parent = find_parent_with_fi_handle(parents, key);

			if (
				parent != NULL &&
				now != get_last_parent_gui_update(key)
			) {
				drecord = gtk_ctree_node_get_row_data(ctree_downloads, 
					parent);		

				if (DL_GUI_IS_HEADER == drecord) {
					/* There is a header entry, we need to update it */
					
					/* Download is done */
					if (GTA_DL_DONE == d->status) {						

						gm_snprintf(tmpstr, sizeof(tmpstr),
							"Complete");
						gtk_ctree_node_set_text(ctree_downloads, parent, 
							c_dl_status, tmpstr);
						record_parent_gui_update(key, now);
						
					} else {
						if ((GTA_DL_RECEIVING == d->status) && 
							(d->pos - d->skip > 0)) {

							percent_done = 0;
							s = 0;
							bs = 0;

	        			    if (download_filesize(d))
		                		percent_done = ((download_filedone(d) * 100.0) 
									/ download_filesize(d));

							active_src = fi->recvcount;
							tot_src = fi->lifecount;

							if (fi->recv_last_rate)
								s = (fi->size - fi->done) / fi->recv_last_rate;	
							bs = fi->recv_last_rate / 1024;

							if (s)
								gm_snprintf(tmpstr, sizeof(tmpstr),
						"%.02f%%  (%.1f k/s)  [%d/%d]  TR:  %s", percent_done,
									bs, active_src, tot_src, short_time(s));
							else
								gm_snprintf(tmpstr, sizeof(tmpstr),
						"%.02f%%  (%.1f k/s)  [%d/%d]  TR:  -", percent_done,
									bs, active_src, tot_src);
						
							gtk_ctree_node_set_text(ctree_downloads, 
								parent, c_dl_status, tmpstr);
							record_parent_gui_update(key, now);
						}
					}
				}	
			}
		}	
	}
}


void gui_update_download_abort_resume(void)
{
   	struct download *d;
    GList *node_list;
    GList *data_list;
    GList *l;

	gboolean do_abort  = FALSE;
    gboolean do_resume = FALSE;
    gboolean do_remove = FALSE;
    gboolean do_queue  = FALSE;
    gboolean abort_sha1 = FALSE;

    node_list = g_list_copy(GTK_CLIST(ctree_downloads)->selection);
	data_list = downloads_gui_collect_ctree_data(ctree_downloads, 
		node_list, FALSE, TRUE);
	
    for (l = data_list; NULL != l; l = g_list_next(l)) {
		d = (struct download *) l->data;
	
        if (!d) {
			g_warning
				("gui_update_download_abort_resume(): row has NULL data\n");
			continue;
		}

		g_assert(d->status != GTA_DL_REMOVED);

		switch (d->status) {
		case GTA_DL_COMPLETED:
		case GTA_DL_VERIFY_WAIT:
		case GTA_DL_VERIFYING:
		case GTA_DL_VERIFIED:
			break;
		default:
			do_queue = TRUE;
			break;
		}

        if (d->file_info->sha1 != NULL)
            abort_sha1 = TRUE;

		switch (d->status) {
		case GTA_DL_QUEUED:
			g_warning("gui_update_download_abort_resume(): "
				"found queued download '%s' in active download list !",
				d->file_name);
			continue;
		case GTA_DL_CONNECTING:
		case GTA_DL_PUSH_SENT:
		case GTA_DL_FALLBACK:
		case GTA_DL_REQ_SENT:
		case GTA_DL_HEADERS:
		case GTA_DL_RECEIVING:
		case GTA_DL_ACTIVE_QUEUED:
		case GTA_DL_SINKING:
			do_abort = TRUE;
			break;
		case GTA_DL_ERROR:
		case GTA_DL_ABORTED:
			do_resume = TRUE;
            /* only check if file exists if really necessary */
            if (!do_remove && download_file_exists(d))
                do_remove = TRUE;
			break;
		case GTA_DL_TIMEOUT_WAIT:
			do_abort = do_resume = TRUE;
			break;
        default:
			break;
		}
		if (do_abort & do_resume & do_remove)
			break;
	}

	g_list_free(data_list);
	g_list_free(node_list);

	gtk_widget_set_sensitive
        (lookup_widget(main_window, "button_downloads_abort"), do_abort);
	gtk_widget_set_sensitive
        (lookup_widget(popup_downloads, "popup_downloads_abort"), do_abort);
    gtk_widget_set_sensitive
        (lookup_widget(popup_downloads, "popup_downloads_abort_named"),
		do_abort);
    gtk_widget_set_sensitive
        (lookup_widget(popup_downloads, "popup_downloads_abort_host"),
		do_abort);
    gtk_widget_set_sensitive(
        lookup_widget(popup_downloads, "popup_downloads_abort_sha1"), 
        abort_sha1);
	gtk_widget_set_sensitive
        (lookup_widget(main_window, "button_downloads_resume"), do_resume);
	gtk_widget_set_sensitive
        (lookup_widget(popup_downloads, "popup_downloads_resume"), do_resume);
    gtk_widget_set_sensitive
        (lookup_widget(popup_downloads, "popup_downloads_remove_file"),
		do_remove);
    gtk_widget_set_sensitive
        (lookup_widget(popup_downloads, "popup_downloads_queue"), do_queue);
}

/*
 * download_gui_remove
 *
 * Remove a download from the GUI.
 */
void download_gui_remove(struct download *d)
{
	GtkCTreeNode *node, *parent;
	GtkCTreeRow *parent_row;
	struct download *drecord;
	gchar *host, *range, *server, *status;
	const gchar *filename;
	gpointer key;
	gint n;
	
	g_return_if_fail(d);
	
	if (!DOWNLOAD_IS_VISIBLE(d)) {
		g_warning
			("download_gui_remove() called on invisible download '%s' !",
			 d->file_name);
		return;
	}

	
	if (DOWNLOAD_IS_QUEUED(d)) {
		node = gtk_ctree_find_by_row_data(ctree_downloads_queue, 
			NULL, (gpointer) d);

		if (NULL != node) {
			/*  We need to discover if the download has a parent */
			if (NULL != d->file_info) {
		
				key = &d->file_info->fi_handle;
				parent =  find_parent_with_fi_handle(parents_queue, key);

				if (NULL != parent) {
	
					n = parent_children_add(ctree_downloads_queue, key, 0);
										
					/* If there are children, there should be >1 */
					if (1 == n || n < 0) {
						g_warning("gui_remove_download (queued):" 
							"node has %d children!", n);
						return;
					}

					if (2 == n) {
						/* Removing this download will leave only one left, 
						 * we'll have to get rid of the header. */
				
						/* Get rid of current download, d */
						gtk_ctree_remove_node(ctree_downloads_queue, node);
						parent_children_add(ctree_downloads_queue, key, -1);

						/* Replace header with only remaining child */
						parent_row = GTK_CTREE_ROW(parent);
						node = parent_row->children;			

						drecord = gtk_ctree_node_get_row_data
							(ctree_downloads_queue, node);
						filename = file_info_readable_filename(drecord->file_info);
						gtk_ctree_node_get_text(ctree_downloads_queue, node,
							c_queue_host, &host);
						gtk_ctree_node_get_text(ctree_downloads_queue, node,
							c_queue_server, &server);
						gtk_ctree_node_get_text(ctree_downloads_queue, node,
							c_queue_status, &status);

						gtk_ctree_node_set_row_data
							(ctree_downloads_queue, parent, drecord);
						gtk_ctree_node_set_text(ctree_downloads_queue,  parent,
							c_queue_host, filename);
						gtk_ctree_node_set_text(ctree_downloads_queue,  parent,
							c_queue_host, host);
						gtk_ctree_node_set_text(ctree_downloads_queue,  parent,
							c_queue_server, server);
						gtk_ctree_node_set_text(ctree_downloads_queue,  parent,
							c_queue_status, status);
					}
				
					if (0 == n) {
						/* Node has no children -> is a parent */
						remove_parent_with_fi_handle
							(parents_queue, &(d->file_info->fi_handle));
					}
						
					if (n > 2){
						gm_snprintf(tmpstr, sizeof(tmpstr), "%u hosts", 
                            n - 1);

						gtk_ctree_node_set_text(ctree_downloads_queue,  parent,
							c_queue_host, tmpstr);
					}
			
					/*  Note: this line IS correct for cases n=0, n=2,and n>2 */
					gtk_ctree_remove_node(ctree_downloads_queue, node);
					if (n > 0)
						parent_children_add(ctree_downloads_queue, key, -1);
				
				} else 
					g_warning("download_gui_remove(): "
						"Download '%s' has no parent", d->file_name);
			}
		} else
			g_warning("download_gui_remove(): "
				"Queued download '%s' not found in treeview !?", d->file_name);
		
	} else { /* Removing active download */

		node = gtk_ctree_find_by_row_data(ctree_downloads, NULL, (gpointer) d);

		if (NULL != node) {
			/*  We need to discover if the download has a parent */
			if (NULL != d->file_info) {
		
				key = &d->file_info->fi_handle;
				parent = find_parent_with_fi_handle(parents, key);

				if (NULL != parent) {
	
					n = parent_children_add(ctree_downloads, key, 0);
												
					/* If there are children, there should be >1 */
					if (1 == n || n < 0) {
						g_warning("gui_remove_download (active):" 
							"node has %d children!", n);
						return;						
					}

					if (2 == n) {
						/* Removing this download will leave only one left, 
						 * we'll have to get rid of the header. */
				
						/* Get rid of current download, d */
						gtk_ctree_remove_node(ctree_downloads, node);
						parent_children_add(ctree_downloads, key, -1);

						/* Replace header with only remaining child */
						parent_row = GTK_CTREE_ROW(parent);
						node = parent_row->children;			

						drecord = gtk_ctree_node_get_row_data
							(ctree_downloads, node);
						filename = file_info_readable_filename
							(drecord->file_info);
						gtk_ctree_node_get_text(ctree_downloads, node,
							c_dl_host, &host);
						gtk_ctree_node_get_text(ctree_downloads, node,
							c_dl_server, &server);
						gtk_ctree_node_get_text(ctree_downloads, node,
							c_dl_status, &status);
						gtk_ctree_node_get_text(ctree_downloads, node,
							c_dl_range, &range);

						gtk_ctree_node_set_row_data(ctree_downloads, parent, 
							drecord);
						gtk_ctree_node_set_text(ctree_downloads,  parent,
							c_dl_host, filename);
						gtk_ctree_node_set_text(ctree_downloads,  parent,
							c_dl_host, host);
						gtk_ctree_node_set_text(ctree_downloads,  parent,
							c_dl_server, server);
						gtk_ctree_node_set_text(ctree_downloads,  parent,
							c_dl_status, status);
						gtk_ctree_node_set_text(ctree_downloads,  parent,
							c_dl_range, range);
					}
				
					if (0 == n) {
						/* Node has no children -> is a parent */
						remove_parent_with_fi_handle
							(parents, &(d->file_info->fi_handle));
					}
						
					if (2 < n){
						gm_snprintf(tmpstr, sizeof(tmpstr), 
                            "%u hosts", n-1);

						gtk_ctree_node_set_text(ctree_downloads,  parent,
							c_dl_host, tmpstr);
					}
			
					/*  Note: this line IS correct for cases n=0, n=2,and n>2 */
					gtk_ctree_remove_node(ctree_downloads, node);
					if (n > 0)
						parent_children_add(ctree_downloads, key, -1);

				} else 
					g_warning("download_gui_remove(): "
						"Active download '%s' has no parent", d->file_name);
			}	
		} else
			g_warning("download_gui_remove(): "
				"Active download '%s' not found in treeview!?",  d->file_name);
	}

	d->visible = FALSE;

	gui_update_download_abort_resume();
	gui_update_download_clear();
}

/*
 *	downloads_gui_collapse_all
 *
 *	Collapse all nodes in given, tree either downloads or downloads_queue
 */
void downloads_gui_expand_all(GtkCTree *ctree)
{	
	gtk_ctree_expand_recursive(ctree, NULL);
}


/*
 *	downloads_gui_collapse_all
 *
 *	Collapse all nodes in given, tree either downloads or downloads_queue
 */
void downloads_gui_collapse_all(GtkCTree *ctree)
{
	gtk_ctree_collapse_recursive(ctree, NULL);
}

/* vi: set ts=4: */
#endif
