/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* 
 * Copyright (C) 2002 Dave Camp
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA. 
 * 
 * Authors: Dave Camp <dave@ximian.com>
 *          Gustavo Gir�ldez <gustavo.giraldez@gmx.net>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <glib-object.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <gio/gio.h>


#include "project.h"
#include "project-util.h"
#include "project-model.h"


struct _GbfProjectModelPrivate {
	AnjutaPmProject      *proj;
	gulong               project_updated_handler;
	
	GtkTreeRowReference *root_group;
	GList               *shortcuts;

	gboolean default_shortcut;	   /* Add shortcut for each primary node */
};

enum {
	PROP_NONE,
	PROP_PROJECT
};


/* Function prototypes ------------- */

static void     gbf_project_model_class_init         (GbfProjectModelClass   *klass);
static void     gbf_project_model_instance_init      (GbfProjectModel        *tree);

static void     load_project                         (GbfProjectModel        *model,
						      AnjutaPmProject  *proj);
static void     insert_empty_node                    (GbfProjectModel        *model);
static void     unload_project                       (GbfProjectModel        *model);

static gint     default_sort_func                    (GtkTreeModel           *model,
						      GtkTreeIter            *iter_a,
						      GtkTreeIter            *iter_b,
						      gpointer                user_data);

static GtkTreeStoreClass *parent_class = NULL;


/* Implementation ---------------- */

/* Helper functions */

/* Type & interfaces initialization */

static void
gbf_project_model_class_init_trampoline (gpointer klass,
					 gpointer data)
{
	parent_class = g_type_class_ref (GTK_TYPE_TREE_STORE);
	gbf_project_model_class_init (klass);
}

GType
gbf_project_model_get_type (void)
{
	static GType object_type = 0;
	if (object_type == 0) {
		static const GTypeInfo object_info = {
		    sizeof (GbfProjectModelClass),
		    NULL,		/* base_init */
		    NULL,		/* base_finalize */
		    gbf_project_model_class_init_trampoline,
		    NULL,		/* class_finalize */
		    NULL,               /* class_data */
		    sizeof (GbfProjectModel),
		    0,                  /* n_preallocs */
		    (GInstanceInitFunc) gbf_project_model_instance_init
		};

		object_type = g_type_register_static (
			GTK_TYPE_TREE_STORE, "GbfProjectModel",
			&object_info, 0);
	}
	return object_type;
}

static void 
get_property (GObject    *object,
	      guint       prop_id,
	      GValue     *value,
	      GParamSpec *pspec)
{
        GbfProjectModel *model = GBF_PROJECT_MODEL (object);
        
        switch (prop_id) {
        case PROP_PROJECT:
                g_value_set_pointer (value, model->priv->proj);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void 
set_property (GObject      *object,
	      guint         prop_id,
	      const GValue *value,
	      GParamSpec   *pspec)
{
        GbfProjectModel *model = GBF_PROJECT_MODEL (object);

        switch (prop_id) {
        case PROP_PROJECT:
		gbf_project_model_set_project (model, g_value_get_pointer (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }   
}

static void
dispose (GObject *obj)
{
	GbfProjectModel *model = GBF_PROJECT_MODEL (obj);

	if (model->priv->proj) {
		unload_project (model);
	}

	G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
finalize (GObject *obj)
{
	GbfProjectModel *model = GBF_PROJECT_MODEL (obj);

	g_free (model->priv);

	G_OBJECT_CLASS (parent_class)->dispose (obj);
}

static void 
gbf_project_model_class_init (GbfProjectModelClass *klass)
{
	parent_class = g_type_class_peek_parent (klass);

	G_OBJECT_CLASS (klass)->dispose = dispose;
	G_OBJECT_CLASS (klass)->finalize = finalize;
	G_OBJECT_CLASS (klass)->get_property = get_property;
	G_OBJECT_CLASS (klass)->set_property = set_property;

	g_object_class_install_property 
                (G_OBJECT_CLASS (klass), PROP_PROJECT,
                 g_param_spec_pointer ("project", 
                                       _("Project"),
                                       _("GbfProject Object"),
                                       G_PARAM_READWRITE));
}

static void
gbf_project_model_instance_init (GbfProjectModel *model)
{
	static GType types [GBF_PROJECT_MODEL_NUM_COLUMNS];

	types [GBF_PROJECT_MODEL_COLUMN_DATA] = G_TYPE_POINTER;

	gtk_tree_store_set_column_types (GTK_TREE_STORE (model),
					 GBF_PROJECT_MODEL_NUM_COLUMNS,
					 types);

	model->priv = g_new0 (GbfProjectModelPrivate, 1);
	model->priv->default_shortcut = TRUE;

	/* sorting function */
	gtk_tree_sortable_set_default_sort_func (GTK_TREE_SORTABLE (model),
						 default_sort_func,
						 NULL, NULL);
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (model),
					      GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
					      GTK_SORT_ASCENDING);

	insert_empty_node (model);
}

GtkTreeRowReference *
gbf_project_model_get_root (GbfProjectModel *model)
{
	if (model->priv->root_group == NULL)
	{
		GtkTreeIter iter;
		gboolean valid;

		/* Search root group */
		for (valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &iter, NULL); valid; valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (model), &iter))
		{
			GbfTreeData *data;
			
			gtk_tree_model_get (GTK_TREE_MODEL (model), &iter,
	   			 GBF_PROJECT_MODEL_COLUMN_DATA, &data,
		    		-1);
			
			if (data->type == GBF_TREE_NODE_GROUP)
			{
				GtkTreePath *path;
				
				path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), &iter);
				model->priv->root_group = gtk_tree_row_reference_new (GTK_TREE_MODEL (model), path);
				gtk_tree_path_free (path);
			}
		}
	}

	return model->priv->root_group;
}

