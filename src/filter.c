/*
 * Copyright (c) 2001-2002, Raphael Manfredi, Richard Eckart
 *
 * GUI filtering functions.
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

#include <netinet/in.h>
#include <arpa/inet.h>

#include "filter.h"
#include "misc.h"
#include "interface.h"
#include "gtk-missing.h"
#include "search.h"

#define BIT_TO_BOOL(m) ((m == 0) ? FALSE : TRUE)
#define DEFAULT_TARGET (filter_drop)
#define IS_BOUND(r) (r->search != NULL)
#define IS_GLOBAL(r) ((r == filter_global_pre) || (r == filter_global_post))
#define IS_BUILTIN(r) ((r == filter_show) || (r == filter_drop))

typedef struct shadow {
    filter_t *filter;
    GList *current;
    GList *removed;
    GList *added;
    gint32 refcount;
} shadow_t;



/*
 * Private functions prototypes
 */
static gint shadow_filter_eq(const shadow_t *a, const filter_t *b);
static shadow_t *shadow_new(filter_t *s);
static shadow_t *shadow_find(filter_t *s);
static rule_t *filter_get_text_rule();
static rule_t *filter_get_ip_rule();
static rule_t *filter_get_size_rule();
static rule_t *filter_get_jump_rule();
static void filter_set_ruleset(GList *);
static void rule_free(rule_t *f);
gchar *rule_to_gchar(rule_t *f);
static gchar *rule_condition_to_gchar(rule_t *);
static int filter_apply(filter_t *, struct record *rec);
static void on_filter_filters_activate(GtkMenuItem *, gpointer);
static void filter_rebuild_target_combos();

/*
 * Public variables
 */
filter_t *work_filter = NULL;

gchar * rule_text_type_labels[] = {
    "starts with",
    "contains the words",
    "ends with",
    "contains the substring",
    "matches regex"
};



/*
 * Private variables
 */
static GList *shadow_filters = NULL;
static GtkWidget *filter_dialog = NULL;
static gchar f_tmp[1024];

/* not static because needed in search_xml. */
filter_t *filter_drop = NULL;
filter_t *filter_show = NULL;
filter_t *filter_global_pre = NULL;
filter_t *filter_global_post = NULL;
GList *filters = NULL;



/***
 *** Implementation
 ***/
void dump_ruleset(GList *ruleset)
{
    GList *r;
    gint n = 0;

    for (r = ruleset; r != NULL; r=r->next)
        printf("       rule %3d : %s\n", n, rule_to_gchar(r->data));
}

void dump_filter(filter_t *filter)
{
    g_assert(filter != NULL);
    printf("Filter name     : %s\n", filter->name);
    printf("       bound    : %p\n", filter->search);
    printf("       refcount : %d\n", filter->refcount);
    dump_ruleset(filter->ruleset);
}

void dump_shadow(shadow_t *shadow)
{
    g_assert(shadow != NULL);
    printf("Shadow for filt : %s\n", shadow->filter->name);
    printf("       bound    : %p\n", shadow->filter->search);
    printf("       refcount : %d\n", shadow->refcount);
    printf("       flt. ref : %d\n", shadow->filter->refcount);
    printf("  Added:\n");
    dump_ruleset(shadow->added);
    printf("  Removed:\n");
    dump_ruleset(shadow->removed);
    printf("  Current:\n");
    dump_ruleset(shadow->current);
    printf("  Original:\n");
    dump_ruleset(shadow->filter->ruleset);
}

/*
 * shadow_filter_eq:
 *
 * Comparator function to match a shadow and a filter.
 */
static gint shadow_filter_eq(const shadow_t *a, const filter_t *b)
{
    if((a != NULL) && (b != NULL)) {
        if(a->filter == b)
            return 0;
    }

    return 1;
}



/*
 * shadow_find:
 *
 * Get the shadow for the given filter. Returns NULL if the filter
 * does not have a shadow yet.
 */
static shadow_t *shadow_find(filter_t *f)
{
    GList * l;

    g_assert(f != NULL);

    l = g_list_find_custom
        (shadow_filters, f, (GCompareFunc) shadow_filter_eq);

    if (l != NULL) {
        if (dbg >= 6)
            printf("shadow found for: %s\n", f->name);
        return l->data;
    } else {
        if (dbg >= 6)
            printf("no shadow found for: %s\n", f->name);
        return NULL;
    }
}



/*
 * shadow_new:
 *
 * Creates a new shadow for a given filter and registers it with
 * our current shadow list.
 */
static shadow_t *shadow_new(filter_t *f)
{
    shadow_t *shadow;

    g_assert(f != NULL);
    g_assert(f->name != NULL);

    if (dbg >= 6)
        printf("creating shadow for: %s\n", f->name);

    shadow = g_new0(shadow_t, 1);

    shadow->filter   = f;
    shadow->current  = g_list_copy(f->ruleset);
    shadow->added    = NULL;
    shadow->removed  = NULL;
    shadow->refcount = f->refcount;

    shadow_filters = g_list_append(shadow_filters, shadow);

    return shadow;
}



/*
 * shadow_cancel:
 *
 * Forgets all about a given shadow and free's ressourcs for it.
 * At this point we can no longer assume that the shadow->current
 * field contains a valid pointer. We may have been called to 
 * clean up a shadow for a filter whose ruleset has already been
 * cleared. We don't clean up any memory that is owned by the 
 * associated filter.
 */
static void shadow_cancel(shadow_t *shadow)
{
    GList *r;

    g_assert(shadow != NULL);
    g_assert(shadow->filter != NULL);

    if (dbg >= 6)
        printf("cancel shadow for filter: %s\n", shadow->filter->name);

    for (r = shadow->added; r != NULL; r = r->next)
        rule_free(r->data);

    /* 
     * Since we cancel the shadow, we also free the added,
     * removed and current lists now. Then we remove the shadow
     * kill it also.
     */
    g_list_free(shadow->removed);
    g_list_free(shadow->added);
    g_list_free(shadow->current);
    shadow->removed = shadow->added = shadow->current = NULL;

    shadow_filters = g_list_remove(shadow_filters, shadow);
    g_free(shadow);
}



/*
 * shadow_commit:
 *
 * Commit all the changes for a given shadow and then forget and free
 * it.
 */
