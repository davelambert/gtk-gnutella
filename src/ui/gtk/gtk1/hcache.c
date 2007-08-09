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

#include "common.h"

RCSID("$Id$")

#include "gtk/gui.h"

#include "gtk/columns.h"
#include "gtk/hcache.h"
#include "gtk/misc.h"
#include "gtk/notebooks.h"

#include "if/gui_property.h"
#include "if/bridge/ui2c.h"

#include "lib/glib-missing.h"
#include "lib/override.h"	/* Must be the last header included */

/***
 *** Private functions
 ***/
static gchar *
guint_to_str(guint32 i)
{
    static gchar strbuf[UINT32_DEC_BUFLEN];

    gm_snprintf(strbuf, sizeof(strbuf), "%u", i);
    return strbuf;
}



/***
 *** Public functions
 ***/

void
hcache_gui_init(void)
{
    GtkCList *clist;
    const gchar *titles[5];
    guint i;

    for (i = 0; i < G_N_ELEMENTS(titles); i++) {
        titles[i] = "-";
	}
    clist = GTK_CLIST(gui_main_window_lookup("clist_hcache"));

    /*
     * Stats can't be sorted: make column headers insensitive.
     */
	gtk_clist_column_titles_passive(clist);

    /*
     * Initialize stats tables.
     */
    for (i = 0; i < HCACHE_MAX; i++) {
        gint row;

		if (i == HCACHE_NONE)
			continue;

        titles[0] = get_hcache_name(i);

	/* Override const */
        row = gtk_clist_append(clist, (gchar **) titles);
        gtk_clist_set_selectable(clist, row, FALSE);
    }

    for (i = 1; i < 4; i++) {
        gtk_clist_set_column_justification(clist, i, GTK_JUSTIFY_RIGHT);
    }
	clist_restore_widths(clist, PROP_HCACHE_COL_WIDTHS);
}

void
hcache_gui_shutdown(void)
{
	clist_save_widths(GTK_CLIST(gui_main_window_lookup("clist_hcache")),
		PROP_HCACHE_COL_WIDTHS);
}

static gboolean
hcache_gui_is_visible(void)
{
	return main_gui_window_visible() &&
		nb_main_page_hostcache == main_gui_notebook_get_page();
}

void
hcache_gui_update(time_t now)
{
	static time_t last_update;
    hcache_stats_t stats[HCACHE_MAX];
    GtkCList *clist;
    gint n;

	if (last_update == now)
		return;
	last_update = now;

	if (!hcache_gui_is_visible())
		return;

    guc_hcache_get_stats(stats);

    clist = GTK_CLIST(gui_main_window_lookup("clist_hcache"));
    gtk_clist_freeze(clist);

    for (n = 0; n < HCACHE_MAX; n ++) {
		if (n == HCACHE_NONE)
			continue;

        gtk_clist_set_text(clist, n,
            c_hcs_host_count, guint_to_str(stats[n].host_count));

        gtk_clist_set_text(clist, n,
            c_hcs_hits, guint_to_str(stats[n].hits));

        gtk_clist_set_text(clist, n,
            c_hcs_misses, guint_to_str(stats[n].misses));
    }

    gtk_clist_thaw(clist);
}

/* vi: set ts=4 sw=4 cindent: */