/* Model data functions ------------ */

/* Remove node without checking its shortcuts */
static gboolean
gbf_project_model_remove_children (GbfProjectModel *model, GtkTreeIter *iter)
{
	GtkTreeIter child;
	GbfTreeData *data;
	gboolean valid;

	/* Free all children */
	valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &child, iter);
	while (valid)
	{
		valid = gbf_project_model_remove_children (model, &child);
		
		/* Free children node */
		gtk_tree_model_get (GTK_TREE_MODEL (model), &child,
		   	 GBF_PROJECT_MODEL_COLUMN_DATA, &data,
		    	-1);
		valid = gtk_tree_store_remove (GTK_TREE_STORE (model), &child);
		if (data != NULL) gbf_tree_data_free (data);
	}

	return valid;
}

static gboolean
gbf_project_model_invalidate_children (GbfProjectModel *model, GtkTreeIter *iter)
{
	GtkTreeIter child;
	GbfTreeData *data;
	gboolean valid;

	/* Mark all children as invalid */
	valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &child, iter);
	while (valid)
	{
		valid = gbf_project_model_invalidate_children (model, &child);
		
		/* Invalidate children node */
		gtk_tree_model_get (GTK_TREE_MODEL (model), &child,
		   	 GBF_PROJECT_MODEL_COLUMN_DATA, &data,
		    	-1);
		gbf_tree_data_invalidate (data);

		valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (model), &child);
	}

	return valid;
}

static gboolean
gbf_project_model_remove_invalid_shortcut (GbfProjectModel *model, GtkTreeIter *iter)
{
	GtkTreeIter child;
	gboolean valid;
	GbfTreeData *data;

	/* Get all shortcut */
	valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &child, iter);
	while (valid)
	{
		gtk_tree_model_get (GTK_TREE_MODEL (model), &child,
	   		 GBF_PROJECT_MODEL_COLUMN_DATA, &data,
	    		-1);
		/* Shortcuts are always at the beginning */
		if (data->type != GBF_TREE_NODE_SHORTCUT) break;
		
		if (data->shortcut->type == GBF_TREE_NODE_INVALID)
		{
			gbf_project_model_remove_children (model, &child);
			valid = gtk_tree_store_remove (GTK_TREE_STORE (model), &child);
			if (data != NULL) gbf_tree_data_free (data);
		}
		else
		{
			gbf_project_model_remove_invalid_shortcut (model, &child); 
			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (model), &child);
		}
	}
		
	return FALSE;
}