static void shadow_commit(shadow_t *shadow)
{
    GList *f;
    filter_t *realf;

    g_assert(shadow != NULL);
    g_assert(shadow->filter != NULL); 

    realf = shadow->filter;

    if (dbg >= 6) {
        printf("committing shadow for filter:\n");
        dump_shadow(shadow);
    }

    /*
     * Free memory for all removed rules
     */
    for (f = shadow->removed; f != NULL; f = f->next)
        rule_free(f->data);

    /* 
     * We also free the memory of the filter->ruleset GList.
     * We don't need them anymore.
     */
    g_list_free(shadow->filter->ruleset);

    /*
     * Now the actual filter is corrupted, because
     * we have freed memory its rules.
     * But we have a copy of the ruleset without exactly those 
     * rules we freed now. We use this as new ruleset.
     */
    shadow->filter->ruleset = shadow->current;

    /*
     * Not forgetting to update the refcount. There is a chance
     * that this shadow only existed because of a change in the
     * refcount.
     */
    shadow->filter->refcount = shadow->refcount;
    
    /* 
     * Now that we have actually commited the changes for this
     * shadow, we remove this shadow from our shadow list
     * and free it's ressources. Note that we do not free
     * shadow->current because this is the new filter ruleset.
     */
    g_list_free(shadow->added);
    g_list_free(shadow->removed);
    shadow->added = shadow->removed = shadow->current = NULL;
    shadow->filter = NULL;
    shadow_filters = g_list_remove(shadow_filters, shadow); 
    g_free(shadow);

    if (dbg >= 6) {
        printf("after commit filter looks like this\n");
        dump_filter(realf);
    }
}



static void filter_rebuild_target_combos()
{
    GtkMenu *m;
    GList *l;
    GList *buf = NULL;
    GtkWidget *opt_menus[] = {
        optionmenu_filter_text_target,
        optionmenu_filter_ip_target,
        optionmenu_filter_size_target,
        optionmenu_filter_jump_target,
        NULL };
    gint i;
    
    /*

     * Prepare a list of unbound filters and also leave
     * out the global and builtin filters.
     */
    for (l = filters; l != NULL; l = l->next) {
        filter_t *filter = (filter_t *)l->data;

        if (!IS_BOUND(filter) && !IS_GLOBAL(filter))
            buf = g_list_append(buf, filter);
    }

    /*
     * These can only be updated if there is a dialog.
     */
    if (filter_dialog != NULL) {
        for (i = 0; opt_menus[i] != NULL; i ++) {
            m = GTK_MENU(gtk_menu_new());
    
            for (l = buf; l != NULL; l = l->next) {
                filter_t *filter = (filter_t *)l->data;
                if (filter != work_filter)
                    menu_new_item_with_data(m, filter->name, filter);
            }
    
            gtk_option_menu_set_menu
                (GTK_OPTION_MENU(opt_menus[i]), GTK_WIDGET(m));
        }
    }

    /*
     * The following is in the main window and should always be
     * updateable.
     */
    m = GTK_MENU(gtk_menu_new());

    menu_new_item_with_data(m, "no default filter", NULL);
    for (l = buf; l != NULL; l = l->next) {
        filter_t *filter = (filter_t *)l->data;
        /*
         * This is no need to create a query which should not
         * display anything.
         */
        if ((filter != filter_drop) && (filter != filter_show))
            menu_new_item_with_data(m, filter->name, filter);
    }

    gtk_option_menu_set_menu
        (GTK_OPTION_MENU(optionmenu_search_filter), GTK_WIDGET(m));

    g_list_free(buf);
}


void filter_open_dialog() {
    if (filter_dialog == NULL) {
        GtkMenu *m;
        gint i;

        filter_dialog = create_dlg_filters();
        g_assert(filter_dialog != NULL);

        gtk_notebook_set_show_tabs
            (GTK_NOTEBOOK(notebook_filter_detail), FALSE);

       	gtk_clist_set_reorderable(GTK_CLIST(clist_filter_rules), TRUE);
        for (i = 0; i < 3; i++)
            gtk_clist_set_column_width(GTK_CLIST(clist_filter_rules), i,
                filter_table_col_widths[i]);
   
        m = GTK_MENU(gtk_menu_new());
        menu_new_item_with_data
            (m, rule_text_type_labels[RULE_TEXT_PREFIX], 
                (gpointer) RULE_TEXT_PREFIX);
        menu_new_item_with_data
            (m, rule_text_type_labels[RULE_TEXT_WORDS], 
                (gpointer) RULE_TEXT_WORDS);
        menu_new_item_with_data
            (m, rule_text_type_labels[RULE_TEXT_SUFFIX], 
                (gpointer)RULE_TEXT_SUFFIX);
        menu_new_item_with_data
            (m, rule_text_type_labels[RULE_TEXT_SUBSTR], 
                (gpointer) RULE_TEXT_SUBSTR);
        menu_new_item_with_data
            (m, rule_text_type_labels[RULE_TEXT_REGEXP], 
                (gpointer) RULE_TEXT_REGEXP);
        gtk_option_menu_set_menu
            (GTK_OPTION_MENU(optionmenu_filter_text_type), GTK_WIDGET(m));

        m = GTK_MENU(gtk_menu_new());
        menu_new_item_with_data(m, "display", (gpointer) 1);
        menu_new_item_with_data(m, "don't display", (gpointer) 0);
        gtk_option_menu_set_menu
            (GTK_OPTION_MENU(optionmenu_filter_default_policy), GTK_WIDGET(m));
    }

  	gtk_window_set_position(GTK_WINDOW(filter_dialog), GTK_WIN_POS_CENTER);

    if (current_search != NULL) {
        filter_set(current_search->filter);
    } else {
        filter_set(NULL);
    }

    option_menu_select_item_by_data(
        optionmenu_filter_default_policy, 
        (gpointer)filter_default_policy);
   
    gtk_widget_show(filter_dialog);
    gdk_window_raise(filter_dialog->window);
}



/*
 * filter_close_dialog:
 *
 * Close the filter dialog. If commit is TRUE the changes
 * are committed, otherwise dropped.
 */
void filter_close_dialog(gboolean commit)
{
    if (commit) {
        filter_commit_changes();
        filter_default_policy = (gint) option_menu_get_selected_data
            (optionmenu_filter_default_policy);
    } else
        filter_cancel_changes();

    if (filter_dialog != NULL) {
        //gtk_object_destroy(GTK_OBJECT(filter_dialog));
        //filter_dialog = NULL;
        gtk_widget_hide(filter_dialog);
    }
}



/*
 * filter_edit_rule:
 *
 * Load the given rule into the detail view.
 */