gboolean
gbf_project_model_remove (GbfProjectModel *model, GtkTreeIter *iter)
{
	GtkTreeIter child;
	GbfTreeData *data;
	gboolean valid;

	/* Check if node is not a shortcut. In this case we need to remove
	 * all shortcuts first. */
	gtk_tree_model_get (GTK_TREE_MODEL (model), iter,
		    GBF_PROJECT_MODEL_COLUMN_DATA, &data,
		    -1);
	if (data->type != GBF_TREE_NODE_SHORTCUT)
	{
		/* Mark all nodes those will be removed */
		gbf_project_model_invalidate_children (model, iter);
		gbf_tree_data_invalidate (data);

		gbf_project_model_remove_invalid_shortcut (model, NULL);
	}
	
	/* Free all children */
	valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &child, iter);
	while (valid)
	{
		valid = gbf_project_model_remove_children (model, &child);
	}

	/* Free parent node */
	valid = gtk_tree_store_remove (GTK_TREE_STORE (model), iter);
	if (data != NULL) gbf_tree_data_free (data);

	return valid;
}


static void
gbf_project_model_clear (GbfProjectModel *model)
{
	GtkTreeIter child;
	gboolean valid;

	valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &child, NULL);
	while (valid)
	{
		valid = gbf_project_model_remove (model, &child);
	}
}

static gint 
default_sort_func (GtkTreeModel *model,
		   GtkTreeIter  *iter_a,
		   GtkTreeIter  *iter_b,
		   gpointer      user_data)
{
	GbfTreeData *data_a, *data_b;
	gint retval = 0;
	gboolean unsorted_a, unsorted_b;
	
	gtk_tree_model_get (model, iter_a,
			    GBF_PROJECT_MODEL_COLUMN_DATA, &data_a,
			    -1);
	gtk_tree_model_get (model, iter_b,
			    GBF_PROJECT_MODEL_COLUMN_DATA, &data_b,
			    -1);

	unsorted_a = (data_a->type == GBF_TREE_NODE_SHORTCUT) || (data_a->type == GBF_TREE_NODE_UNKNOWN) || (data_a->is_shortcut);
	unsorted_b = (data_b->type == GBF_TREE_NODE_SHORTCUT) || (data_b->type == GBF_TREE_NODE_UNKNOWN) || (data_b->is_shortcut);
	if (unsorted_a && unsorted_b) {
		GtkTreeIter iter;
		gboolean valid;
		
		/* special case: the order of shortcuts is
		 * user customizable */
		for (valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (model), &iter);
		    valid == TRUE;
		    valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (model), &iter))
		{
			GbfTreeData *data;
			
			gtk_tree_model_get (model, &iter,
				    GBF_PROJECT_MODEL_COLUMN_DATA, &data,
				    -1);
			if (data == data_a) {
				/* a comes first */
				retval = -1;
				break;
			}
			else if (data == data_b) {
				/* b comes first */
				retval = 1;
				break;
			}
		}
		
	} else if (unsorted_a && !unsorted_b) {
		retval = -1;
	 
	} else if (!unsorted_a && unsorted_b) {
		retval = 1;
	 
	} else if (data_a->type == data_b->type) {
		retval = strcmp (data_a->name, data_b->name);

	} else {
		/* assume a->b and check for the opposite cases */
		retval = -1;
		retval = data_a->type < data_b->type ? -1 : 1;
	}
	
	return retval;
}


void
gbf_project_model_add_source (GbfProjectModel    	      *model,
	    AnjutaProjectNode *source,
	    GtkTreeIter               *parent)
{
	GtkTreeIter iter;
	GbfTreeData *data;

	if ((!source) || (anjuta_project_node_get_node_type (source) != ANJUTA_PROJECT_SOURCE))
		return;
	
	data = gbf_tree_data_new_source (source);
	gtk_tree_store_append (GTK_TREE_STORE (model), &iter, parent);
	gtk_tree_store_set (GTK_TREE_STORE (model), &iter, 
			    GBF_PROJECT_MODEL_COLUMN_DATA, data,
			    -1);
}

void 
gbf_project_model_add_target_shortcut (GbfProjectModel *model,
                     GtkTreeIter     *shortcut,
		     GbfTreeData     *target,
		     GtkTreePath     *before_path,
                     gboolean	     *expanded)
{
	AnjutaProjectNode *node;
	GtkTreeIter iter, sibling;
	GtkTreePath *root_path;
	GbfTreeData *data;
	AnjutaProjectNode *parent;
	gboolean valid = FALSE;
	
	if (!target)
		return;

	if (expanded != NULL) *expanded = FALSE;

	root_path = gtk_tree_row_reference_get_path (gbf_project_model_get_root (model));
	if ((before_path == NULL) && (target->type != GBF_TREE_NODE_SHORTCUT))
	{
		/* Check is a proxy node is not already existing. It is used to
		 * save the shortcut order */

		for (valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &iter, NULL);
			valid;
			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (model), &iter))
		{
			GbfTreeData *data;

			/* Look for current node */
			gtk_tree_model_get (GTK_TREE_MODEL (model), &iter,
				GBF_PROJECT_MODEL_COLUMN_DATA, &data,
				-1);

			if (((data->type == GBF_TREE_NODE_UNKNOWN) || (data->type == GBF_TREE_NODE_SHORTCUT)) && (g_strcmp0 (target->name, data->name) == 0))
			{
				/* Find already existing node and replace it */
				if (expanded != NULL) *expanded = data->expanded;
				gbf_tree_data_free (data);
				
				data = gbf_tree_data_new_shortcut (target);
				gtk_tree_store_set (GTK_TREE_STORE (model), &iter, 
				    GBF_PROJECT_MODEL_COLUMN_DATA, data,
				    -1);
				break;
			}
		}
	}
	if (!valid)
	{
		/* check before_path */
		if ((before_path == NULL) ||
		    gtk_tree_path_get_depth (before_path) > 1 ||
		    gtk_tree_path_compare (before_path, root_path) > 0)
		{
			before_path = root_path;
		}
		
		/* get the tree iter for the row before which to insert the shortcut */
		if (!gtk_tree_model_get_iter (GTK_TREE_MODEL (model), &sibling, before_path)) {
			gtk_tree_path_free (root_path);
			return;
		}

		if (target->type != GBF_TREE_NODE_SHORTCUT)
		{
			data = gbf_tree_data_new_shortcut (target);
		}
		else
		{
			data = target;
		}
		gtk_tree_store_insert_before (GTK_TREE_STORE (model), &iter, NULL, &sibling);
		gtk_tree_store_set (GTK_TREE_STORE (model), &iter, 
				    GBF_PROJECT_MODEL_COLUMN_DATA, data,
				    -1);
	}
	
	/* add sources */
	parent = gbf_tree_data_get_node (target);
	for (node = anjuta_project_node_first_child (parent); node; node = anjuta_project_node_next_sibling (node))
		gbf_project_model_add_source (model, node, &iter);

	gtk_tree_path_free (root_path);

	if (shortcut) *shortcut = iter;
}

void 
gbf_project_model_move_target_shortcut (GbfProjectModel *model,
		     GtkTreeIter     *iter,
    		     GbfTreeData     *shortcut,
		     GtkTreePath     *before_path)
{
	AnjutaProjectNode *node;
	GtkTreeIter sibling;
	GtkTreePath *root_path;
	GtkTreePath *src_path;
	AnjutaProjectNode *parent;
	
	if (!shortcut)
		return;

	root_path = gtk_tree_row_reference_get_path (gbf_project_model_get_root (model));
	
	/* check before_path */
	if (!before_path ||
	    gtk_tree_path_get_depth (before_path) > 1)
	{
		/* Missing destination path, use root path */
		before_path = root_path;
	}
	else if (gtk_tree_path_compare (before_path, root_path) > 0)
	{
		/* Destination path outside shortcut are, remove shortcut */
		gbf_project_model_remove (model, iter);
		gtk_tree_path_free (root_path);
		
		return;
	}
		
	/* get the tree iter for the row before which to insert the shortcut */
	if (!gtk_tree_model_get_iter (GTK_TREE_MODEL (model), &sibling, before_path)) {
		gtk_tree_path_free (root_path);
		return;
	}

	src_path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), iter);
	if (gtk_tree_path_compare (src_path, before_path) != 0)
	{
		gtk_tree_store_remove (GTK_TREE_STORE (model), iter);			
		gtk_tree_store_insert_before (GTK_TREE_STORE (model), iter, NULL, &sibling);
		gtk_tree_store_set (GTK_TREE_STORE (model), iter, 
				    GBF_PROJECT_MODEL_COLUMN_DATA, shortcut,
				    -1);

		/* add sources */
		parent = gbf_tree_data_get_node (shortcut->shortcut);
		for (node = anjuta_project_node_first_child (parent); node; node = anjuta_project_node_next_sibling (node))
			gbf_project_model_add_source (model, node, iter);
	}

	gtk_tree_path_free (src_path);
	gtk_tree_path_free (root_path);

}