void filter_edit_rule(rule_t *r)
{
    if (filter_dialog == NULL)
        return;

    if (r != NULL) {
        switch (r->type) {
        case RULE_TEXT:
            filter_edit_text_rule(r);
            break;
        case RULE_IP:
            filter_edit_ip_rule(r);
            break;
        case RULE_SIZE:
            filter_edit_size_rule(r);
            break;
        case RULE_JUMP:
            filter_edit_jump_rule(r);
            break;
        default:
            g_error("Unknown rule type: %d", r->type);
        }
    } else {
        gtk_notebook_set_page(
            GTK_NOTEBOOK(notebook_filter_detail),
            nb_filt_page_buttons);
        gtk_clist_unselect_all(GTK_CLIST(clist_filter_rules));
    }
}



void filter_edit_text_rule(rule_t *r) 
{
    g_assert((r == NULL) || (r->type == RULE_TEXT));

    if (filter_dialog == NULL)
        return;

    if (r == NULL) {
        gtk_entry_set_text(
            GTK_ENTRY(entry_filter_text_pattern),
            "");
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(checkbutton_filter_text_case),
            FALSE);
        gtk_option_menu_set_history(
            GTK_OPTION_MENU(optionmenu_filter_text_type),
            RULE_TEXT_WORDS);
        option_menu_select_item_by_data(optionmenu_filter_text_target,
            (gpointer) DEFAULT_TARGET);
        gtk_toggle_button_set_active
            (GTK_TOGGLE_BUTTON(checkbutton_filter_text_invert_cond), FALSE);
    } else {
        gtk_entry_set_text(
            GTK_ENTRY(entry_filter_text_pattern),
            r->u.text.match);
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(checkbutton_filter_text_case),
            r->u.text.case_sensitive);
        gtk_option_menu_set_history(
            GTK_OPTION_MENU(optionmenu_filter_text_type),
            r->u.text.type);
        option_menu_select_item_by_data(optionmenu_filter_text_target,
            (gpointer) r->target);
        gtk_toggle_button_set_active
            (GTK_TOGGLE_BUTTON(checkbutton_filter_text_invert_cond),
            r->negate);
    }

    gtk_notebook_set_page(
        GTK_NOTEBOOK(notebook_filter_detail),
         nb_filt_page_text);
}



void filter_edit_ip_rule(rule_t *r)
{
    g_assert((r == NULL) ||(r->type == RULE_IP));

    if (filter_dialog == NULL)
        return;

    if (r == NULL) {
        gtk_entry_set_text(GTK_ENTRY(entry_filter_ip_address), "");
        gtk_entry_set_text(GTK_ENTRY(entry_filter_ip_mask), "");
        option_menu_select_item_by_data(optionmenu_filter_ip_target,
            (gpointer) DEFAULT_TARGET);
        gtk_toggle_button_set_active
            (GTK_TOGGLE_BUTTON(checkbutton_filter_ip_invert_cond), FALSE);
    } else {
        gtk_entry_set_text(
            GTK_ENTRY(entry_filter_ip_address), 
            ip_to_gchar(r->u.ip.addr));
        gtk_entry_set_text(
            GTK_ENTRY(entry_filter_ip_mask),
            ip_to_gchar(r->u.ip.mask));
        option_menu_select_item_by_data(optionmenu_filter_ip_target,
            (gpointer) r->target);
        gtk_toggle_button_set_active
            (GTK_TOGGLE_BUTTON(checkbutton_filter_ip_invert_cond),
            r->negate);
    }

    gtk_notebook_set_page(
        GTK_NOTEBOOK(notebook_filter_detail),
        nb_filt_page_ip);
}



void filter_edit_size_rule(rule_t *r)
{
    g_assert((r == NULL) || (r->type == RULE_SIZE));

    if (filter_dialog == NULL)
        return;

    if (r == NULL) {
        gtk_spin_button_set_value(
            GTK_SPIN_BUTTON(spinbutton_filter_size_min), 
            0);
        gtk_spin_button_set_value(
            GTK_SPIN_BUTTON(spinbutton_filter_size_max),
            0);
        option_menu_select_item_by_data(optionmenu_filter_size_target,
            (gpointer) DEFAULT_TARGET);
        gtk_toggle_button_set_active
            (GTK_TOGGLE_BUTTON(checkbutton_filter_size_invert_cond), FALSE);
    } else {
        gtk_spin_button_set_value(
            GTK_SPIN_BUTTON(spinbutton_filter_size_min), 
            r->u.size.lower);
        gtk_spin_button_set_value(
            GTK_SPIN_BUTTON(spinbutton_filter_size_max),
            r->u.size.upper);
        option_menu_select_item_by_data(optionmenu_filter_size_target,
            (gpointer) r->target);
        gtk_toggle_button_set_active
            (GTK_TOGGLE_BUTTON(checkbutton_filter_size_invert_cond),
            r->negate);
    }

    gtk_notebook_set_page(
        GTK_NOTEBOOK(notebook_filter_detail),
        nb_filt_page_size);
}



void filter_edit_jump_rule(rule_t *r)
{
    g_assert((r == NULL) || (r->type == RULE_JUMP));

    if (filter_dialog == NULL)
        return;

    if (r == NULL) {
        option_menu_select_item_by_data(optionmenu_filter_jump_target,
            (gpointer) DEFAULT_TARGET);
    } else {
        option_menu_select_item_by_data(optionmenu_filter_jump_target,
            (gpointer) r->target);
    }

    gtk_notebook_set_page(
        GTK_NOTEBOOK(notebook_filter_detail),
        nb_filt_page_jump);
}



/*
 * filter_get_rule:
 *
 * Fetch the rule which is currently edited. This
 * returns a completely new rule_t item in new memory.
 */
rule_t *filter_get_rule() 
{
    gint page;
    rule_t *r;

    g_return_val_if_fail(filter_dialog != NULL, NULL);

    page = gtk_notebook_get_current_page
        (GTK_NOTEBOOK(notebook_filter_detail));

    gtk_notebook_set_page(
        GTK_NOTEBOOK(notebook_filter_detail),
        nb_filt_page_buttons);

    switch (page) {
    case nb_filt_page_buttons:
        r = NULL;
        break;
    case nb_filt_page_text:
        r = filter_get_text_rule();
        break;
    case nb_filt_page_ip:
        r = filter_get_ip_rule();
        break;
    case nb_filt_page_size:
        r = filter_get_size_rule();
        break;
    case nb_filt_page_jump:
        r = filter_get_jump_rule();
        break;
    default:
        g_assert_not_reached();
        r = NULL;
    };

    if ((r != NULL) && (dbg >= 5))
        printf("got rule: %s\n", rule_to_gchar(r));

    return r;
}