void
gbf_project_model_add_package (GbfProjectModel    	      *model,
	    AnjutaProjectNode *package,
	    GtkTreeIter               *parent)
{
	GtkTreeIter iter;
	GbfTreeData *data;
	AnjutaProjectNode *node;

	if ((!package) || (anjuta_project_node_get_node_type (package) != ANJUTA_PROJECT_PACKAGE))
		return;
	
	data = gbf_tree_data_new_package (package);
	gtk_tree_store_append (GTK_TREE_STORE (model), &iter, parent);
	gtk_tree_store_set (GTK_TREE_STORE (model), &iter, 
			    GBF_PROJECT_MODEL_COLUMN_DATA, data,
			    -1);
	
	/* ... and sources */
	for (node = anjuta_project_node_first_child (package); node; node = anjuta_project_node_next_sibling (node))
	{
		gbf_project_model_add_source (model, node, &iter);
	}
}

void
gbf_project_model_add_module (GbfProjectModel 		*model,
	    AnjutaProjectNode   *module,
	    GtkTreeIter     	        *parent)
{
	AnjutaProjectNode *l;
	GtkTreeIter iter;	
	GbfTreeData *data;
	
	if ((!module) || (anjuta_project_node_get_node_type (module) != ANJUTA_PROJECT_MODULE))
		return;
	
	data = gbf_tree_data_new_module (module);
	gtk_tree_store_append (GTK_TREE_STORE (model), &iter, parent);
	gtk_tree_store_set (GTK_TREE_STORE (model), &iter, 
			    GBF_PROJECT_MODEL_COLUMN_DATA, data,
			    -1);
	
	/* add package */
	for (l = anjuta_project_node_first_child (module); l; l = anjuta_project_node_next_sibling (l))
	{
		gbf_project_model_add_package (model, l, &iter);
	}
}

void
gbf_project_model_add_target (GbfProjectModel 		*model,
	    AnjutaProjectNode   *target,
	    GtkTreeIter     	        *parent)
{
	AnjutaProjectNode *l;
	GtkTreeIter iter;	
	GbfTreeData *data;
	
	if ((!target) || (anjuta_project_node_get_node_type (target) != ANJUTA_PROJECT_TARGET))
		return;
	
	data = gbf_tree_data_new_target (target);
	gtk_tree_store_append (GTK_TREE_STORE (model), &iter, parent);
	gtk_tree_store_set (GTK_TREE_STORE (model), &iter, 
			    GBF_PROJECT_MODEL_COLUMN_DATA, data,
			    -1);
	
	/* add sources */
	for (l = anjuta_project_node_first_child (target); l; l = anjuta_project_node_next_sibling (l))
	{
		gbf_project_model_add_source (model, l, &iter);
	}

	/* Add shortcut if needed */
	if ((data != NULL) && model->priv->default_shortcut && (anjuta_project_node_get_full_type (target) & ANJUTA_PROJECT_PRIMARY))
	{
		gbf_project_model_add_target_shortcut (model, NULL, data, NULL, NULL);
	}
}

void
gbf_project_model_add_target_group (GbfProjectModel 	*model,
		  AnjutaProjectNode	*group,
		  GtkTreeIter     	*parent)
{
	GtkTreeIter iter;
	AnjutaProjectNode *node;
	GbfTreeData *data;

	if ((!group) || (anjuta_project_node_get_node_type (group) != ANJUTA_PROJECT_GROUP))
		return;
	
	data = gbf_tree_data_new_group (group);
	gtk_tree_store_append (GTK_TREE_STORE (model), &iter, parent);
	gtk_tree_store_set (GTK_TREE_STORE (model), &iter, 
			    GBF_PROJECT_MODEL_COLUMN_DATA, data,
			    -1);

	/* add groups ... */
	for (node = anjuta_project_node_first_child (group); node; node = anjuta_project_node_next_sibling (node))
		gbf_project_model_add_target_group (model, node, &iter);
	
	/* ... and targets */
	for (node = anjuta_project_node_first_child (group); node; node = anjuta_project_node_next_sibling (node))
		gbf_project_model_add_target (model, node, &iter);
	
	/* ... and sources */
	for (node = anjuta_project_node_first_child (group); node; node = anjuta_project_node_next_sibling (node))
		gbf_project_model_add_source (model, node, &iter);
}

void
gbf_project_model_add_root (GbfProjectModel 	*model,
    AnjutaProjectNode	*root,
    GtkTreeIter     	*parent)
{
	AnjutaProjectNode *node;

	if ((!root) || (anjuta_project_node_get_node_type (root) != ANJUTA_PROJECT_ROOT))
		return;

	/* create root reference */
	/*root_path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), &iter);
	model->priv->root_group = gtk_tree_row_reference_new (GTK_TREE_MODEL (model), root_path);
	gtk_tree_path_free (root_path);*/

	/* add groups ... */
	for (node = anjuta_project_node_first_child (root); node; node = anjuta_project_node_next_sibling (node))
		gbf_project_model_add_target_group (model, node, NULL);
	
	/* ... and module */
	for (node = anjuta_project_node_first_child (root); node; node = anjuta_project_node_next_sibling (node))
		gbf_project_model_add_module (model, node, NULL);
}

static void
load_project (GbfProjectModel *model, AnjutaPmProject *proj)
{
	model->priv->proj = proj;
	g_object_ref (proj);

	gbf_project_model_add_root (model, anjuta_pm_project_get_root (proj), NULL);
}

static void 
insert_empty_node (GbfProjectModel *model)
{
	GtkTreeIter iter;
	GbfTreeData *empty_node;

	empty_node = gbf_tree_data_new_string (_("No project loaded"));

	gtk_tree_store_append (GTK_TREE_STORE (model), &iter, NULL);
	gtk_tree_store_set (GTK_TREE_STORE (model), &iter,
			    GBF_PROJECT_MODEL_COLUMN_DATA, empty_node,
			    -1);
}

static void
unload_project (GbfProjectModel *model)
{
	if (model->priv->proj) {
		gtk_tree_row_reference_free (model->priv->root_group);
		model->priv->root_group = NULL;

		gbf_project_model_clear (model);

		g_list_free (model->priv->shortcuts);
		model->priv->shortcuts = NULL;

		//g_signal_handler_disconnect (anjuta_pm_project_get_project (model->priv->proj),
		//			     model->priv->project_updated_handler);
		//model->priv->project_updated_handler = 0;
		model->priv->proj = NULL;

		insert_empty_node (model);
	}
}

static gboolean 
recursive_find_tree_data (GtkTreeModel  *model,
		          GtkTreeIter   *iter,
		          GbfTreeData  	*data)
{
	GtkTreeIter tmp;
	gboolean retval = FALSE;

	tmp = *iter;
	
	do {
		GtkTreeIter child;
		GbfTreeData *tmp_data;
		
		gtk_tree_model_get (model, &tmp,
				    GBF_PROJECT_MODEL_COLUMN_DATA, &tmp_data, -1);
		if (gbf_tree_data_equal (tmp_data, data))
		{
			*iter = tmp;
			retval = TRUE;
		}
		
		if (gtk_tree_model_iter_children (model, &child, &tmp)) {
			if (recursive_find_tree_data (model, &child, data)) {
				*iter = child;
				retval = TRUE;
			}
		}
				
	} while (!retval && gtk_tree_model_iter_next (model, &tmp));

	return retval;
}

gboolean 
gbf_project_model_find_tree_data (GbfProjectModel 	*model,
			          GtkTreeIter     	*iter,
			          GbfTreeData  		*data)
{
	GtkTreeIter tmp_iter;
	gboolean retval = FALSE;
	
	if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (model), &tmp_iter)) {
		if (recursive_find_tree_data (GTK_TREE_MODEL (model), &tmp_iter, data)) {
			retval = TRUE;
			*iter = tmp_iter;
		}
	}
	
	return retval;
}

/* Can return shortcut node if exist */
gboolean 
gbf_project_model_find_file (GbfProjectModel 	*model,
    GtkTreeIter		*found,
    GtkTreeIter		*parent,
    AnjutaProjectNodeType type,
    GFile		*file)
{
	GtkTreeIter iter;
	gboolean valid;

	/* Search for direct children */
	for (valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &iter, parent); valid == TRUE; valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (model), &iter))
	{
		GbfTreeData *data;
		
		gtk_tree_model_get (GTK_TREE_MODEL (model), &iter,
		    GBF_PROJECT_MODEL_COLUMN_DATA, &data, -1);

		if (gbf_tree_data_equal_file (data, type, file))
		{
			*found = iter;
			break;
		}
	}

	/* Search for children of children */
	if (!valid)
	{
		for (valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &iter, parent); valid == TRUE; valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (model), &iter))
		{
			if (gbf_project_model_find_file (model, found, &iter, type, file)) break;
		}
	}

	return valid;
}