rule_t *filter_new_text_rule(gchar * match, gint type, 
    gboolean case_sensitive, filter_t *target, gboolean negate)
{
  	rule_t *r;
    gchar *buf;

    g_assert(match != NULL);
    g_assert(target != NULL);

  	r = g_new0(rule_t, 1);

   	r->type                  = RULE_TEXT;
    r->negate                = negate;
    r->target                = target;
    r->u.text.case_sensitive = case_sensitive;
    r->u.text.type           = type;
    r->u.text.match          = g_strdup(match);
    r->valid                 = TRUE;

    if (!r->u.text.case_sensitive)
        strlower(r->u.text.match, r->u.text.match);

    buf = g_strdup(r->u.text.match);

  	if (r->u.text.type == RULE_TEXT_WORDS) {
		gchar *s;
		GList *l = NULL;

		for (s = strtok(buf, " \t\n"); s; s = strtok(NULL, " \t\n"))
			l = g_list_append(l, pattern_compile(s));

		r->u.text.u.words = l;
	} else 
    if (r->u.text.type == RULE_TEXT_REGEXP) {
		int err;
		regex_t *re;

		re = g_new0(regex_t, 1);
		err = regcomp
            (re, buf, REG_NOSUB|(r->u.text.case_sensitive ? 0 : REG_ICASE));

		if (err) {
			char buf[1000];
			regerror(err, re, buf, 1000);

			g_warning(
                "problem in regular expression: %s"
				"; falling back to substring match", buf);

			r->u.text.type = RULE_TEXT_SUBSTR;
            g_free(re);
		} else {
			r->u.text.u.re = re;
		}
	}

	/* no "else" because REGEXP can fall back here */
	if (r->u.text.type == RULE_TEXT_SUBSTR)
		r->u.text.u.pattern = pattern_compile(buf);

    g_free(buf);

    return r;
}



rule_t *filter_new_ip_rule
    (guint32 addr, guint32 mask, filter_t *target, gboolean negate)
{
	rule_t *r;

    g_assert(target != NULL);

	r = g_new0(rule_t, 1);

   	r->type = RULE_IP;

	r->u.ip.addr  = addr;
	r->u.ip.mask  = mask;
	r->u.ip.addr &= r->u.ip.mask;
    r->target     = target;
    r->negate     = negate;
    r->valid      = TRUE;

    return r;
}



rule_t *filter_new_size_rule
    (size_t lower, size_t upper, filter_t *target, gboolean negate)
{
   	rule_t *f;

    g_assert(target != NULL);

    f = g_new0(rule_t, 1);

    f->type = RULE_SIZE;

    if (lower > upper) {
        f->u.size.lower = upper;
        f->u.size.upper = lower;
    } else {
        f->u.size.lower = lower;
        f->u.size.upper = upper;
    }

  	f->target       = target;
    f->negate       = negate;
    f->valid        = TRUE;

    return f;
}




rule_t *filter_new_jump_rule(filter_t *target)
{
   	rule_t *f;

    g_assert(target != NULL);

    f = g_new0(rule_t, 1);

    f->type = RULE_JUMP;

  	f->target       = target;
    f->negate       = FALSE;
    f->valid        = TRUE;

    return f;
}


/* 
 * filter_get_text_rule:
 *
 * Extract information about a text rule.
 * NEVER CALL DIRECTLY!!! Use rule_get_rule().
 */
static rule_t *filter_get_text_rule()
{
  	rule_t *r;
    gchar *match;
    gint type;
    gboolean case_sensitive;
    filter_t *target;
    gboolean negate;

    g_return_val_if_fail(filter_dialog != NULL, NULL);

	type = (enum rule_text_type)
        option_menu_get_selected_data(optionmenu_filter_text_type);

	match = gtk_editable_get_chars
        (GTK_EDITABLE(entry_filter_text_pattern), 0, -1);

	case_sensitive = gtk_toggle_button_get_active
        (GTK_TOGGLE_BUTTON(checkbutton_filter_text_case));

	negate = gtk_toggle_button_get_active
        (GTK_TOGGLE_BUTTON(checkbutton_filter_text_invert_cond));

    target = (filter_t *)option_menu_get_selected_data
        (optionmenu_filter_text_target);

    r = filter_new_text_rule
        (match, type, case_sensitive, target, negate);

    g_free(match);
    
    return r;
}

/* 
 * filter_get_ip_rule:
 *
 * Extract information about a ip rule.
 * NEVER CALL DIRECTLY!!! Use filter_get_rule().
 */
static rule_t *filter_get_ip_rule()
{
    gchar *s;
    guint32 addr;
    guint32 mask;
    filter_t *target;
    gboolean negate;

    g_return_val_if_fail(filter_dialog != NULL, NULL);

	s = gtk_editable_get_chars(GTK_EDITABLE(entry_filter_ip_address), 0, -1);
	addr = ntohl(inet_addr(s));
	g_free(s);

	s = gtk_editable_get_chars(GTK_EDITABLE(entry_filter_ip_mask), 0, -1);
	mask = ntohl(inet_addr(s));
	g_free(s);

    negate = gtk_toggle_button_get_active
        (GTK_TOGGLE_BUTTON(checkbutton_filter_ip_invert_cond));

    target = (filter_t *)option_menu_get_selected_data
        (optionmenu_filter_ip_target);

    return filter_new_ip_rule(addr, mask, target, negate);
}



/* 
 * filter_get_size_rule:
 *
 * Extract information about a size rule.
 * NEVER CALL DIRECTLY!!! Use filter_get_rule().
 */
static rule_t *filter_get_size_rule()
{
    size_t lower;
    size_t upper;
    filter_t *target;
    gboolean negate;

    if (filter_dialog == NULL)
        return NULL;

    lower = gtk_spin_button_get_value_as_int
        (GTK_SPIN_BUTTON(spinbutton_filter_size_min));

    upper = gtk_spin_button_get_value_as_int
        (GTK_SPIN_BUTTON(spinbutton_filter_size_max));

    negate = gtk_toggle_button_get_active
        (GTK_TOGGLE_BUTTON(checkbutton_filter_size_invert_cond));

    target = (filter_t *)option_menu_get_selected_data
        (optionmenu_filter_size_target);

    return filter_new_size_rule(lower, upper, target, negate);
}



/* 
 * filter_get_jump_rule:
 *
 * Extract information about a size rule.
 * NEVER CALL DIRECTLY!!! Use filter_get_rule().
 */
static rule_t *filter_get_jump_rule()
{
    filter_t *target;

    if (filter_dialog == NULL)
        return NULL;

    target = (filter_t *)option_menu_get_selected_data
        (optionmenu_filter_jump_target);

    return filter_new_jump_rule(target);
}



/*
 * filter_update_filters:
 *
 * Update the filters. Selects the current work filter in the
 * option menu. If the work_filter does no longer exists, it
 * looks for another filter to display. If none is left, it
 * blocks the buttons.
 */