gboolean 
gbf_project_model_find_node (GbfProjectModel 	*model,
    GtkTreeIter		*found,
    GtkTreeIter		*parent,
    AnjutaProjectNode	*node)
{
	GtkTreeIter iter;
	gboolean valid;

	/* Search for direct children */
	for (valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &iter, parent); valid == TRUE; valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (model), &iter))
	{
		GbfTreeData *data;
		
		gtk_tree_model_get (GTK_TREE_MODEL (model), &iter,
			GBF_PROJECT_MODEL_COLUMN_DATA, &data, -1);

		if (node == gbf_tree_data_get_node (data))
		{
			*found = iter;
			break;
		}
	}

	/* Search for children of children */
	if (!valid)
	{
		for (valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &iter, parent); valid == TRUE; valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (model), &iter))
		{
			if (gbf_project_model_find_node (model, found, &iter, node)) break;
		}
	}

	return valid;
}

/* Can return shortcut node if exist */
gboolean 
gbf_project_model_find_child_name (GbfProjectModel 	*model,
	GtkTreeIter		*found,
	GtkTreeIter		*parent,
        const gchar		*name)                        
{
	GtkTreeIter iter;
	gboolean valid;

	/* Search for direct children only */
	for (valid = gtk_tree_model_iter_children (GTK_TREE_MODEL (model), &iter, parent); valid == TRUE; valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (model), &iter))
	{
		GbfTreeData *data;
		
		gtk_tree_model_get (GTK_TREE_MODEL (model), &iter,
		    GBF_PROJECT_MODEL_COLUMN_DATA, &data, -1);

		if (gbf_tree_data_equal_name (data, name))
		{
			*found = iter;
			break;
		}
	}

	return valid;
}

GbfProjectModel *
gbf_project_model_new (AnjutaPmProject *project)
{
	return GBF_PROJECT_MODEL (g_object_new (GBF_TYPE_PROJECT_MODEL,
						"project", project,
						NULL));
}

void 
gbf_project_model_set_project (GbfProjectModel *model, AnjutaPmProject *project)
{
	g_return_if_fail (model != NULL && GBF_IS_PROJECT_MODEL (model));
	
	if (model->priv->proj != project)
	{
		//unload_project (model);

		/* project can be NULL */
		if (project)
			load_project (model, project);
	}
}

AnjutaPmProject *
gbf_project_model_get_project (GbfProjectModel *model)
{
	g_return_val_if_fail (model != NULL && GBF_IS_PROJECT_MODEL (model), NULL);
	
	return model->priv->proj;
}

GtkTreePath *
gbf_project_model_get_project_root (GbfProjectModel *model)
{
	GtkTreePath *path = NULL;
	
	g_return_val_if_fail (GBF_IS_PROJECT_MODEL (model), NULL);

	if (gbf_project_model_get_root (model))
		path = gtk_tree_row_reference_get_path (model->priv->root_group);
	
	return path;
}

AnjutaProjectNode *
gbf_project_model_get_node (GbfProjectModel *model,
                            GtkTreeIter     *iter)
{
	GbfTreeData *data = NULL;
	
	gtk_tree_model_get (GTK_TREE_MODEL (model), iter,
			    GBF_PROJECT_MODEL_COLUMN_DATA, &data,
			    -1);

	return gbf_tree_data_get_node (data);
}

void
gbf_project_model_add_shortcut (GbfProjectModel *model,
                                GtkTreeIter     *iter,
                                GtkTreeIter     *before, 
                                GbfTreeData     *target)
{
	GtkTreePath *path;

	path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), before);
	gbf_project_model_add_target_shortcut (model, iter, target, path, NULL);
	gtk_tree_path_free (path);
}

void
gbf_project_model_set_default_shortcut (GbfProjectModel *model,
                                        gboolean enable)
{
	model->priv->default_shortcut = enable;
}