void filter_update_filters() 
{
    GList *l;
    gboolean work_filter_exists = FALSE;
    GtkWidget *m;
    gint active_item = -1;
    gint item_count = 0;

    filter_rebuild_target_combos();

    if(filter_dialog == NULL)
        return;

    m = gtk_menu_new();
     
    for (l = filters; l != NULL; l = l->next) {
        filter_t *r = (filter_t *) l->data;
        GtkWidget *w;

        if (!IS_BUILTIN(r)) {
            w = menu_new_item_with_data(GTK_MENU(m), r->name, r);
            gtk_signal_connect
                (GTK_OBJECT(w), "activate", on_filter_filters_activate, r);
    
            if (work_filter == r) {
                work_filter_exists = TRUE;
                active_item = item_count;
            }
    
            item_count ++;
        }
    }
        
    gtk_option_menu_set_menu
        (GTK_OPTION_MENU(optionmenu_filter_filters), m);
    if (active_item != -1)
        gtk_option_menu_set_history
            (GTK_OPTION_MENU(optionmenu_filter_filters), active_item);

    /*
     * If the current filter no longer exists, look for 
     * an other one and switch to that if possible.
     */
    if (!work_filter_exists) {
        filter_t *r = NULL;
    
        if (filters != NULL)
            r = option_menu_get_selected_data(optionmenu_filter_filters);

        filter_set((filter_t *) r);
    }
}



/*
 * filter_set:
 *
 * Start working on the given filter. Set this filter as 
 * work_filter so we can commit the changed rules to this
 * filter.
 */
void filter_set(filter_t *f)
{
    gboolean is_work;
    shadow_t *shadow;

    work_filter = f;
    is_work = work_filter != NULL;

    if (filter_dialog == NULL)
        return;

    if (f != NULL)
        shadow = shadow_find(f);
    else
        shadow = NULL;

    gtk_widget_set_sensitive(button_filter_add_rule_text, is_work);
    gtk_widget_set_sensitive(button_filter_add_rule_ip, is_work);
    gtk_widget_set_sensitive(button_filter_add_rule_size, is_work);
    gtk_widget_set_sensitive(button_filter_remove, 
        (f != NULL) && !IS_BOUND(f) && !IS_GLOBAL(f) && !IS_BUILTIN(f) &&
        (shadow == NULL || shadow->refcount == 0) && (f->refcount == 0));
 
    filter_edit_rule(NULL);

    if (f == NULL) {
        filter_set_ruleset(NULL);
    } else {
        shadow_t *shadow;
        /*
         * Check if there already is a shadow for this filter, if not,
         * allocate one.
         */
        
        shadow = shadow_find(f);
        if (shadow == NULL)
            shadow = shadow_new(f);

        /*
         * Display the current state of the filter as stored in the
         * shadow.
         */
        if (dbg >= 5)
            printf("showing ruleset for filter: %s\n", f->name);
        filter_set_ruleset(shadow->current);
    }

    /* 
     * don't want the work_filter to be selectable as a target
     * so we changed it... we have to rebuild.
     */
    filter_update_filters();
}



/*
 * filter_close_search:
 *
 * Clear the searches shadow, update the combobox and the filter
 * bound to this search (search->ruleser).
 */
void filter_close_search(search_t *s)
{
    g_assert(s != NULL);

    if (dbg >= 6)
        printf("closing search (freeing filter): %s\n", s->query);

    filter_free(s->filter);
    s->filter = NULL;

    filter_update_filters();
}



/*
 * filter_commit_changes:
 *
 * Go through all the shadow filters, and commit the recorded
 * changes to the assosicated filter. We walk through the 
 * shadow->current list. Every item in shadow->removed will be
 * removed from the searchs filter and the memory will be freed.
 * Then shadow->current will be set as the new filter for that
 * search.
 */
void filter_commit_changes() 
{
    GList *s;

    /*
     * Free memory for all removed filters;
     */
    for (s = shadow_filters; s != NULL; s = shadow_filters)
        shadow_commit((shadow_t*)s->data);
}



/*
 * filter_cancel_changes:
 *
 * Free the ressources for all added filters and forget all shadows.
 */
void filter_cancel_changes()
{
    GList *s;

    /*
     * Free memory for all added filters and for the shadows.
     */
    for (s = shadow_filters; s != NULL; s = shadow_filters)
        shadow_cancel((shadow_t *)s->data);
}



/*
 * filter_set_ruleset:
 *
 * Display the given ruleset in the table.
 */
static void filter_set_ruleset(GList *ruleset)
{
    GList *l;
    gint count = 0;

    if (filter_dialog == NULL)
        return;

    gtk_clist_freeze(GTK_CLIST(clist_filter_rules));
    gtk_clist_clear(GTK_CLIST(clist_filter_rules));
        
    for (l = ruleset; l != NULL; l = l->next) {
        rule_t *r = (rule_t *)l->data;
        gchar *titles[3];
        gint row;

        g_assert(r != NULL);
        count ++;
        titles[0] = r->negate ? "X" : "";
        titles[1] = rule_condition_to_gchar(r);
        titles[2] = r->target->name;
        row = gtk_clist_append(GTK_CLIST(clist_filter_rules), titles);
        gtk_clist_set_row_data
            (GTK_CLIST(clist_filter_rules), row, (gpointer) r);
    }
    gtk_clist_thaw(GTK_CLIST(clist_filter_rules));

    gtk_widget_set_sensitive(button_filter_clear, count != 0);

    if (dbg >= 5)
        printf("updated %d items\n", count);
}



/*
 * rule_condition_to_gchar:
 *
 * Convert a rule condition to a human readable string.
 */
static gchar *rule_condition_to_gchar(rule_t *r)
{
    static gchar tmp[256];

    g_assert(r != NULL);
    
    switch (r->type) {
    case RULE_TEXT:
        switch (r->u.text.type) {
        case RULE_TEXT_PREFIX:
           	g_snprintf(
                tmp, sizeof(tmp), 
                "If filename begins with \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        case RULE_TEXT_WORDS:
           	g_snprintf(
                tmp, sizeof(tmp), 
                "If filename contains the words \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        case RULE_TEXT_SUFFIX:
          	g_snprintf(
                tmp, sizeof(tmp), 
                "If filename ends with \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        case RULE_TEXT_SUBSTR:
           	g_snprintf(
                tmp, sizeof(tmp), 
                "If filename contains the substring \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        case RULE_TEXT_REGEXP:
           	g_snprintf(
                tmp, sizeof(tmp), 
                "If filename matches the regex \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        default:
            g_error("Unknown text rule type: %d", r->u.text.type);
        };
        break;
    case RULE_IP:
       	g_snprintf(
            tmp, sizeof(tmp), 
            "If IP address matches %s/%s",
            ip_to_gchar(r->u.ip.addr),
            ip_to_gchar(r->u.ip.mask));
        break;
    case RULE_SIZE:
		if (r->u.size.lower == 0)
			g_snprintf(tmp, sizeof(tmp),
				"If filesize is smaller than %d (%s)",
				r->u.size.upper, short_size(r->u.size.upper));
		else if (r->u.size.upper == r->u.size.lower)
			g_snprintf(tmp, sizeof(tmp),
				"If filesize is exactly %d (%s)",
				r->u.size.upper, short_size(r->u.size.upper));
		else
			g_snprintf(tmp, sizeof(tmp),
				"If filesize is between %d and %d (%s - %s)",
				r->u.size.lower, r->u.size.upper,
				short_size(r->u.size.lower), short_size(r->u.size.upper));
        break;
    case RULE_JUMP:
       	g_snprintf(
            tmp, sizeof(tmp), 
            "Always");
        break;
    default:
        g_error("Unknown rule type: %d", r->type);
        return NULL;
    };

    return tmp;
}



/*
 * rule_to_gchar:
 *
 * Convert the filter to a human readable string.
 */
gchar *rule_to_gchar(rule_t *r) 
{
    gchar *cond;

    g_assert(r != NULL);

    cond = g_strdup(rule_condition_to_gchar(r));

	g_snprintf(f_tmp, sizeof(f_tmp), "%s %s jump to \"%s\"", 
        r->negate ? "(Negate) " : "", cond, 
        r->valid ? r->target->name : "(invalid)");

    g_free(cond);
  
    return f_tmp;
}



/*
 * filter_new:
 *
 * Create a new filter with the given name and register it.
 */
filter_t *filter_new(gchar *name)
{
    filter_t *f;

    g_assert(name != NULL);

    f = g_new0(filter_t, 1);
    f->name = g_strdup(name);
    f->ruleset = NULL;
    f->search = NULL;
    f->visited = FALSE;

    filters = g_list_append(filters, f);

    return f;
}



/*
 * filter_new_for_search:
 *
 * Create a new filter bound to a search and register it.
 */
void filter_new_for_search(search_t *s)
{
    filter_t *f;

    g_assert(s != NULL);
    g_assert(s->query != NULL);

    f = filter_new(s->query);
    f->search = s;
    s->filter = f;

    filter_update_filters();
}



/*
 * filter_free:
 *
 * Frees a filter and the filters assiciated with it and
 * unregister it.
 */
void filter_free(filter_t *f) 
{
    GList *l;

    g_assert(f != NULL);

    // FIXME: Have to rework shadow code to allow removal of
    // filter without previous commit.
    filter_commit_changes();

    if (f->refcount != 0)
        g_error("Unable to free referenced filter \"%s\" with refcount %d",
            f->name, f->refcount);

    /*
     * If this is the filter currently worked on, clear the display.
     */
    if (work_filter == f)
        filter_set_ruleset(NULL);

    filters = g_list_remove(filters, f);
   
    for (l = f->ruleset; l != NULL; l = l->next) {
        filter_remove_rule(f, (rule_t *)l->data);
    }

    filter_commit_changes();

    g_free(f->name);
    f->name = NULL;
    
    g_free(f);
}



/*
 * rule_free:
 *
 * Free memory reserved by rule respecting the type of the rule.
 */
static void rule_free(rule_t *r)
{
    g_assert(r != NULL);

    if (dbg >= 6)
        printf("freeing rule: %s\n", rule_to_gchar(r));

	if (r->type == RULE_TEXT) {
        g_free(r->u.text.match);

        switch (r->u.text.type) {
        case RULE_TEXT_WORDS:
            g_list_foreach(r->u.text.u.words, (GFunc)pattern_free, NULL);
            g_list_free(r->u.text.u.words);
            r->u.text.u.words = NULL;
            break;
        case RULE_TEXT_SUBSTR:
            pattern_free(r->u.text.u.pattern);
            r->u.text.u.pattern = NULL;
            break;
        case RULE_TEXT_REGEXP:
            regfree(r->u.text.u.re);
            r->u.text.u.re = NULL;
            break;
        case RULE_TEXT_PREFIX:
        case RULE_TEXT_SUFFIX:
            break;
        default:
            g_error("Unknown text filter type: %d", r->u.text.type);
        }
    }
	g_free(r);
}



/*
 * filter_append_rule:
 *
 * Append a new rule to the filter shadow. This call will fail
 * with an assertion error if the rule is already existing in
 * the shadow.
 */
void filter_append_rule(filter_t *f, rule_t *r)
{
    shadow_t *shadow = NULL;
    shadow_t *target_shadow = NULL;

    g_assert(r != NULL);
    g_assert(f != NULL);

    if (dbg >= 4)
        printf("appending rule to filter: %s <- %s (%p)\n",
            f->name, rule_to_gchar(r), r->target);

    /*
     * Create a new shadow if necessary.
     */
    shadow = shadow_find(f);
    if (shadow == NULL)
        shadow = shadow_new(f);
    else {
        /*
         * You can never add a filter to a shadow or filter
         * twice! Not even if you removed it before (without
         * committing).
         */
        g_assert(g_list_find(shadow->added,   r) == NULL);
        g_assert(g_list_find(shadow->current, r) == NULL);
        g_assert(g_list_find(shadow->removed, r) == NULL);
    }

    shadow->added   = g_list_append(shadow->added, r);
    shadow->current = g_list_append(shadow->current, r);

    /*
     * We need to increase the refcount on the target.
     */
    target_shadow = shadow_find(r->target);
    if (target_shadow == NULL)
        target_shadow = shadow_new(r->target);

    target_shadow->refcount ++;
    if (dbg >= 6)
        printf("increased refcount on \"%s\" to %d\n",
            target_shadow->filter->name, target_shadow->refcount);

    /*
     * Update dialog if necessary.
     */
    if (work_filter == f)
        filter_set_ruleset(shadow->current);
}



/*
 * filter_remove_rule:
 *
 * Remove rule from a filter shadow. This call will fail
 * with an assertion error if the rule has already been 
 * removed from the shadow or if it never was in the shadow.
 * The memory associated with the rule will be freed.
 */
void filter_remove_rule(filter_t *f, rule_t *r)
{
    shadow_t *shadow;
    shadow_t *target_shadow;
    GList *l = NULL;
       
    g_assert(r != NULL);
    g_assert(f != NULL);

    if (dbg >= 4)
        printf("removing rule in filter: %s -> %s\n", 
            f->name, rule_to_gchar(r));

    /*
     * Create a new shadow if necessary.
     */
    shadow = shadow_find(f);
    if (shadow == NULL)
        shadow = shadow_new(f);

    g_assert(g_list_find(shadow->current, r) != NULL);

    l = g_list_find(shadow->added, r);
    if (l != NULL) {
        /*
         * The rule was added only to the shadow and was
         * not committed. We removed it from the added list
         * and free the ressources.
         */
        if (dbg >= 4)
            printf("while removing from %s: removing from added: %s\n",
                f->name, rule_to_gchar(r));
        shadow->added = g_list_remove(shadow->added, r);
        rule_free(r);
    } else {
        /*
         * The rule was not added, so it must be existent.
         * If it is, we remember it on shadow->removed.
         */
        g_assert(g_list_find(shadow->removed, r) == NULL);

        if (dbg >= 4)
            printf("while removing from %s: adding to removed: %s\n",
                f->name, rule_to_gchar(r));
      
        shadow->removed = g_list_append(shadow->removed, r);
    }

    shadow->current = g_list_remove(shadow->current, r);

    /*
     * We need to decrease the refcount on the target.
     */
    target_shadow = shadow_find(r->target);
    if (target_shadow == NULL)
        target_shadow = shadow_new(r->target);

    target_shadow->refcount --;
    if (dbg >= 6)
        printf("decreased refcount on \"%s\" to %d\n",
            target_shadow->filter->name, target_shadow->refcount);

    /*
     * Update dialog if necessary.
     */
    if (work_filter == f)
        filter_set_ruleset(shadow->current);
}



/*
 * filter_replace_rule:
 *
 * Replaces filter rule A with filter rule B in filter . A
 * must already be in the shadow and B must not! 
 *
 * CAUTION: ACTUALLY B MUST NOT BE IN ANY OTHER SEARCH !!!
 *
 * The memory for A is freed in the process.
 */
void filter_replace_rule(filter_t *f, 
    rule_t *old_rule, rule_t *new_rule)
{
    GList *filter;
    GList *added;
    shadow_t *shadow;
    shadow_t *target_shadow;

    g_assert(old_rule != new_rule);
    g_assert(old_rule != NULL);
    g_assert(new_rule != NULL);

    /*
     * Create a new shadow if necessary.
     */
    shadow = shadow_find(f);
    if (shadow == NULL)
        shadow = shadow_new(f);

    /*
     * Find the list node where we have to replace the
     * rule.
     */
    filter = g_list_find(shadow->current, old_rule);
    g_assert(filter != NULL);

    if (dbg >= 4) {
        gchar * f1 = g_strdup(rule_to_gchar(old_rule));
        gchar * f2 = g_strdup(rule_to_gchar(new_rule));

        printf("replacing rules (old <- new): %s <- %s\n", f1, f2);

        g_free(f1);
        g_free(f2);
    }

    /*
     * Find wether the node to be replaced is in shadow->added. 
     * If so, we may free the memory of the old rule later.
     */
    added = g_list_find(shadow->added, old_rule);

    if (added != NULL) {
        /*
         * If it was added, then free and remove the rule.
         */
        shadow->added = g_list_remove(shadow->added, old_rule);
        rule_free(old_rule);
    } else {
        /*
         * If the filter was not added, then it must be marked
         * for begin removed.
         */
        shadow->removed = g_list_append(shadow->removed, old_rule);
    }

    /*
     * In any case we have to reduce the refcount on the old rule's
     * target.
     */
    target_shadow = shadow_find(old_rule->target);
    if (target_shadow == NULL)
        target_shadow = shadow_new(old_rule->target);

    target_shadow->refcount --;
    if (dbg >= 6)
        printf("decreased refcount on \"%s\" to %d\n",
            target_shadow->filter->name, target_shadow->refcount);
     
    /*
     * The new rule can't be in the original filter, so we mark it
     * as added.
     */
    shadow->added = g_list_append(shadow->added, new_rule);

    /*
     * And we also need to increase the refcount on the new rule's
     * target
     */
    target_shadow = shadow_find(new_rule->target);
    if (target_shadow == NULL)
        target_shadow = shadow_new(new_rule->target);

    target_shadow->refcount ++;
    if (dbg >= 6)
        printf("increased refcount on \"%s\" to %d\n",
            target_shadow->filter->name, target_shadow->refcount);
        
    /*
     * In shadow->current we just replace the rule.
     */
    filter->data = new_rule;
    
    /*
     * Update dialog if necessary.
     */
    if (work_filter == f)
        filter_set_ruleset(shadow->current);
}



/*
 * filter_adapt_order:
 *
 * Reorders the filter according to the order in the user's
 * table in the gui. This should only be used after the
 * user has reordered the table. It can not properly cope
 * with added or deleted items. This will also only work
 * if a filter is currently being displayed in the table.
 * If the filter dialog has not been initialized or not
 * filter is currently worked on, it will silently fail.
 */
void filter_adapt_order(void)
{
    GList *neworder = NULL;
    gint row;
    shadow_t *shadow;

    if (!work_filter || filter_dialog == NULL)
        return;
   
    /*
     * Create a new shadow if necessary.
     */
    shadow = shadow_find(work_filter);
    if (shadow == NULL)
        shadow = shadow_new(work_filter);

    /*
     * Assumption: every rule in shadow->current is also
     * bound to a row in the filter table. So we can free
     * this list and rebuild it in the right order from the
     * row data.
     */
    g_list_free(shadow->current);

    for (row = 0; row < GTK_CLIST(clist_filter_rules)->rows; row ++) {
        filter_t *f;

        f = gtk_clist_get_row_data(GTK_CLIST(clist_filter_rules), row);
        g_assert(f != NULL);
        
        neworder = g_list_append(neworder, f);
    }

    shadow->current = neworder;
}



/*
 * filter_apply:
 *
 * returns 0 for hide, 1 for display, -1 for undecided 
 */
static int filter_apply(filter_t *filter, struct record *rec)
{
    size_t namelen;
	char *l_name;
    GList *list;

    g_assert(filter != NULL);
    g_assert(rec != NULL);

    /*
     * If we reached one of the builtin targets, we bail out.
     */
    if (filter == filter_show) {
        return 1;
    }
    if (filter == filter_drop) {
        return 0;
    }
  
    /*
     * We only try to prevent circles.
     */
    if (filter->visited == TRUE) {
        return -1;
    }

    filter->visited = TRUE;

    list = filter->ruleset;

	namelen = strlen(rec->name);
	l_name = g_malloc(sizeof(char) * (namelen + 1));
	strlower(l_name, rec->name);

	list = g_list_first(list);
	while (list != NULL) {
		size_t n;
		int i;
		rule_t *r; 
        gboolean match = FALSE;

        r = (rule_t *)list->data;
        if (dbg >= 6)
            printf("trying to match against: %s\n", rule_to_gchar(r));

		switch (r->type) {
        case RULE_JUMP:
            match = TRUE;
            break;
		case RULE_TEXT:
			switch (r->u.text.type) {
			case RULE_TEXT_PREFIX:
				if (strncmp(r->u.text.case_sensitive ? rec->name : l_name,
					    r->u.text.match, strlen(r->u.text.match)) == 0)
					match = TRUE;
				break;
			case RULE_TEXT_WORDS:
                {
                    GList *l;
    
                    for (
                        l = g_list_first(r->u.text.u.words);
                        l && !match; 
                        l = g_list_next(l)
                    ) {
                        if (pattern_qsearch
                            ((cpattern_t *)l->data,
                             r->u.text.case_sensitive
                             ? rec->name : l_name, 0, 0, qs_any)
                            != NULL)
                            match = TRUE;
                    }
                }
				break;
			case RULE_TEXT_SUFFIX:
				n = strlen(r->u.text.match);
				if (namelen > n
				    && strcmp((r->u.text.case_sensitive
					       ? rec->name : l_name) + namelen
					      - n, r->u.text.match) == 0)
				    match = TRUE;
				break;
			case RULE_TEXT_SUBSTR: 
				if (pattern_qsearch(r->u.text.u.pattern,
						    r->u.text.case_sensitive
						    ? rec->name : l_name, 0, 0,
						    qs_any) != NULL)
					match = TRUE;
				break;
			case RULE_TEXT_REGEXP:
				if ((i = regexec(r->u.text.u.re, rec->name,
						 0, NULL, 0)) == 0)
					match = TRUE;
				if (i == REG_ESPACE)
					g_warning("regexp memory overflow");
				break;
			default:
				g_error("Unknown text rule type: %d",
					r->u.text.type);
			}
			break;
		case RULE_IP:
			if ((rec->results_set->ip & r->u.ip.mask) == r->u.ip.addr)
				match = TRUE;
			break;
		case RULE_SIZE:
			if (rec->size >= r->u.size.lower && rec->size <= r->u.size.upper)
				match = TRUE;
			break;
		default:
			g_error("Unknown rule type: %d", r->type);
			break;
		}
    
        /*
         * If negate is set, we invert the meaning of match.
         */

		if (r->negate)
			match = !match;

        if (match) {
            gint val;                                                
            val = filter_apply(r->target, rec);                      

            /*
             * If a decision could be reached, we return.
             */
            if (val != -1) {
                if(dbg >= 6)
                    printf("matched rule: %s\n", rule_to_gchar(r));

                g_free(l_name); 
                filter->visited = FALSE; 
                return val;    
            }
        }

		list = g_list_next(list);
	}
    g_free(l_name);

    filter->visited = FALSE;
    return -1;
}



/*
 * filter_record:
 *
 * Check a particular record against the search filter and the global
 * filters. Returns TRUE if the record can be displayed, FALSE if not
 */
gboolean filter_record(search_t *sch, struct record *rec)
{
    gint r;

    g_assert(sch != NULL);
    g_assert(rec != NULL);

	if (search_strict_and) {	
        // FIXME:
        // config value for strict AND checking
		// XXX for now -- RAM
	}



    /*
     * Default to "display" if there is no filter defined or to
     * global_policy if there is.
     */
    if (
        (sch->filter->ruleset == NULL) && 
        (filter_global_pre->ruleset == NULL) &&
        (filter_global_post->ruleset == NULL)
    )
        r = 1;
    else
        r = -1;

    /*
     * If it has not yet been decided, try the global filter
     */
    if (r == -1)
        r = filter_apply(filter_global_pre, rec);

    /*
     * If not decided check if the filters for this search apply.
     */
    if (r == -1)
        r = filter_apply(sch->filter, rec);

    /*
     * If it has not yet been decided, try the global filter
     */
	if (r == -1)
		r = filter_apply(filter_global_post, rec);

    /*
     * If no filter can decide then use the default.
     */
    if (r == -1)
        r = filter_default_policy;

    if (dbg >= 5) {
        printf("result %d for search \"%s\" matching \"%s\" (%s)\n",
            r, sch->query, rec->name, 
            ((sch->filter->ruleset == NULL) && 
            (filter_global_pre->ruleset == NULL) &&
            (filter_global_post->ruleset == NULL)) ?
                "unfiltered" : "filtered");
    }

	return r;
}



/*
 * filters_shutdown
 *
 * Free global filters and save state.
 */
void filter_shutdown(void)
{
    GList *f;

    filter_close_dialog(FALSE);

    if (dbg >= 5)
        printf("shutting down filters\n");

    /*
     * It is important that all searches have already been closed.
     * Since it is not allowd to use a bound filter as a target,
     * a bound filter will always have a refcount of 0. So it is
     * not a problem just closing the searches.
     * But for the free filters, we have to prune all rules before
     * we may free the filers, because we have to reduce the 
     * refcount on every filter to 0 before we are allowed to free it.
     */

    for (f = filters; f != NULL; f = f->next) {
        filter_t *filter = (filter_t*) f->data;
        GList *l;

        /*
         * Since we use the shadows, we can just traverse the list.
         */
        for (l = filter->ruleset; l != NULL; l=l->next) {
            filter_remove_rule(filter, (rule_t *)l->data);
        }

        /*
         * Don't push the commit off too long. We just commit
         * after every filter.
         */
        filter_commit_changes();
    }

    /*
     * Now we remove the filters. So we may not traverse. We just
     * free the first filter until none is left. This will also
     * clean up the builtin filters (filter_drop/show) and the
     * global filters;
     */
    for (f = filters; f != NULL; f = filters)
        filter_free(f->data);
}



/*
 * filter_init
 *
 * Initialize global filters.
 */
void filter_init(void)
{
    filter_global_pre  = filter_new("Global (pre)");
    filter_global_post = filter_new("Global (post)");
    filter_drop        = filter_new("don't display");
    filter_show        = filter_new("display");

    /*
     * Acutally filter_drop and filter_show can stay empty.
     */
}

/*
 * on_filter_filters_activate:
 *
 * Callback for option menu to select filters.
 */
static void on_filter_filters_activate
    (GtkMenuItem * menuitem, gpointer user_data)
{
    filter_t *filter = (filter_t *) option_menu_get_selected_data
        (optionmenu_filter_filters);

    g_assert(filter != NULL);

    filter_set(filter);
}

