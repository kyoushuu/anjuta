/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4; coding: utf-8 -*- */
/* am-project.c
 *
 * Copyright (C) 2009  Sébastien Granjoux
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "am-project.h"

#include "am-project-private.h"
#include "am-node.h"

#include <libanjuta/interfaces/ianjuta-project.h>
#include <libanjuta/anjuta-debug.h>
#include <libanjuta/anjuta-utils.h>

#include <string.h>
#include <memory.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <signal.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <glib.h>

#include "ac-scanner.h"
#include "ac-writer.h"
#include "am-scanner.h"
#include "am-writer.h"
//#include "am-config.h"
#include "am-properties.h"


#define UNIMPLEMENTED  G_STMT_START { g_warning (G_STRLOC": unimplemented"); } G_STMT_END

/* Constant strings for parsing perl script error output */
#define ERROR_PREFIX      "ERROR("
#define WARNING_PREFIX    "WARNING("
#define MESSAGE_DELIMITER ": "

static const gchar *valid_am_makefiles[] = {"GNUmakefile.am", "makefile.am", "Makefile.am", NULL};

/* convenient shortcut macro the get the AnjutaProjectNode from a GNode */
#define AMP_NODE_DATA(node)  ((AnjutaProjectNode *)node)
#define AMP_GROUP_DATA(node)  ((AnjutaAmGroupNode *)node)
#define AMP_TARGET_DATA(node)  ((AnjutaAmTargetNode *)node)
#define AMP_SOURCE_DATA(node)  ((AnjutaAmSourceNode *)node)
#define AMP_PACKAGE_DATA(node)  ((AnjutaAmPackageNode *)node)
#define AMP_MODULE_DATA(node)  ((AnjutaAmModuleNode *)node)
#define AMP_ROOT_DATA(node)  ((AnjutaAmRootNode *)node)

#define STR_REPLACE(target, source) \
	{ g_free (target); target = source == NULL ? NULL : g_strdup (source);}


typedef struct _AmpConfigFile AmpConfigFile;

struct _AmpConfigFile {
	GFile *file;
	AnjutaToken *token;
};

typedef struct _AmpNodeInfo AmpNodeInfo;

struct _AmpNodeInfo {
	AnjutaProjectNodeInfo base;
	AnjutaTokenType token;
	const gchar *prefix;
	const gchar *install;
};

struct _AmpTargetPropertyBuffer {
	GList *sources;
	GList *properties;
};

/* Node types
 *---------------------------------------------------------------------------*/

static AmpNodeInfo AmpNodeInformations[] = {
	{{ANJUTA_PROJECT_GROUP,
	N_("Group"),
	"text/plain"},
	ANJUTA_TOKEN_NONE,
	NULL,
	NULL},

	{{ANJUTA_PROJECT_SOURCE,
	N_("Source"),
	"text/plain"},
	ANJUTA_TOKEN_NONE,
	NULL,
	NULL},

	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_UNKNOWN,
	N_("Unknown"),
	"text/plain"},
	ANJUTA_TOKEN_NONE,
	NULL,
	NULL},

	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_PRIMARY |  ANJUTA_PROJECT_SHAREDLIB,
	N_("Shared Library"),
	"application/x-sharedlib"},
	AM_TOKEN__LTLIBRARIES,
	"_LTLIBRARIES",
	"lib"},
	
	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_PRIMARY |  ANJUTA_PROJECT_STATICLIB,
	N_("Static Library"),
	"application/x-archive"},
	AM_TOKEN__LIBRARIES,
	"_LIBRARIES",
	"lib"},

	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_PRIMARY | ANJUTA_PROJECT_EXECUTABLE | ANJUTA_PROJECT_BINARY,
	N_("Program"),
	"application/x-executable"},
	AM_TOKEN__PROGRAMS,
	"_PROGRAMS",
	"bin"},

	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_PYTHON,
	N_("Python Module"),
	"application/x-python"},
	AM_TOKEN__PYTHON,
	"_PYTHON",
	NULL},
	
	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_JAVA,
	N_("Java Module"),
	"application/x-java"},
	AM_TOKEN__JAVA,
	"_JAVA",
	NULL},
	
	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_UNKNOWN,
	N_("Lisp Module"),
	"text/plain"},
	AM_TOKEN__LISP,
	"_LISP",
	"lisp"},
	
	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_UNKNOWN,
	N_("Header Files"),
	"text/x-chdr"},
	AM_TOKEN__HEADERS,
	"_HEADERS",
	"include"},
	
	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_UNKNOWN,
	N_("Man Documentation"),
	"text/x-troff-man"},
	AM_TOKEN__MANS,
	"_MANS",
	"man"},

	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_UNKNOWN,
	N_("Info Documentation"),
	"application/x-tex-info"},
	AM_TOKEN__TEXINFOS,
	"_TEXINFOS",
	"info"},
	
	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_UNKNOWN,
	N_("Miscellaneous Data"),
	"application/octet-stream"},
	AM_TOKEN__DATA,
	"_DATA",
	"data"},
	
	{{ANJUTA_PROJECT_TARGET | ANJUTA_PROJECT_EXECUTABLE | ANJUTA_PROJECT_SCRIPT,
	N_("Script"),
	"text/x-shellscript"},
	AM_TOKEN__SCRIPTS,
	"_SCRIPTS",
	"bin"},

	{{ANJUTA_PROJECT_MODULE,
	N_("Module"),
	""},
	ANJUTA_TOKEN_NONE,
	NULL,
	NULL},

	{{ANJUTA_PROJECT_PACKAGE,
	N_("Package"),
	""},
	ANJUTA_TOKEN_NONE,
	NULL,
	NULL},

	{{ANJUTA_PROJECT_UNKNOWN,
	NULL,
	NULL},
	ANJUTA_TOKEN_NONE,
	NULL,
	NULL}
};

/* Properties
 *---------------------------------------------------------------------------*/




/* ----- Standard GObject types and variables ----- */

enum {
	PROP_0,
	PROP_PROJECT_DIR
};

static GObject *parent_class;

/* Work even if file is not a descendant of parent */
static gchar*
get_relative_path (GFile *parent, GFile *file)
{
	gchar *relative;

	relative = g_file_get_relative_path (parent, file);
	if (relative == NULL)
	{
		if (g_file_equal (parent, file))
		{
			relative = g_strdup ("");
		}
		else
		{
			GFile *grand_parent = g_file_get_parent (parent);
			gint level;
			gchar *grand_relative;
			gchar *ptr;
			gsize len;
			
			
			for (level = 1;  !g_file_has_prefix (file, grand_parent); level++)
			{
				GFile *next = g_file_get_parent (grand_parent);
				
				g_object_unref (grand_parent);
				grand_parent = next;
			}

			grand_relative = g_file_get_relative_path (grand_parent, file);
			g_object_unref (grand_parent);

			len = strlen (grand_relative);
			relative = g_new (gchar, len + level * 3 + 1);
			ptr = relative;
			for (; level; level--)
			{
				memcpy(ptr, ".." G_DIR_SEPARATOR_S, 3);
				ptr += 3;
			}
			memcpy (ptr, grand_relative, len + 1);
			g_free (grand_relative);
		}
	}

	return relative;
}

static GFileType
file_type (GFile *file, const gchar *filename)
{
	GFile *child;
	GFileInfo *info;
	GFileType type;

	child = filename != NULL ? g_file_get_child (file, filename) : g_object_ref (file);

	//g_message ("check file %s", g_file_get_path (child));
	
	info = g_file_query_info (child,
	                          G_FILE_ATTRIBUTE_STANDARD_TYPE, 
	                          G_FILE_QUERY_INFO_NONE,
	                          NULL,
	                          NULL);
	if (info != NULL)
	{
		type = g_file_info_get_file_type (info);
		g_object_unref (info);
	}
	else
	{
		type = G_FILE_TYPE_UNKNOWN;
	}
	
	g_object_unref (child);
	
	return type;
}

/* Automake parsing function
 *---------------------------------------------------------------------------*/

/* Remove invalid character according to automake rules */
static gchar*
canonicalize_automake_variable (gchar *name)
{
	gchar *canon_name = g_strdup (name);
	gchar *ptr;
	
	for (ptr = canon_name; *ptr != '\0'; ptr++)
	{
		if (!g_ascii_isalnum (*ptr) && (*ptr != '@'))
		{
			*ptr = '_';
		}
	}

	return canon_name;
}

static gboolean
split_automake_variable (gchar *name, gint *flags, gchar **module, gchar **primary)
{
	GRegex *regex;
	GMatchInfo *match_info;
	gint start_pos;
	gint end_pos;

	regex = g_regex_new ("(nobase_|notrans_)?"
	                     "(dist_|nodist_)?"
	                     "(noinst_|check_|man_|man[0-9al]_)?"
	                     "(.*_)?"
	                     "([^_]+)",
	                     G_REGEX_ANCHORED,
	                     G_REGEX_MATCH_ANCHORED,
	                     NULL);

	if (!g_regex_match (regex, name, G_REGEX_MATCH_ANCHORED, &match_info)) return FALSE;

	if (flags)
	{
		*flags = 0;
		g_match_info_fetch_pos (match_info, 1, &start_pos, &end_pos);
		if (start_pos >= 0)
		{
			if (*(name + start_pos + 2) == 'b') *flags |= AM_TARGET_NOBASE;
			if (*(name + start_pos + 2) == 't') *flags |= AM_TARGET_NOTRANS;
		}

		g_match_info_fetch_pos (match_info, 2, &start_pos, &end_pos);
		if (start_pos >= 0)
		{
			if (*(name + start_pos) == 'd') *flags |= AM_TARGET_DIST;
			if (*(name + start_pos) == 'n') *flags |= AM_TARGET_NODIST;
		}

		g_match_info_fetch_pos (match_info, 3, &start_pos, &end_pos);
		if (start_pos >= 0)
		{
			if (*(name + start_pos) == 'n') *flags |= AM_TARGET_NOINST;
			if (*(name + start_pos) == 'c') *flags |= AM_TARGET_CHECK;
			if (*(name + start_pos) == 'm')
			{
				gchar section = *(name + end_pos - 1);
				*flags |= AM_TARGET_MAN;
				if (section != 'n') *flags |= (section & 0x1F) << 7;
			}
		}
	}

	if (module)
	{
		g_match_info_fetch_pos (match_info, 4, &start_pos, &end_pos);
		if (start_pos >= 0)
		{
			*module = name + start_pos;
			*(name + end_pos - 1) = '\0';
		}
		else
		{
			*module = NULL;
		}
	}

	if (primary)
	{
		g_match_info_fetch_pos (match_info, 5, &start_pos, &end_pos);
		if (start_pos >= 0)
		{
			*primary = name + start_pos;
		}
		else
		{
			*primary = NULL;
		}
	}

	g_regex_unref (regex);

	return TRUE;
}

static gchar*
ac_init_default_tarname (const gchar *name)
{
	gchar *tarname;

	if (name == NULL) return NULL;
	
	/* Remove GNU prefix */
	if (strncmp (name, "GNU ", 4) == 0) name += 4;

	tarname = g_ascii_strdown (name, -1);
	g_strcanon (tarname, "abcdefghijklmnopqrstuvwxyz0123456789", '-');
	
	return tarname;
}

/* Properties buffer objects
 *---------------------------------------------------------------------------*/

AmpTargetPropertyBuffer*
amp_target_property_buffer_new (void)
{
	AmpTargetPropertyBuffer* buffer;

	buffer = g_new0 (AmpTargetPropertyBuffer, 1);

	return buffer;
}

void
amp_target_property_buffer_free (AmpTargetPropertyBuffer *buffer)
{
	g_list_foreach (buffer->sources, (GFunc)amp_source_free, NULL);
	g_list_free (buffer->sources);
	g_list_foreach (buffer->properties, (GFunc)amp_property_free, NULL);
	g_free (buffer);
}

void
amp_target_property_buffer_add_source (AmpTargetPropertyBuffer *buffer, AnjutaAmSourceNode *source)
{
	buffer->sources = g_list_prepend (buffer->sources, source);
}

void
amp_target_property_buffer_add_property (AmpTargetPropertyBuffer *buffer, AnjutaProjectProperty *prop)
{
	buffer->properties = g_list_prepend (buffer->properties, prop);
}

GList *
amp_target_property_buffer_steal_sources (AmpTargetPropertyBuffer *buffer)
{
	GList *list = buffer->sources;

	buffer->sources = NULL;

	return list;
}

GList *
amp_target_property_buffer_steal_properties (AmpTargetPropertyBuffer *buffer)
{
	GList *list = buffer->properties;

	buffer->properties = NULL;

	return list;
}


/* Config file objects
 *---------------------------------------------------------------------------*/

static AmpConfigFile*
amp_config_file_new (const gchar *pathname, GFile *project_root, AnjutaToken *token)
{
	AmpConfigFile *config;

	g_return_val_if_fail ((pathname != NULL) && (project_root != NULL), NULL);

	config = g_slice_new0(AmpConfigFile);
	config->file = g_file_resolve_relative_path (project_root, pathname);
	config->token = token;

	return config;
}

static void
amp_config_file_free (AmpConfigFile *config)
{
	if (config)
	{
		g_object_unref (config->file);
		g_slice_free (AmpConfigFile, config);
	}
}


/* Package objects
 *---------------------------------------------------------------------------*/



/* Module objects
 *---------------------------------------------------------------------------*/

static void
amp_project_new_module_hash (AmpProject *project)
{
	project->modules = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

static void
amp_project_free_module_hash (AmpProject *project)
{
	if (project->modules != NULL)
	{
		g_hash_table_destroy (project->modules);
		project->modules = NULL;
	}
}

/* Group objects
 *---------------------------------------------------------------------------*/

static void
remove_config_file (gpointer data, GObject *object, gboolean is_last_ref)
{
	if (is_last_ref)
	{
		AmpProject *project = (AmpProject *)data;
		g_hash_table_remove (project->files, anjuta_token_file_get_file (ANJUTA_TOKEN_FILE (object)));
	}
}

static gboolean 
amp_group_fill_token (AmpProject  *project, AnjutaAmGroupNode *group, GError **error)
{
	AnjutaAmGroupNode *last;
	GFile *directory;
	GFile *makefile;
	AnjutaToken *list;
	gchar *basename;
	gchar *uri;
	AnjutaTokenFile* tfile;
	AnjutaAmTargetNode *sibling;
	AnjutaAmGroupNode *parent;
	gboolean after;
	gchar *name;
	
	/* Check that the new group doesn't already exist */
	/*directory = g_file_get_child (AMP_GROUP_DATA (parent)->base.file, name);
	uri = g_file_get_uri (directory);
	if (g_hash_table_lookup (project->groups, uri) != NULL)
	{
		g_free (uri);
		error_set (error, IANJUTA_PROJECT_ERROR_DOESNT_EXIST,
			_("Group already exists"));
		return NULL;
	}*/

	/* Get parent target */
	parent = ANJUTA_AM_GROUP_NODE (anjuta_project_node_parent(ANJUTA_PROJECT_NODE (group)));
	name = anjuta_project_node_get_name (ANJUTA_PROJECT_NODE (group));
	directory = g_file_get_child (anjuta_project_node_get_file (ANJUTA_PROJECT_NODE (parent)), name);

	/* Find a sibling if possible */
	if (anjuta_project_node_prev_sibling (ANJUTA_PROJECT_NODE (group)) != NULL)
	{
		sibling = anjuta_project_node_prev_sibling (ANJUTA_PROJECT_NODE (group));
		after = TRUE;
	}
	else if (anjuta_project_node_next_sibling (ANJUTA_PROJECT_NODE (group)) != NULL)
	{
		sibling = anjuta_project_node_next_sibling (ANJUTA_PROJECT_NODE (group));
		after = FALSE;
	}
	else
	{
		sibling = NULL;
		after = TRUE;
	}
	
	/* Create directory */
	g_file_make_directory (directory, NULL, NULL);

	/* Create Makefile.am */
	basename = AMP_GROUP_DATA (parent)->makefile != NULL ? g_file_get_basename (AMP_GROUP_DATA (parent)->makefile) : NULL;
	if (basename != NULL)
	{
		makefile = g_file_get_child (directory, basename);
		g_free (basename);
	}
	else
	{
		makefile = g_file_get_child (directory, "Makefile.am");
	}
	g_file_replace_contents (makefile, "", 0, NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, NULL);
	tfile = amp_group_set_makefile (group, makefile, project);
	g_hash_table_insert (project->files, makefile, tfile);
	g_object_add_toggle_ref (G_OBJECT (tfile), remove_config_file, project);

	if (sibling == NULL)
	{
		/* Find a sibling before */
		for (last = anjuta_project_node_prev_sibling (group); (last != NULL) && (anjuta_project_node_get_node_type (last) != ANJUTA_PROJECT_GROUP); last = anjuta_project_node_prev_sibling (last));
		if (last != NULL)
		{
			sibling = last;
			after = TRUE;
		}
		else
		{
			/* Find a sibling after */
			for (last = anjuta_project_node_next_sibling (group); (last != NULL) && (anjuta_project_node_get_node_type (last) != ANJUTA_PROJECT_GROUP); last = anjuta_project_node_next_sibling (last));
			if (last != NULL)
			{
				sibling = last;
				after = FALSE;
			}
		}
	}
	
	/* Add in configure */
	list = NULL;
	if (sibling) list = amp_group_get_first_token (sibling, AM_GROUP_TOKEN_CONFIGURE);
	if (list == NULL) list= amp_group_get_first_token (parent, AM_GROUP_TOKEN_CONFIGURE);
	if (list != NULL) list = anjuta_token_list (list);
	if (list == NULL)
	{
		list = amp_project_write_config_list (project);
		list = anjuta_token_next (list);
	}
	if (list != NULL)
	{
		gchar *relative_make;
		gchar *ext;
		AnjutaToken *prev = NULL;

		if (sibling)
		{
			prev = amp_group_get_first_token (sibling, AM_GROUP_TOKEN_CONFIGURE);
			/*if ((prev != NULL) && after)
			{
				prev = anjuta_token_next_word (prev);
			}*/
		}
		//prev_token = (AnjutaToken *)token_list->data;

		relative_make = g_file_get_relative_path (anjuta_project_node_get_file (project->root), makefile);
		ext = relative_make + strlen (relative_make) - 3;
		if (strcmp (ext, ".am") == 0)
		{
			*ext = '\0';
		}
		//token = anjuta_token_new_string (ANJUTA_TOKEN_NAME | ANJUTA_TOKEN_ADDED,  relative_make);
		amp_project_write_config_file (project, list, after, prev, relative_make);
		g_free (relative_make);
		
		//style = anjuta_token_style_new (NULL," ","\n",NULL,0);
		//anjuta_token_add_word (prev_token, token, style);
		//anjuta_token_style_free (style);
	}

	/* Add in Makefile.am */
	if (sibling == NULL)
	{
		AnjutaToken *pos;
		static gint eol_type[] = {ANJUTA_TOKEN_EOL, ANJUTA_TOKEN_SPACE, ANJUTA_TOKEN_COMMENT, 0};
	
		pos = anjuta_token_find_type (AMP_GROUP_DATA (parent)->make_token, ANJUTA_TOKEN_SEARCH_NOT, eol_type);
		if (pos == NULL)
		{
			pos = anjuta_token_prepend_child (AMP_GROUP_DATA (parent)->make_token, anjuta_token_new_static (ANJUTA_TOKEN_SPACE, "\n"));
		}

		list = anjuta_token_insert_token_list (FALSE, pos,
		    	ANJUTA_TOKEN_SPACE, "\n");
		list = anjuta_token_insert_token_list (FALSE, list,
	    		AM_TOKEN_SUBDIRS, "SUBDIRS",
		    	ANJUTA_TOKEN_SPACE, " ",
		    	ANJUTA_TOKEN_OPERATOR, "=",
	    		ANJUTA_TOKEN_LIST, NULL,
	    		ANJUTA_TOKEN_LAST, NULL,
	    		NULL);
		list = anjuta_token_next (anjuta_token_next ( anjuta_token_next (list)));
	}
	else
	{
		AnjutaToken *prev;
		
		prev = amp_group_get_first_token (sibling, AM_GROUP_TOKEN_SUBDIRS);
		list = anjuta_token_list (prev);
	}

	if (list != NULL)
	{
		AnjutaToken *token;
		AnjutaToken *prev;

		if (sibling)
		{
			prev = amp_group_get_first_token (sibling, AM_GROUP_TOKEN_SUBDIRS);
		}
		
		token = anjuta_token_new_string (ANJUTA_TOKEN_NAME | ANJUTA_TOKEN_ADDED, name);
		if (after)
		{
			anjuta_token_insert_word_after (list, prev, token);
		}
		else
		{
			anjuta_token_insert_word_before (list, prev, token);
		}
	
		anjuta_token_style_format (project->am_space_list, list);
		anjuta_token_file_update (AMP_GROUP_DATA (parent)->tfile, token);
		
		amp_group_add_token (group, token, AM_GROUP_TOKEN_SUBDIRS);
	}
	g_free (name);

	return TRUE;
}

/* Target objects
 *---------------------------------------------------------------------------*/

static gboolean
find_target (AnjutaProjectNode *node, gpointer data)
{
	if ((AMP_NODE_DATA (node)->type  & ANJUTA_PROJECT_TYPE_MASK) == ANJUTA_PROJECT_TARGET)
	{
		if (strcmp (AMP_TARGET_DATA (node)->base.name, *(gchar **)data) == 0)
		{
			/* Find target, return node value in pointer */
			*(AnjutaProjectNode **)data = node;

			return TRUE;
		}
	}

	return FALSE;
}

static gboolean
find_canonical_target (AnjutaProjectNode *node, gpointer data)
{
	if ((AMP_NODE_DATA (node)->type  & ANJUTA_PROJECT_TYPE_MASK) == ANJUTA_PROJECT_TARGET)
	{
		gchar *canon_name = canonicalize_automake_variable (AMP_TARGET_DATA (node)->base.name);	
		DEBUG_PRINT ("compare canon %s vs %s node %p", canon_name, *(gchar **)data, node);
		if (strcmp (canon_name, *(gchar **)data) == 0)
		{
			/* Find target, return node value in pointer */
			*(AnjutaProjectNode **)data = node;
			g_free (canon_name);

			return TRUE;
		}
		g_free (canon_name);
	}

	return FALSE;
}

static gboolean 
amp_target_fill_token (AmpProject  *project, AnjutaAmTargetNode *target, GError **error)
{
	AnjutaToken* token;
	AnjutaToken *args;
	AnjutaToken *var;
	AnjutaToken *prev;
	AmpNodeInfo *info;
	gchar *targetname;
	gchar *find;
	gchar *name;
	GList *last;
	AnjutaAmTargetNode *sibling;
	AnjutaAmGroupNode *parent;
	gboolean after;

	/* Get parent target */
	parent = ANJUTA_AM_GROUP_NODE (anjuta_project_node_parent (ANJUTA_PROJECT_NODE (target)));
	
	info = (AmpNodeInfo *)amp_project_get_type_info (project, anjuta_project_node_get_full_type (target));
	name = anjuta_project_node_get_name (ANJUTA_PROJECT_NODE (target));

	/* Check that the new target doesn't already exist */
	/*find = name;
	anjuta_project_node_children_foreach (parent, find_target, &find);
	if ((gchar *)find != name)
	{
		g_free (name);
		error_set (error, IANJUTA_PROJECT_ERROR_DOESNT_EXIST,
		_("Target already exists"));

		return FALSE;
	}*/

	/* Find a sibling if possible */
	if (target->base.prev != NULL)
	{
		sibling = target->base.prev;
		after = TRUE;
	}
	else if (target->base.next != NULL)
	{
		sibling = target->base.next;
		after = FALSE;
	}
	else
	{
		sibling = NULL;
		after = TRUE;
	}
	
	/* Add in Makefile.am */
	targetname = g_strconcat (info->install, info->prefix, NULL);

	// Get token corresponding to sibling and check if the target are compatible
	args = NULL;
	var = NULL;
	prev = NULL;
	if (sibling != NULL)
	{
		last = amp_target_get_token (sibling);

		if (last != NULL) 
		{
			AnjutaToken *token = (AnjutaToken *)last->data;

			/* Check that the sibling is of the same kind */
			token = anjuta_token_list (token);
			if (token != NULL)
			{
				token = anjuta_token_list (token);
				var = token;
				if (token != NULL)
				{
					token = anjuta_token_first_item (token);
					if (token != NULL)
					{
						gchar *value;
						
						value = anjuta_token_evaluate (token);

						if ((value != NULL) && (strcmp (targetname, value) == 0))
						{
							g_free (value);
							prev = (AnjutaToken *)last->data;
							args = anjuta_token_list (prev);
						}
					}
				}
			}
		}
	}

	if (args == NULL)
	{
		for (last = amp_group_get_token (parent, AM_GROUP_TARGET); last != NULL; last = g_list_next (last))
		{
			gchar *value = anjuta_token_evaluate ((AnjutaToken *)last->data);
		
			if ((value != NULL) && (strcmp (targetname, value) == 0))
			{
				g_free (value);
				args = anjuta_token_last_item (anjuta_token_list ((AnjutaToken *)last->data));
				break;
			}
			g_free (value);
		}
	}


	if (args == NULL)
	{
		args = amp_project_write_target (AMP_GROUP_DATA (parent)->make_token, info->token, targetname, after, var);
	}
	g_free (targetname);
	
	if (args != NULL)
	{
		AnjutaTokenStyle *style;

		anjuta_token_dump (anjuta_token_list (args));
		
		token = anjuta_token_new_string (ANJUTA_TOKEN_ARGUMENT | ANJUTA_TOKEN_ADDED, name);
		if (after)
		{
			anjuta_token_insert_word_after (args, prev, token);
		}
		else
		{
			anjuta_token_insert_word_before (args, prev, token);
		}

		/* Try to use the same style than the current target list */
		style = anjuta_token_style_new_from_base (project->am_space_list);
		anjuta_token_style_update (style, args);
		anjuta_token_style_format (style, args);
		anjuta_token_style_free (style);
		anjuta_token_file_update (AMP_GROUP_DATA (parent)->tfile, token);
		
		amp_target_add_token (target, token);
	}
	g_free (name);

	return TRUE;
}

static gboolean 
amp_target_remove_token (AmpProject  *project, AnjutaAmTargetNode *target, GError **error)
{
	GList *item;
	AnjutaAmGroupNode *parent;

	/* Get parent target */
	parent = ANJUTA_AM_GROUP_NODE (anjuta_project_node_parent (ANJUTA_PROJECT_NODE (target)));

	for (item = amp_target_get_token (target); item != NULL; item = g_list_next (item))
	{
		AnjutaToken *token = (AnjutaToken *)item->data;
		AnjutaToken *args;
		AnjutaTokenStyle *style;

		args = anjuta_token_list (token);

		/* Try to use the same style than the current target list */
		style = anjuta_token_style_new_from_base (project->am_space_list);
		anjuta_token_style_update (style, args);
		
		anjuta_token_remove_word (token);
		
		anjuta_token_style_format (style, args);
		anjuta_token_style_free (style);
		
		anjuta_token_file_update (AMP_GROUP_DATA (parent)->tfile, args);
	}

	return TRUE;
}

/* Source objects
 *---------------------------------------------------------------------------*/

static gboolean 
amp_source_fill_token (AmpProject  *project, AnjutaAmSourceNode *source, GError **error)
{
	AnjutaAmGroupNode *group;
	AnjutaAmTargetNode *target;
	gboolean after;
	AnjutaToken *token;
	AnjutaToken *prev;
	AnjutaToken *args;
	gchar *relative_name;

	/* Get parent target */
	target = ANJUTA_AM_TARGET_NODE (anjuta_project_node_parent (ANJUTA_PROJECT_NODE (source)));
	if ((target == NULL) || (anjuta_project_node_get_node_type (target) != ANJUTA_PROJECT_TARGET)) return FALSE;
	
	group = ANJUTA_AM_GROUP_NODE (anjuta_project_node_parent (ANJUTA_PROJECT_NODE (target)));
	relative_name = g_file_get_relative_path (AMP_GROUP_DATA (group)->base.file, AMP_SOURCE_DATA (source)->base.file);

	/* Add in Makefile.am */
	/* Find a sibling if possible */
	if (source->base.prev != NULL)
	{
		prev = AMP_SOURCE_DATA (source->base.prev)->token;
		after = TRUE;
		args = anjuta_token_list (prev);
	}
	else if (source->base.next != NULL)
	{
		prev = AMP_SOURCE_DATA (source->base.next)->token;
		after = FALSE;
		args = anjuta_token_list (prev);
	}
	else
	{
		prev = NULL;
		args = NULL;
	}
	
	if (args == NULL)
	{
		gchar *target_var;
		gchar *canon_name;
		AnjutaToken *var;
		GList *list;
		
		canon_name = canonicalize_automake_variable (AMP_TARGET_DATA (target)->base.name);
		target_var = g_strconcat (canon_name,  "_SOURCES", NULL);

		/* Search where the target is declared */
		var = NULL;
		list = amp_target_get_token (target);
		if (list != NULL)
		{
			var = (AnjutaToken *)list->data;
			if (var != NULL)
			{
				var = anjuta_token_list (var);
				if (var != NULL)
				{
					var = anjuta_token_list (var);
				}
			}
		}
		
		args = amp_project_write_source_list (AMP_GROUP_DATA (group)->make_token, target_var, after, var);
		g_free (target_var);
	}
	
	if (args != NULL)
	{
		token = anjuta_token_new_string (ANJUTA_TOKEN_NAME | ANJUTA_TOKEN_ADDED, relative_name);
		if (after)
		{
			anjuta_token_insert_word_after (args, prev, token);
		}
		else
		{
			anjuta_token_insert_word_before (args, prev, token);
		}
	
		anjuta_token_style_format (project->am_space_list, args);
		anjuta_token_file_update (AMP_GROUP_DATA (group)->tfile, token);
	}
	
	AMP_SOURCE_DATA (source)->token = token;

	return TRUE;
}

/*
 * File monitoring support --------------------------------
 * FIXME: review these
 */
static void
monitor_cb (GFileMonitor *monitor,
			GFile *file,
			GFile *other_file,
			GFileMonitorEvent event_type,
			gpointer data)
{
	AmpProject *project = data;

	g_return_if_fail (project != NULL && AMP_IS_PROJECT (project));

	switch (event_type) {
		case G_FILE_MONITOR_EVENT_CHANGED:
		case G_FILE_MONITOR_EVENT_DELETED:
			/* monitor will be removed here... is this safe? */
			//amp_project_reload (project, NULL);
			g_message ("project updated");
			g_signal_emit_by_name (G_OBJECT (project), "project-updated");
			break;
		default:
			break;
	}
}


/*
 * ---------------- Data structures managment
 */

static void
amp_dump_node (AnjutaProjectNode *g_node)
{
	gchar *name = NULL;
	
	switch (AMP_NODE_DATA (g_node)->type & ANJUTA_PROJECT_TYPE_MASK) {
		case ANJUTA_PROJECT_GROUP:
			name = g_file_get_uri (AMP_GROUP_DATA (g_node)->base.file);
			DEBUG_PRINT ("GROUP: %s", name);
			break;
		case ANJUTA_PROJECT_TARGET:
			name = g_strdup (AMP_TARGET_DATA (g_node)->base.name);
			DEBUG_PRINT ("TARGET: %s", name);
			break;
		case ANJUTA_PROJECT_SOURCE:
			name = g_file_get_uri (AMP_SOURCE_DATA (g_node)->base.file);
			DEBUG_PRINT ("SOURCE: %s", name);
			break;
		default:
			g_assert_not_reached ();
			break;
	}
	g_free (name);
}

static void
foreach_node_destroy (AnjutaProjectNode    *g_node,
		      gpointer  data)
{
	AmpProject *project = (AmpProject *)data;
	
	switch (AMP_NODE_DATA (g_node)->type & ANJUTA_PROJECT_TYPE_MASK) {
		case ANJUTA_PROJECT_GROUP:
			//g_hash_table_remove (project->groups, g_file_get_uri (AMP_GROUP_NODE (g_node)->file));
			amp_group_free (g_node);
			break;
		case ANJUTA_PROJECT_TARGET:
			amp_target_free (g_node);
			break;
		case ANJUTA_PROJECT_SOURCE:
			//g_hash_table_remove (project->sources, AMP_NODE (g_node)->id);
			amp_source_free (g_node);
			break;
		case ANJUTA_PROJECT_MODULE:
			amp_module_free (g_node);
			break;
		case ANJUTA_PROJECT_PACKAGE:
			amp_package_free (g_node);
			break;
		case ANJUTA_PROJECT_ROOT:
			amp_root_clear (g_node);
			break;
		default:
			g_assert_not_reached ();
			break;
	}
}

static gboolean
project_node_destroy (AmpProject *project, AnjutaProjectNode *g_node)
{
	g_return_val_if_fail (project != NULL, FALSE);
	g_return_val_if_fail (AMP_IS_PROJECT (project), FALSE);
	
	if (g_node) {
		/* free each node's data first */
		foreach_node_destroy (g_node, project);
		//anjuta_project_node_foreach (g_node, G_POST_ORDER, 
		//		 foreach_node_destroy, project);
		
		/* now destroy the tree itself */
		//g_node_destroy (g_node);
	}

	return TRUE;
}

static AnjutaProjectNode *
project_node_new (AmpProject *project, AnjutaProjectNodeType type, GFile *file, const gchar *name, GError **error)
{
	AnjutaProjectNode *node = NULL;
	
	switch (type & ANJUTA_PROJECT_TYPE_MASK) {
		case ANJUTA_PROJECT_GROUP:
			node = amp_group_new (file, FALSE, error);
			break;
		case ANJUTA_PROJECT_TARGET:
			node = amp_target_new (name, 0, NULL, 0, error);
			break;
		case ANJUTA_PROJECT_SOURCE:
			if (file == NULL)
			{
				file = g_file_new_for_commandline_arg (name);
				node = amp_source_new (file, error);
				g_object_unref (file);
			}
			else
			{
				node = amp_source_new (file, error);
			}
			break;
		case ANJUTA_PROJECT_MODULE:
			node = amp_module_new (NULL, error);
			break;
		case ANJUTA_PROJECT_PACKAGE:
			node = amp_package_new (name, error);
			break;
		case ANJUTA_PROJECT_ROOT:
			node = amp_root_new (file, error);
			break;
		default:
			g_assert_not_reached ();
			break;
	}
	if (node != NULL) AMP_NODE_DATA (node)->type = type;

	return node;
}

/* Save a node. If the node doesn't correspond to a file, get its parent until
 * a file node is found and save it. The node returned correspond to the
 * file node */
static AnjutaProjectNode *
project_node_save (AmpProject *project, AnjutaProjectNode *g_node, GError **error)
{
	GHashTableIter iter;
	GHashTable *files;
	AnjutaProjectNode *parent;
	gpointer key;
	gpointer value;

	g_return_val_if_fail (project != NULL, FALSE);

	/* Create a hash table because some node can be split in several files and to
	 * avoid duplicate, not sure if it is really necessary */
	files = 	g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
	
	switch (AMP_NODE_DATA (g_node)->type & ANJUTA_PROJECT_TYPE_MASK) {
		case ANJUTA_PROJECT_GROUP:
			g_hash_table_insert (files, AMP_GROUP_DATA (g_node)->tfile, NULL);
			g_hash_table_insert (files, AMP_ROOT_DATA (project->root)->configure_file, NULL);
			break;
		case ANJUTA_PROJECT_TARGET:
		case ANJUTA_PROJECT_SOURCE:
			for (parent = g_node; anjuta_project_node_get_node_type (parent) != ANJUTA_PROJECT_GROUP; parent = anjuta_project_node_parent (parent));
			g_hash_table_insert (files, AMP_GROUP_DATA (parent)->tfile, NULL);
			break;
		case ANJUTA_PROJECT_MODULE:
		case ANJUTA_PROJECT_PACKAGE:
			/* Save only configure file */
			g_hash_table_insert (files, AMP_ROOT_DATA (project->root)->configure_file, NULL);
			break;
		case ANJUTA_PROJECT_ROOT:
			/* Get all project files */
			g_hash_table_iter_init (&iter, project->files);
			while (g_hash_table_iter_next (&iter, &key, &value))
			{
				AnjutaTokenFile *tfile = (AnjutaTokenFile *)value;
				g_hash_table_insert (files, tfile, NULL);
			}
			break;
		default:
			g_assert_not_reached ();
			break;
	}

	/* Save all files */
	g_hash_table_iter_init (&iter, files);
	while (g_hash_table_iter_next (&iter, &key, &value))
	{
		GError *error = NULL;
		AnjutaTokenFile *tfile = (AnjutaTokenFile *)key;

		anjuta_token_file_save (tfile, &error);
	}
}

void
amp_project_load_properties (AmpProject *project, AnjutaToken *macro, AnjutaToken *args)
{
	GList *item;
	
	//fprintf (stderr, "property list:\n");
	//anjuta_token_dump (args);

	project->ac_init = macro;
	project->args = args;

	for (item = anjuta_project_node_get_native_properties (project->root); item != NULL; item = g_list_next (item))
	{
		AmpProperty *prop = (AmpProperty *)item->data;

		if (prop->position >= 0)
		{
			AnjutaProjectProperty *new_prop;
			AnjutaToken *arg;

			new_prop = anjuta_project_node_remove_property (project->root, prop);
			if (new_prop != NULL)
			{
				amp_property_free (new_prop);
			}
			new_prop = amp_property_new (NULL, prop->token_type, prop->position, NULL, macro);
			anjuta_project_node_insert_property (project->root, prop, new_prop);

			arg = anjuta_token_nth_word (args, prop->position);
			if ((new_prop->value != NULL) && (new_prop->value != prop->base.value)) g_free (new_prop->value);
			new_prop->value = anjuta_token_evaluate (arg);
		}
	}
	//g_message ("prop list %p get prop %p", *list, anjuta_project_node_get_property (project->root);
}

void
amp_project_load_module (AmpProject *project, AnjutaToken *module_token)
{
	AmpAcScanner *scanner = NULL;

	if (module_token != NULL)
	{
		AnjutaToken *arg;
		AnjutaToken *list;
		AnjutaToken *item;
		gchar *value;
		AnjutaAmModuleNode *module;
		AnjutaAmPackageNode *package;
		gchar *compare;


		/* Module name */
		arg = anjuta_token_first_item (module_token);
		value = anjuta_token_evaluate (arg);
		module = amp_module_new (arg, NULL);
		anjuta_project_node_append (project->root, module);
		if (value != NULL) g_hash_table_insert (project->modules, value, module);

		/* Package list */
		arg = anjuta_token_next_word (arg);
		scanner = amp_ac_scanner_new (project);
		list = amp_ac_scanner_parse_token (scanner, arg, AC_SPACE_LIST_STATE, NULL);
		anjuta_token_free_children (arg);
		list = anjuta_token_delete_parent (list);
		anjuta_token_prepend_items (arg, list);
		amp_ac_scanner_free (scanner);
		
		package = NULL;
		compare = NULL;
		for (item = anjuta_token_first_word (arg); item != NULL; item = anjuta_token_next_word (item))
		{
			value = anjuta_token_evaluate (item);
			if (value == NULL) continue;		/* Empty value, a comment of a quote by example */
			if (*value == '\0')
			{
				g_free (value);
				continue;
			}
			
			if ((package != NULL) && (compare != NULL))
			{
				amp_package_set_version (package, compare, value);
				g_free (value);
				g_free (compare);
				package = NULL;
				compare = NULL;
			}
			else if ((package != NULL) && (anjuta_token_get_type (item) == ANJUTA_TOKEN_OPERATOR))
			{
				compare = value;
			}
			else
			{
				package = amp_package_new (value, NULL);
				anjuta_project_node_append (module, package);
				anjuta_project_node_set_state ((AnjutaProjectNode *)package, ANJUTA_PROJECT_INCOMPLETE);
				g_free (value);
				compare = NULL;
			}
		}
	}
}

void
amp_project_load_config (AmpProject *project, AnjutaToken *arg_list)
{
	AmpAcScanner *scanner = NULL;

	if (arg_list != NULL)
	{
		AnjutaToken *arg;
		AnjutaToken *list;
		AnjutaToken *item;

		/* File list */
		scanner = amp_ac_scanner_new (project);
		fprintf (stderr, "\nParse list\n");
		
		arg = anjuta_token_first_item (arg_list);
		list = amp_ac_scanner_parse_token (scanner, arg, AC_SPACE_LIST_STATE, NULL);
		anjuta_token_free_children (arg);
		list = anjuta_token_delete_parent (list);
		anjuta_token_prepend_items (arg, list);
		amp_ac_scanner_free (scanner);
		
		for (item = anjuta_token_first_word (arg); item != NULL; item = anjuta_token_next_word (item))
		{
			gchar *value;
			AmpConfigFile *cfg;

			value = anjuta_token_evaluate (item);
			if (value == NULL) continue;
		
			cfg = amp_config_file_new (value, anjuta_project_node_get_file (project->root), item);
			g_hash_table_insert (project->configs, cfg->file, cfg);
			g_free (value);
		}
	}
}

static AnjutaToken*
project_load_target (AmpProject *project, AnjutaToken *name, AnjutaTokenType token_type, AnjutaToken *list, AnjutaProjectNode *parent, GHashTable *orphan_properties)
{
	AnjutaToken *arg;
	gchar *install;
	gchar *value;
	gint flags;
	AmpNodeInfo *info = AmpNodeInformations; 

	while (info->base.type != 0)
	{
		if (token_type == info->token)
		{
			break;
		}
		info++;
	}

	value = anjuta_token_evaluate (name);
	split_automake_variable (value, &flags, &install, NULL);

	amp_group_add_token (parent, name, AM_GROUP_TARGET);

	for (arg = anjuta_token_first_word (list); arg != NULL; arg = anjuta_token_next_word (arg))
	{
		gchar *value;
		gchar *canon_id;
		AnjutaAmTargetNode *target;
		AmpTargetPropertyBuffer *buffer;
		gchar *orig_key;
		gpointer find;

		value = anjuta_token_evaluate (arg);
		
		/* This happens for variable token which are considered as value */
		if (value == NULL) continue;
		canon_id = canonicalize_automake_variable (value);
		
		/* Check if target already exists */
		find = value;
		anjuta_project_node_children_traverse (parent, find_target, &find);
		if ((gchar *)find != value)
		{
			/* Find target */
			g_free (canon_id);
			g_free (value);
			continue;
		}

		/* Create target */
		target = amp_target_new (value, info->base.type, install, flags, NULL);
		amp_target_add_token (target, arg);
		anjuta_project_node_append (parent, target);
		DEBUG_PRINT ("create target %p name %s", target, value);

		/* Check if there are sources or properties availables */
		if (g_hash_table_lookup_extended (orphan_properties, canon_id, (gpointer *)&orig_key, (gpointer *)&buffer))
		{
			GList *sources;
			GList *src;

			g_hash_table_steal (orphan_properties, canon_id);
			sources = amp_target_property_buffer_steal_sources (buffer);
			for (src = sources; src != NULL; src = g_list_next (src))
			{
				AnjutaAmSourceNode *source = src->data;

				anjuta_project_node_prepend (target, source);
			}
			g_free (orig_key);
			g_list_free (sources);

			amp_target_property_buffer_free (buffer);
		}

		/* Set target properties */
		if (flags & AM_TARGET_NOBASE) 
			amp_node_property_load (target, AM_TOKEN__PROGRAMS, 0, "1", arg);
		if (flags & AM_TARGET_NOTRANS) 
			amp_node_property_load (target, AM_TOKEN__PROGRAMS, 1, "1", arg);
		if (flags & AM_TARGET_DIST) 
			amp_node_property_load (target, AM_TOKEN__PROGRAMS, 2, "1", arg);
		if (flags & AM_TARGET_NODIST) 
			amp_node_property_load (target, AM_TOKEN__PROGRAMS, 2, "0", arg);
		if (flags & AM_TARGET_NOINST) 
		{
			amp_node_property_load (target, AM_TOKEN__PROGRAMS, 3, "1", arg);
		}
		else
		{
			gchar *instdir = g_strconcat ("$(", install, "dir)", NULL);
			amp_node_property_load (target, AM_TOKEN__PROGRAMS, 6, instdir, arg);
			g_free (instdir);
		}
		
		if (flags & AM_TARGET_CHECK) 
			amp_node_property_load (target, AM_TOKEN__PROGRAMS, 4, "1", arg);
		if (flags & AM_TARGET_MAN)
		{
			gchar section[] = "0";

			section[0] += (flags >> 7) & 0x1F;
			amp_node_property_load (target, AM_TOKEN__PROGRAMS, 4, section, arg);
		}
		
		g_free (canon_id);
		g_free (value);
	}
	g_free (value);

	return NULL;
}

static AnjutaToken*
project_load_sources (AmpProject *project, AnjutaToken *name, AnjutaToken *list, AnjutaProjectNode *parent, GHashTable *orphan_properties)
{
	AnjutaToken *arg;
	GFile *parent_file = g_object_ref (anjuta_project_node_get_file (ANJUTA_PROJECT_NODE (parent)));
	gchar *target_id = NULL;
	AmpTargetPropertyBuffer *orphan = NULL;

	target_id = anjuta_token_evaluate (name);
	if (target_id)
	{
		gchar *end = strrchr (target_id, '_');
		if (end)
		{
			*end = '\0';
		}
	}

	if (target_id)
	{
		gpointer find;
		
		find = target_id;
		DEBUG_PRINT ("search for canonical %s", target_id);
		anjuta_project_node_children_traverse (parent, find_canonical_target, &find);
		parent = (gchar *)find != target_id ? (AnjutaProjectNode *)find : NULL;

		/* Get orphan buffer if there is no target */
		if (parent == NULL)
		{
			gchar *orig_key;
			if (g_hash_table_lookup_extended (orphan_properties, target_id, (gpointer *)&orig_key, (gpointer *)&orphan))
			{
				g_hash_table_steal (orphan_properties, target_id);
				g_free (orig_key);
			}
			else
			{
				orphan = amp_target_property_buffer_new ();
			}
		}

		for (arg = anjuta_token_first_word (list); arg != NULL; arg = anjuta_token_next_word (arg))
		{
			gchar *value;
			AnjutaAmSourceNode *source;
			GFile *src_file;
		
			value = anjuta_token_evaluate (arg);

			/* Create source */
			src_file = g_file_get_child (parent_file, value);
			source = project_node_new (project, ANJUTA_PROJECT_SOURCE | ANJUTA_PROJECT_PROJECT, src_file, NULL, NULL);
			g_object_unref (src_file);
			AMP_SOURCE_DATA(source)->token = arg;

			if (orphan != NULL)
			{
				amp_target_property_buffer_add_source (orphan, source);
			}
			else
			{
				DEBUG_PRINT ("add target child %p", parent);
				/* Add as target child */
				anjuta_project_node_append (parent, source);
			}

			g_free (value);
		}

		if (orphan != NULL)
		{
			g_hash_table_insert (orphan_properties, target_id, orphan);
		}
		else
		{
			g_free (target_id);
		}
	}

	g_object_unref (parent_file);

	return NULL;
}

static AnjutaToken*
project_load_data (AmpProject *project, AnjutaToken *name, AnjutaToken *list, AnjutaProjectNode *parent, GHashTable *orphan_properties)
{
	gchar *install;
	AnjutaProjectNode *target;
	gchar *target_id;
	gpointer find;
	gint flags;
	AmpNodeInfo *info = AmpNodeInformations; 
	AnjutaToken *arg;

	while (info->base.name != NULL)
	{
		if (anjuta_token_get_type (name) == info->token)
		{
			break;
		}
		info++;
	}

	target_id = anjuta_token_evaluate (name);
	split_automake_variable (target_id, &flags, &install, NULL);
	/*if (target_id)
	{
		gchar *end = strrchr (target_id, '_');
		if (end)
		{
			*end = '\0';
		}
	}*/
	
	amp_group_add_token (parent, name, AM_GROUP_TARGET);

	/* Check if target already exists */
	find = target_id;
	anjuta_project_node_children_traverse (parent, find_target, &find);
	if ((gchar *)find == target_id)
	{
		/* Create target */
		target = amp_target_new (target_id, info->base.type, install, flags, NULL);
		amp_target_add_token (target, arg);
		anjuta_project_node_append (parent, target);
		DEBUG_PRINT ("create target %p name %s", target, target_id);
	}
	else
	{
		target = (AnjutaProjectNode *)find;
	}
	g_free (target_id);

	if (target)
	{
		GFile *parent_file = g_object_ref (AMP_GROUP_DATA (parent)->base.file);
		
		for (arg = anjuta_token_first_word (list); arg != NULL; arg = anjuta_token_next_word (arg))
		{
			gchar *value;
			AnjutaAmSourceNode *source;
			GFile *src_file;
		
			value = anjuta_token_evaluate (arg);

			/* Create source */
			src_file = g_file_get_child (parent_file, value);
			source = project_node_new (project, ANJUTA_PROJECT_SOURCE | ANJUTA_PROJECT_PROJECT, src_file, NULL, NULL);
			g_object_unref (src_file);
			AMP_SOURCE_DATA(source)->token = arg;

			/* Add as target child */
			DEBUG_PRINT ("add target child %p", target);
			anjuta_project_node_append (target, source);

			g_free (value);
		}
		g_object_unref (parent_file);
	}

	return NULL;
}

static AnjutaToken*
project_load_target_properties (AmpProject *project, AnjutaToken *name, AnjutaTokenType type, AnjutaToken *list, AnjutaProjectNode *parent, GHashTable *orphan_properties)
{
	gchar *target_id = NULL;
	
	target_id = anjuta_token_evaluate (name);
	if (target_id)
	{
		gchar *end = strrchr (target_id, '_');
		if (end)
		{
			*end = '\0';
		}
	}

	if (target_id)
	{
		gpointer find;
		gchar *value;
		AnjutaProjectProperty *prop;
		AmpTargetPropertyBuffer *orphan = NULL;
		
		find = target_id;
		DEBUG_PRINT ("search for canonical %s", target_id);
		anjuta_project_node_children_traverse (parent, find_canonical_target, &find);
		parent = (gchar *)find != target_id ? (AnjutaProjectNode *)find : NULL;

		/* Create property */
		value = anjuta_token_evaluate (list);
		prop = amp_property_new (NULL, type, 0, value, list);

		if (parent == NULL)
		{
			/* Add property to non existing target, create a dummy target (AmpTargetPropertyBuffer) */
			gchar *orig_key;
			AmpTargetPropertyBuffer *orphan = NULL;
			
			if (g_hash_table_lookup_extended (orphan_properties, target_id, (gpointer *)&orig_key, (gpointer *)&orphan))
			{
				/* Dummy target already created */
				g_hash_table_steal (orphan_properties, target_id);
				g_free (orig_key);
			}
			else
			{
				/* Create dummy target */
				orphan = amp_target_property_buffer_new ();
			}
			amp_target_property_buffer_add_property (orphan, prop);
			g_hash_table_insert (orphan_properties, target_id, orphan);
		}
		else
		{
			/* Add property to existing target */
			amp_node_property_add (parent, prop);
			g_free (target_id);
		}
		g_free (value);
	}

	return NULL;
}

static AnjutaToken*
project_load_group_properties (AmpProject *project, AnjutaToken *token, AnjutaTokenType type, AnjutaToken *list, AnjutaProjectNode *parent)
{
	gchar *value;
	gchar *name;
	AnjutaProjectProperty *prop;
		
	/* Create property */
	name = anjuta_token_evaluate (token);
	value = anjuta_token_evaluate (list);

	prop = amp_property_new (name, type, 0, value, list);

	amp_node_property_add (parent, prop);
	g_free (value);
	g_free (name);

	return NULL;
}

static AnjutaAmGroupNode* project_load_makefile (AmpProject *project, AnjutaAmGroupNode *group);

static gboolean
find_group (AnjutaProjectNode *node, gpointer data)
{
	if ((AMP_NODE_DATA (node)->type  & ANJUTA_PROJECT_TYPE_MASK) == ANJUTA_PROJECT_GROUP)
	{
		if (g_file_equal (anjuta_project_node_get_file (node), (GFile *)data))
		{
			/* Find group, return node value in pointer */
			return TRUE;
		}
	}

	return FALSE;
}

static void
project_load_subdirs (AmpProject *project, AnjutaToken *list, AnjutaAmGroupNode *parent, gboolean dist_only)
{
	AnjutaToken *arg;

	for (arg = anjuta_token_first_word (list); arg != NULL; arg = anjuta_token_next_word (arg))
	{
		gchar *value;
		
		value = anjuta_token_evaluate (arg);
		
		/* Skip ., it is a special case, used to defined build order */
		if (strcmp (value, ".") != 0)
		{
			GFile *subdir;
			gchar *group_id;
			AnjutaAmGroupNode *group;

			subdir = g_file_resolve_relative_path (AMP_GROUP_DATA (parent)->base.file, value);
			
			/* Look for already existing group */
			group =anjuta_project_node_children_traverse (parent, find_group, subdir);

			if (group != NULL)
			{
				/* Already existing group, mark for built if needed */
				if (!dist_only) amp_group_set_dist_only (group, FALSE);
			}
			else
			{
				/* Create new group */
				group = amp_group_new (subdir, dist_only, NULL);
				g_hash_table_insert (project->groups, g_file_get_uri (subdir), group);
				anjuta_project_node_append (parent, group);
				group = project_load_makefile (project, group);
			}
			amp_group_add_token (group, arg, dist_only ? AM_GROUP_TOKEN_DIST_SUBDIRS : AM_GROUP_TOKEN_SUBDIRS);
			g_object_unref (subdir);
		}
		g_free (value);
	}
}

static AnjutaAmGroupNode*
project_load_makefile (AmpProject *project, AnjutaAmGroupNode *group)
{
	const gchar **filename;
	AnjutaTokenFile *tfile;
	GFile *makefile = NULL;
	GFile *file = anjuta_project_node_get_file ((AnjutaProjectNode *)group);

	/* Find makefile name
	 * It has to be in the config_files list with .am extension */
	for (filename = valid_am_makefiles; *filename != NULL; filename++)
	{
		makefile = g_file_get_child (file, *filename);
		if (file_type (file, *filename) == G_FILE_TYPE_REGULAR)
		{
			gchar *final_filename = g_strdup (*filename);
			gchar *ptr;
			GFile *final_file;
			AmpConfigFile *config;

			ptr = g_strrstr (final_filename,".am");
			if (ptr != NULL) *ptr = '\0';
			final_file = g_file_get_child (file, final_filename);
			g_free (final_filename);
			
			config = g_hash_table_lookup (project->configs, final_file);
			g_object_unref (final_file);
			if (config != NULL)
			{
				//g_message ("add group =%s= token %p group %p", *filename, config->token, anjuta_token_list (config->token));
				amp_group_add_token (group, config->token, AM_GROUP_TOKEN_CONFIGURE);
				break;
			}
		}
		g_object_unref (makefile);
	}

	if (*filename == NULL)
	{
		/* Unable to find automake file */
		return group;
	}
	
	/* Parse makefile.am */
	DEBUG_PRINT ("Parse: %s", g_file_get_uri (makefile));
	tfile = amp_group_set_makefile (group, makefile, project);
	g_hash_table_insert (project->files, makefile, tfile);
	g_object_add_toggle_ref (G_OBJECT (tfile), remove_config_file, project);
	
	return group;
}

void
amp_project_set_am_variable (AmpProject* project, AnjutaAmGroupNode* group, AnjutaTokenType variable, AnjutaToken *name, AnjutaToken *list, GHashTable *orphan_properties)
{
	switch (variable)
	{
	case AM_TOKEN_SUBDIRS:
		project_load_subdirs (project, list, group, FALSE);
		break;
	case AM_TOKEN_DIST_SUBDIRS:
		project_load_subdirs (project, list, group, TRUE);
		break;
	case AM_TOKEN__DATA:
		project_load_data (project, name, list, group, orphan_properties);
		break;
	case AM_TOKEN__HEADERS:
	case AM_TOKEN__LIBRARIES:
	case AM_TOKEN__LISP:
	case AM_TOKEN__LTLIBRARIES:
	case AM_TOKEN__MANS:
	case AM_TOKEN__PROGRAMS:
	case AM_TOKEN__PYTHON:
	case AM_TOKEN__JAVA:
	case AM_TOKEN__SCRIPTS:
	case AM_TOKEN__TEXINFOS:
		project_load_target (project, name, variable, list, group, orphan_properties);
		break;
	case AM_TOKEN__SOURCES:
		project_load_sources (project, name, list, group, orphan_properties);
		break;
	case AM_TOKEN_DIR:
	case AM_TOKEN__LDFLAGS:
	case AM_TOKEN__CPPFLAGS:
	case AM_TOKEN__CFLAGS:
	case AM_TOKEN__CXXFLAGS:
	case AM_TOKEN__JAVACFLAGS:
	case AM_TOKEN__FCFLAGS:
	case AM_TOKEN__OBJCFLAGS:
	case AM_TOKEN__LFLAGS:
	case AM_TOKEN__YFLAGS:
		project_load_group_properties (project, name, variable, list, group);
		break;
	case AM_TOKEN_TARGET_LDFLAGS:
	case AM_TOKEN_TARGET_CPPFLAGS:
	case AM_TOKEN_TARGET_CFLAGS:
	case AM_TOKEN_TARGET_CXXFLAGS:
	case AM_TOKEN_TARGET_JAVACFLAGS:
	case AM_TOKEN_TARGET_FCFLAGS:
	case AM_TOKEN_TARGET_OBJCFLAGS:
	case AM_TOKEN_TARGET_LFLAGS:
	case AM_TOKEN_TARGET_YFLAGS:
	case AM_TOKEN_TARGET_DEPENDENCIES:
		project_load_target_properties (project, name, variable, list, group, orphan_properties);
		break;
	default:
		break;
	}
}


/* Public functions
 *---------------------------------------------------------------------------*/

AnjutaProjectNodeInfo *
amp_project_get_type_info (AmpProject *project, AnjutaProjectNodeType type)
{
	AmpNodeInfo *info;
		
	for (info = AmpNodeInformations; info->base.type != type; info++)
	{
		if ((info->base.type == type) || (info->base.type == 0)) break;
	}

	return (AnjutaProjectNodeInfo *)info;
}

static AnjutaProjectNode *
amp_project_load_root (AmpProject *project, GError **error) 
{
	AmpAcScanner *scanner;
	AnjutaToken *arg;
	AnjutaAmGroupNode *group;
	GFile *root_file;
	GFile *configure_file;
	gboolean ok = TRUE;
	GError *err = NULL;

	/* Unload current project */
	amp_project_unload (project);
	DEBUG_PRINT ("reload project %p root file %p", project, root_file);

	root_file = anjuta_project_node_get_file (project->root);

	/* shortcut hash tables */
	project->groups = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	project->files = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);
	project->configs = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, NULL, (GDestroyNotify)amp_config_file_free);
	amp_project_new_module_hash (project);

	/* Initialize list styles */
	project->ac_space_list = anjuta_token_style_new (NULL, " ", "\n", NULL, 0);
	project->am_space_list = anjuta_token_style_new (NULL, " ", " \\\n", NULL, 0);
	project->arg_list = anjuta_token_style_new (NULL, ", ", ", ", ")", 0);
	
	/* Find configure file */
	if (file_type (root_file, "configure.ac") == G_FILE_TYPE_REGULAR)
	{
		configure_file = g_file_get_child (root_file, "configure.ac");
	}
	else if (file_type (root_file, "configure.in") == G_FILE_TYPE_REGULAR)
	{
		configure_file = g_file_get_child (root_file, "configure.in");
	}
	else
	{
		g_set_error (error, IANJUTA_PROJECT_ERROR, 
					IANJUTA_PROJECT_ERROR_DOESNT_EXIST,
			_("Project doesn't exist or invalid path"));

		return NULL;
	}

	/* Parse configure */
	project->configure_file = amp_root_set_configure (project->root, configure_file);
	g_hash_table_insert (project->files, configure_file, project->configure_file);
	g_object_add_toggle_ref (G_OBJECT (project->configure_file), remove_config_file, project);
	arg = anjuta_token_file_load (project->configure_file, NULL);
	//fprintf (stderr, "AC file before parsing\n");
	//anjuta_token_dump (arg);
	//fprintf (stderr, "\n");
	scanner = amp_ac_scanner_new (project);
	project->configure_token = amp_ac_scanner_parse_token (scanner, arg, 0, &err);
	//fprintf (stderr, "AC file after parsing\n");
	//anjuta_token_check (arg);
	//anjuta_token_dump (project->configure_token);
	//fprintf (stderr, "\n");
	amp_ac_scanner_free (scanner);
	if (project->configure_token == NULL)
	{
		g_set_error (error, IANJUTA_PROJECT_ERROR, 
						IANJUTA_PROJECT_ERROR_PROJECT_MALFORMED,
						err == NULL ? _("Unable to parse project file") : err->message);
		if (err != NULL) g_error_free (err);
			return NULL;
	}

	/* Load all makefiles recursively */
	group = amp_group_new (root_file, FALSE, NULL);
	g_hash_table_insert (project->groups, g_file_get_uri (root_file), group);
	anjuta_project_node_append (project->root, group);
	
	if (project_load_makefile (project, group) == NULL)
	{
		g_set_error (error, IANJUTA_PROJECT_ERROR, 
					IANJUTA_PROJECT_ERROR_DOESNT_EXIST,
			_("Project doesn't exist or invalid path"));

		return NULL;
	}

	return project->root;
}

static void
list_all_children (GList **children, GFile *dir)
{
	GFileEnumerator *list;
					
	list = g_file_enumerate_children (dir,
	    G_FILE_ATTRIBUTE_STANDARD_NAME,
	    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	    NULL,
	    NULL);

	if (list != NULL)
	{
		GFileInfo *info;
		
		while ((info = g_file_enumerator_next_file (list, NULL, NULL)) != NULL)
		{
			const gchar *name;
			GFile *file;

			name = g_file_info_get_name (info);
			file = g_file_get_child (dir, name);
			g_object_unref (info);

			if (g_file_query_file_type (file, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY)
			{
				list_all_children (children, file);
				g_object_unref (file);
			}
			else
			{
				*children = g_list_prepend (*children, file);
			}
		}
		g_file_enumerator_close (list, NULL, NULL);
		g_object_unref (list);
	}
}

static AnjutaProjectNode *
amp_project_load_package (AmpProject *project, AnjutaProjectNode *node, GError **error)
{
	gchar *cmd;
	gchar *err;
	gchar *out;
	gint status;

	cmd = g_strdup_printf ("pkg-config --cflags %s", anjuta_project_node_get_name (node));

	if (g_spawn_command_line_sync (cmd, &out, &err, &status, error))
	{
		gchar **flags;

		flags = g_strsplit (out, " ", -1);

		if (flags != NULL)
		{
			gchar **flag;
			
			for (flag = flags; *flag != NULL; flag++)
			{
				if (g_regex_match_simple ("\\.*/include/\\w+", *flag, 0, 0) == TRUE)
                {
                    /* FIXME the +2. It's to skip the -I */
					GFile *dir;
					GList *children = NULL;
					GList *file;
					
					dir = g_file_new_for_path (*flag + 2);
					list_all_children (&children, dir);
					for (file = g_list_first (children); file != NULL; file = g_list_next (file))
					{
						/* Create a source for files */
						AnjutaAmSourceNode *source;

						source = project_node_new (project, ANJUTA_PROJECT_SOURCE, (GFile *)file->data, NULL, NULL);
						anjuta_project_node_append (node, source);
						g_object_unref ((GObject *)file->data);
					}
					g_list_free (children);
					g_object_unref (dir);
				}
			}
		}
	}
	else
	{
		node = NULL;
	}
	g_free (cmd);

	return node;
}

static AnjutaProjectNode *
amp_project_load_group (AmpProject *project, AnjutaProjectNode *node, GError **error)
{
	if (project_load_makefile (project, node) == NULL)
	{
		g_set_error (error, IANJUTA_PROJECT_ERROR, 
					IANJUTA_PROJECT_ERROR_DOESNT_EXIST,
			_("Project doesn't exist or invalid path"));

		return NULL;
	}

	return node;
}

AnjutaProjectNode *
amp_project_load_node (AmpProject *project, AnjutaProjectNode *node, GError **error) 
{
	switch (anjuta_project_node_get_node_type (node))
	{
	case ANJUTA_PROJECT_ROOT:
		return amp_project_load_root (project, error);
	case ANJUTA_PROJECT_PACKAGE:
		return amp_project_load_package (project, node, error);
	case ANJUTA_PROJECT_GROUP:
		return amp_project_load_group (project, node, error);
	default:
		return NULL;
	}
}

void
amp_project_unload (AmpProject *project)
{
	/* project data */
	if (project->root) project_node_destroy (project, project->root);

	if (project->configure_file)	g_object_unref (G_OBJECT (project->configure_file));
	project->configure_file = NULL;
	if (project->configure_token) anjuta_token_free (project->configure_token);

	g_list_foreach (project->properties, (GFunc)amp_property_free, NULL);
	project->properties = amp_get_project_property_list ();
	
	/* shortcut hash tables */
	if (project->groups) g_hash_table_destroy (project->groups);
	if (project->files) g_hash_table_destroy (project->files);
	if (project->configs) g_hash_table_destroy (project->configs);
	project->groups = NULL;
	project->files = NULL;
	project->configs = NULL;

	/* List styles */
	if (project->am_space_list) anjuta_token_style_free (project->am_space_list);
	if (project->ac_space_list) anjuta_token_style_free (project->ac_space_list);
	if (project->arg_list) anjuta_token_style_free (project->arg_list);
	
	amp_project_free_module_hash (project);
}

gint
amp_project_probe (GFile *file,
	    GError     **error)
{
	gint probe;
	gboolean dir;

	dir = (file_type (file, NULL) == G_FILE_TYPE_DIRECTORY);
	if (!dir)
	{
		g_set_error (error, IANJUTA_PROJECT_ERROR, 
		             IANJUTA_PROJECT_ERROR_DOESNT_EXIST,
			   _("Project doesn't exist or invalid path"));
	}
	
	probe =  dir; 
	if (probe)
	{
		const gchar **makefile;

		/* Look for makefiles */
		probe = FALSE;
		for (makefile = valid_am_makefiles; *makefile != NULL; makefile++)
		{
			if (file_type (file, *makefile) == G_FILE_TYPE_REGULAR)
			{
				probe = TRUE;
				break;
			}
		}

		if (probe)
		{
			probe = ((file_type (file, "configure.ac") == G_FILE_TYPE_REGULAR) ||
			 				(file_type (file, "configure.in") == G_FILE_TYPE_REGULAR));
		}
	}

	return probe ? IANJUTA_PROJECT_PROBE_PROJECT_FILES : 0;
}

gboolean
amp_project_get_token_location (AmpProject *project, AnjutaTokenFileLocation *location, AnjutaToken *token)
{
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_hash_table_iter_init (&iter, project->files);
	while (g_hash_table_iter_next (&iter, &key, &value))
	{
		if (anjuta_token_file_get_token_location ((AnjutaTokenFile *)value, location, token))
		{
			return TRUE;
		}
	}

	return FALSE;
}

void 
amp_project_remove_group (AmpProject  *project,
		   AnjutaAmGroupNode *group,
		   GError     **error)
{
	GList *token_list;

	if (anjuta_project_node_get_node_type (group) != ANJUTA_PROJECT_GROUP) return;
	
	for (token_list = amp_group_get_token (group, AM_GROUP_TOKEN_CONFIGURE); token_list != NULL; token_list = g_list_next (token_list))
	{
		anjuta_token_remove_word ((AnjutaToken *)token_list->data);
	}
	for (token_list = amp_group_get_token (group, AM_GROUP_TOKEN_SUBDIRS); token_list != NULL; token_list = g_list_next (token_list))
	{
		anjuta_token_remove_word ((AnjutaToken *)token_list->data);
	}
	for (token_list = amp_group_get_token (group, AM_GROUP_TOKEN_DIST_SUBDIRS); token_list != NULL; token_list = g_list_next (token_list))
	{
		anjuta_token_remove_word ((AnjutaToken *)token_list->data);
	}

	amp_group_free (group);
}

void 
amp_project_remove_target (AmpProject  *project,
		    AnjutaAmTargetNode *target,
		    GError     **error)
{
	GList *token_list;

	if (anjuta_project_node_get_node_type (target) != ANJUTA_PROJECT_TARGET) return;
	
	for (token_list = amp_target_get_token (target); token_list != NULL; token_list = g_list_next (token_list))
	{
		anjuta_token_remove_word ((AnjutaToken *)token_list->data);
	}

	amp_target_free (target);
}

void 
amp_project_remove_source (AmpProject  *project,
		    AnjutaAmSourceNode *source,
		    GError     **error)
{
	amp_dump_node (source);
	if (anjuta_project_node_get_node_type (source) != ANJUTA_PROJECT_SOURCE) return;
	
	anjuta_token_remove_word (AMP_SOURCE_DATA (source)->token);

	amp_source_free (source);
}

GList *
amp_project_get_node_info (AmpProject *project, GError **error)
{
	static GList *info_list = NULL;

	if (info_list == NULL)
	{
		AmpNodeInfo *node;
		
		for (node = AmpNodeInformations; node->base.type != 0; node++)
		{
			info_list = g_list_prepend (info_list, node);
		}

		info_list = g_list_reverse (info_list);
	}
	
	return info_list;
}

/* Public functions
 *---------------------------------------------------------------------------*/

gboolean
amp_project_save (AmpProject *project, GError **error)
{
	gpointer key;
	gpointer value;
	GHashTableIter iter;

	g_return_val_if_fail (project != NULL, FALSE);

	g_hash_table_iter_init (&iter, project->files);
	while (g_hash_table_iter_next (&iter, &key, &value))
	{
		GError *error = NULL;
		AnjutaTokenFile *tfile = (AnjutaTokenFile *)value;
	
		anjuta_token_file_save (tfile, &error);
	}

	return TRUE;
}

typedef struct _AmpMovePacket {
	AmpProject *project;
	GFile *old_root_file;
} AmpMovePacket;

static void
foreach_node_move (AnjutaProjectNode *g_node, gpointer data)
{
	AmpProject *project = ((AmpMovePacket *)data)->project;
	const gchar *old_root_file = ((AmpMovePacket *)data)->old_root_file;
	GFile *root_file = anjuta_project_node_get_file (project->root);
	GFile *relative;
	GFile *new_file;
	
	switch (anjuta_project_node_get_node_type (g_node))
	{
	case ANJUTA_PROJECT_GROUP:
		relative = get_relative_path (old_root_file, AMP_GROUP_DATA (g_node)->base.file);
		new_file = g_file_resolve_relative_path (root_file, relative);
		g_free (relative);
		g_object_unref (AMP_GROUP_DATA (g_node)->base.file);
		AMP_GROUP_DATA (g_node)->base.file = new_file;

		g_hash_table_insert (project->groups, g_file_get_uri (new_file), g_node);
		break;
	case ANJUTA_PROJECT_SOURCE:
		relative = get_relative_path (old_root_file, AMP_SOURCE_DATA (g_node)->base.file);
		new_file = g_file_resolve_relative_path (root_file, relative);
		g_free (relative);
		g_object_unref (AMP_SOURCE_DATA (g_node)->base.file);
		AMP_SOURCE_DATA (g_node)->base.file = new_file;
		break;
	case ANJUTA_PROJECT_ROOT:
		relative = get_relative_path (old_root_file, AMP_GROUP_DATA (g_node)->base.file);
		new_file = g_file_resolve_relative_path (root_file, relative);
		g_free (relative);
		g_object_unref (AMP_ROOT_DATA (g_node)->base.file);
		AMP_ROOT_DATA (g_node)->base.file = new_file;
		break;
	default:
		break;
	}
}

gboolean
amp_project_move (AmpProject *project, const gchar *path)
{
	GFile *new_file;
	GFile *root_file;
	gchar *relative;
	GHashTableIter iter;
	gpointer key;
	AnjutaTokenFile *tfile;
	AmpConfigFile *cfg;
	GHashTable* old_hash;
	AmpMovePacket packet= {project, NULL};

	/* Change project root directory */
	packet.old_root_file = g_object_ref (anjuta_project_node_get_file (project->root));
	root_file = g_file_new_for_path (path);
	g_object_unref (AMP_ROOT_DATA (project->root)->base.file);
	AMP_ROOT_DATA (project->root)->base.file = g_object_ref (root_file);

	/* Change project root directory in groups */
	old_hash = project->groups;
	project->groups = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	anjuta_project_node_foreach (project->root, G_POST_ORDER, foreach_node_move, &packet);
	g_hash_table_destroy (old_hash);

	/* Change all files */
	old_hash = project->files;
	project->files = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);
	g_hash_table_iter_init (&iter, old_hash);
	while (g_hash_table_iter_next (&iter, &key, (gpointer *)&tfile))
	{
		relative = get_relative_path (packet.old_root_file, anjuta_token_file_get_file (tfile));
		new_file = g_file_resolve_relative_path (root_file, relative);
		g_free (relative);
		anjuta_token_file_move (tfile, new_file);
		
		g_hash_table_insert (project->files, new_file, tfile);
		g_object_unref (key);
	}
	g_hash_table_steal_all (old_hash);
	g_hash_table_destroy (old_hash);

	/* Change all configs */
	old_hash = project->configs;
	project->configs = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, NULL, (GDestroyNotify)amp_config_file_free);
	g_hash_table_iter_init (&iter, old_hash);
	while (g_hash_table_iter_next (&iter, &key, (gpointer *)&cfg))
	{
		relative = get_relative_path (packet.old_root_file, cfg->file);
		new_file = g_file_resolve_relative_path (root_file, relative);
		g_free (relative);
		g_object_unref (cfg->file);
		cfg->file = new_file;
		
		g_hash_table_insert (project->configs, new_file, cfg);
	}
	g_hash_table_steal_all (old_hash);
	g_hash_table_destroy (old_hash);


	g_object_unref (root_file);
	g_object_unref (packet.old_root_file);

	return TRUE;
}

/* Dump file content of corresponding node */
gboolean
amp_project_dump (AmpProject *project, AnjutaProjectNode *node)
{
	gboolean ok = FALSE;
	
	switch (anjuta_project_node_get_node_type (node))
	{
	case ANJUTA_PROJECT_GROUP:
		anjuta_token_dump (AMP_GROUP_DATA (node)->make_token);
		break;
	default:
		break;
	}

	return ok;
}


AmpProject *
amp_project_new (GFile *file, GError **error)
{
	AmpProject *project;
	
	project = AMP_PROJECT (g_object_new (AMP_TYPE_PROJECT, NULL));
	project->root = amp_root_new (file, error);

	return project;
}

/* Project access functions
 *---------------------------------------------------------------------------*/

AnjutaAmRootNode *
amp_project_get_root (AmpProject *project)
{
	AnjutaAmRootNode *g_node = NULL;

	return ANJUTA_AM_ROOT_NODE (project->root);
}

AnjutaAmGroupNode *
amp_project_get_group (AmpProject *project, const gchar *id)
{
	return (AnjutaAmGroupNode *)g_hash_table_lookup (project->groups, id);
}

AnjutaAmTargetNode *
amp_project_get_target (AmpProject *project, const gchar *id)
{
	AnjutaAmTargetNode **buffer;
	AnjutaAmTargetNode *target;
	gsize dummy;

	buffer = (AnjutaAmTargetNode **)g_base64_decode (id, &dummy);
	target = *buffer;
	g_free (buffer);

	return target;
}

AnjutaAmSourceNode *
amp_project_get_source (AmpProject *project, const gchar *id)
{
	AnjutaAmSourceNode **buffer;
	AnjutaAmSourceNode *source;
	gsize dummy;

	buffer = (AnjutaAmSourceNode **)g_base64_decode (id, &dummy);
	source = *buffer;
	g_free (buffer);

	return source;
}

gchar *
amp_project_get_node_id (AmpProject *project, const gchar *path)
{
	AnjutaProjectNode *node = NULL;

	if (path != NULL)
	{
		for (; *path != '\0';)
		{
			gchar *end;
			guint child = g_ascii_strtoull (path, &end, 10);

			if (end == path)
			{
				/* error */
				return NULL;
			}

			if (node == NULL)
			{
				if (child == 0) node = project->root;
			}
			else
			{
				node = anjuta_project_node_nth_child (node, child);
			}
			if (node == NULL)
			{
				/* no node */
				return NULL;
			}

			if (*end == '\0') break;
			path = end + 1;
		}
	}

	switch (anjuta_project_node_get_node_type (node))
	{
		case ANJUTA_PROJECT_GROUP:
			return g_file_get_uri (AMP_GROUP_DATA (node)->base.file);
		case ANJUTA_PROJECT_TARGET:
		case ANJUTA_PROJECT_SOURCE:
			return g_base64_encode ((guchar *)&node, sizeof (node));
		default:
			return NULL;
	}
}

gchar *
amp_project_get_uri (AmpProject *project)
{
	g_return_val_if_fail (project != NULL, NULL);

	return project->root != NULL ? g_file_get_uri (anjuta_project_node_get_file (project->root)) : NULL;
}

GFile*
amp_project_get_file (AmpProject *project)
{
	g_return_val_if_fail (project != NULL, NULL);

	return anjuta_project_node_get_file (project->root);
}

AnjutaProjectProperty *
amp_project_get_property_list (AmpProject *project)
{
	return project->properties;
}

#if 0
gchar *
amp_project_get_property (AmpProject *project, AmpPropertyType type)
{
	const gchar *value = NULL;

	if (project->property != NULL)
	{
		switch (type)
		{
			case AMP_PROPERTY_NAME:
				value = project->property->name;
				break;
			case AMP_PROPERTY_VERSION:
				value = project->property->version;
				break;
			case AMP_PROPERTY_BUG_REPORT:
				value = project->property->bug_report;
				break;
			case AMP_PROPERTY_TARNAME:
				value = project->property->tarname;
				if (value == NULL) return ac_init_default_tarname (project->property->name);
				break;
			case AMP_PROPERTY_URL:
				value = project->property->url;
				break;
		}
	}

	return value == NULL ? NULL : g_strdup (value);
}

gboolean
amp_project_set_property (AmpProject *project, AmpPropertyType type, const gchar *value)
{
	if (project->property == NULL)
	{
		project->property = amp_property_new (NULL, NULL);
	}
	switch (type)
	{
		case AMP_PROPERTY_NAME:
			STR_REPLACE (project->property->name, value);
			break;
		case AMP_PROPERTY_VERSION:
			STR_REPLACE (project->property->version, value);
			break;
		case AMP_PROPERTY_BUG_REPORT:
			STR_REPLACE (project->property->bug_report, value);
			break;
		case AMP_PROPERTY_TARNAME:
			STR_REPLACE (project->property->tarname, value);
			break;
		case AMP_PROPERTY_URL:
			STR_REPLACE (project->property->url, value);
			break;
	}
	
	return amp_project_update_property (project, type);
}
#endif

/* Implement IAnjutaProject
 *---------------------------------------------------------------------------*/

static gboolean
iproject_load_node (IAnjutaProject *obj, AnjutaProjectNode *node, GError **err)
{
	if (node == NULL) node = AMP_PROJECT (obj)->root;
	
	node = amp_project_load_node (AMP_PROJECT (obj), node, err);

	return node != NULL;
}

static gboolean
iproject_save_node (IAnjutaProject *obj, AnjutaProjectNode *node, GError **error)
{
	if (node == NULL) node = AMP_PROJECT (obj)->root;
	
	switch (anjuta_project_node_get_node_type (node))
	{
		case ANJUTA_PROJECT_ROOT:
			if (!amp_project_save (AMP_PROJECT (obj), error))
			{
				node = NULL;
			}
			break;
		default:
			node = project_node_save (AMP_PROJECT (obj), node, error);
			break;
	}
	
	return node != NULL;
}

static AnjutaProjectNode *
iproject_add_node_before (IAnjutaProject *obj, AnjutaProjectNode *parent, AnjutaProjectNode *sibling, AnjutaProjectNodeType type, GFile *file, const gchar *name, GError **err)
{
	AnjutaProjectNode *node;
	GFile *directory = NULL;
	
	switch (type & ANJUTA_PROJECT_TYPE_MASK)
	{
		case ANJUTA_PROJECT_GROUP:
			if ((file == NULL) && (name != NULL))
			{
				directory = g_file_get_child (AMP_GROUP_DATA (parent)->base.file, name);
				file = directory;
			}
			node = project_node_new (AMP_PROJECT (obj), type, file, name, err);
			if (directory != NULL) g_object_unref (directory);
			anjuta_project_node_insert_before (parent, sibling, node);
			amp_group_fill_token (AMP_PROJECT (obj), node, NULL);
			break;
		case ANJUTA_PROJECT_TARGET:
			node = project_node_new (AMP_PROJECT (obj), type, file, name, err);
			anjuta_project_node_insert_before (parent, sibling, node);
			amp_target_fill_token (AMP_PROJECT (obj), node, NULL);
			break;
		case ANJUTA_PROJECT_SOURCE:
			node = project_node_new (AMP_PROJECT (obj), type, file, name, err);
			anjuta_project_node_insert_before (parent, sibling, node);
			amp_source_fill_token (AMP_PROJECT (obj), node, NULL);
			break;
		default:
			node = project_node_new (AMP_PROJECT (obj), type, file, name, err);
			anjuta_project_node_insert_before (parent, sibling, node);
			break;
	}
	
	return node;
}

static AnjutaProjectNode *
iproject_add_node_after (IAnjutaProject *obj, AnjutaProjectNode *parent, AnjutaProjectNode *sibling, AnjutaProjectNodeType type, GFile *file, const gchar *name, GError **err)
{
	AnjutaProjectNode *node;
	GFile *directory = NULL;

	switch (type & ANJUTA_PROJECT_TYPE_MASK)
	{
		case ANJUTA_PROJECT_GROUP:
			if ((file == NULL) && (name != NULL))
			{
				directory = g_file_get_child (AMP_GROUP_DATA (parent)->base.file, name);
				file = directory;
			}
			node = project_node_new (AMP_PROJECT (obj), type, file, name, err);
			if (directory != NULL) g_object_unref (directory);
			anjuta_project_node_insert_after (parent, sibling, node);
			amp_group_fill_token (AMP_PROJECT (obj), node, NULL);
			break;
		case ANJUTA_PROJECT_TARGET:
			node = project_node_new (AMP_PROJECT (obj), type, file, name, err);
			anjuta_project_node_insert_after (parent, sibling, node);
			amp_target_fill_token (AMP_PROJECT (obj), node, NULL);
			break;
		case ANJUTA_PROJECT_SOURCE:
			node = project_node_new (AMP_PROJECT (obj), type, file, name, err);
			anjuta_project_node_insert_after (parent, sibling, node);
			amp_source_fill_token (AMP_PROJECT (obj), node, NULL);
			break;
		default:
			node = project_node_new (AMP_PROJECT (obj), type, file, name, err);
			anjuta_project_node_insert_after (parent, sibling, node);
			break;
	}
	
	return node;
}

static gboolean
iproject_remove_node (IAnjutaProject *obj, AnjutaProjectNode *node, GError **err)
{
	AnjutaProjectNodeType type = anjuta_project_node_get_node_type (node);
	
	anjuta_project_node_set_state (node, ANJUTA_PROJECT_REMOVED);
	
	switch (type & ANJUTA_PROJECT_TYPE_MASK)
	{
		case ANJUTA_PROJECT_GROUP:
			break;
		case ANJUTA_PROJECT_TARGET:
			amp_target_remove_token (AMP_PROJECT (obj), node, NULL);
			break;
		case ANJUTA_PROJECT_SOURCE:
			break;
		default:
			break;
	}

	return TRUE;
}

static AnjutaProjectProperty *
iproject_set_property (IAnjutaProject *obj, AnjutaProjectNode *node, AnjutaProjectProperty *property, const gchar *value, GError **error)
{
	AnjutaProjectProperty *new_prop;
	
	new_prop = amp_node_property_set (node, property, value);
	amp_project_update_property (AMP_PROJECT (obj), new_prop);
	
	return new_prop;
}

static gboolean
iproject_remove_property (IAnjutaProject *obj, AnjutaProjectNode *node, AnjutaProjectProperty *property, GError **error)
{
	AnjutaProjectProperty *old_prop;

	old_prop = anjuta_project_node_remove_property (node, property);
	amp_property_free (old_prop);
	
	return TRUE;
}

static AnjutaProjectNode *
iproject_get_root (IAnjutaProject *obj, GError **err)
{
	return AMP_PROJECT (obj)->root;
}

static GList* 
iproject_get_node_info (IAnjutaProject *obj, GError **err)
{
	return amp_project_get_node_info (AMP_PROJECT (obj), err);
}

static void
iproject_iface_init(IAnjutaProjectIface* iface)
{
	iface->load_node = iproject_load_node;
	iface->save_node = iproject_save_node;
	iface->add_node_before = iproject_add_node_before;
	iface->add_node_after = iproject_add_node_after;
	iface->remove_node = iproject_remove_node;
	iface->set_property = iproject_set_property;
	iface->remove_property = iproject_remove_property;
	iface->get_root = iproject_get_root;
	iface->get_node_info = iproject_get_node_info;
}

/* Group access functions
 *---------------------------------------------------------------------------*/

GFile*
amp_group_get_directory (AnjutaAmGroupNode *group)
{
	return AMP_GROUP_DATA (group)->base.file;
}

GFile*
amp_group_get_makefile (AnjutaAmGroupNode *group)
{
	return AMP_GROUP_DATA (group)->makefile;
}

gchar *
amp_group_get_id (AnjutaAmGroupNode *group)
{
	return g_file_get_uri (AMP_GROUP_DATA (group)->base.file);
}

void
amp_group_update_variable (AnjutaAmGroupNode *group, AnjutaToken *variable)
{
	AnjutaToken *arg;
	char *name = NULL;
	AnjutaToken *value = NULL;
	AmpVariable *var;

	arg = anjuta_token_first_item (variable);
	name = g_strstrip (anjuta_token_evaluate (arg));
	arg = anjuta_token_next_item (arg);
	value = anjuta_token_next_item (arg);

	var = (AmpVariable *)g_hash_table_lookup (AMP_GROUP_DATA (group)->variables, name);
	if (var != NULL)
	{
		var->value = value;
	}
	else
	{
		var = amp_variable_new (name, 0, value);
		g_hash_table_insert (AMP_GROUP_DATA (group)->variables, var->name, var);
	}

	if (name) g_free (name);
}

AnjutaToken*
amp_group_get_variable_token (AnjutaAmGroupNode *group, AnjutaToken *variable)
{
	guint length;
	const gchar *string;
	gchar *name;
	AmpVariable *var;
		
	length = anjuta_token_get_length (variable);
	string = anjuta_token_get_string (variable);
	if (string[1] == '(')
	{
		name = g_strndup (string + 2, length - 3);
	}
	else
	{
		name = g_strndup (string + 1, 1);
	}
	var = g_hash_table_lookup (AMP_GROUP_DATA (group)->variables, name);
	g_free (name);

	return var != NULL ? var->value : NULL;
}

/* Target access functions
 *---------------------------------------------------------------------------*/

const gchar *
amp_target_get_name (AnjutaAmTargetNode *target)
{
	return AMP_TARGET_DATA (target)->base.name;
}

gchar *
amp_target_get_id (AnjutaAmTargetNode *target)
{
	return g_base64_encode ((guchar *)&target, sizeof (target));
}

/* Source access functions
 *---------------------------------------------------------------------------*/

gchar *
amp_source_get_id (AnjutaAmSourceNode *source)
{
	return g_base64_encode ((guchar *)&source, sizeof (source));
}

GFile*
amp_source_get_file (AnjutaAmSourceNode *source)
{
	return AMP_SOURCE_DATA (source)->base.file;
}

/* GbfProject implementation
 *---------------------------------------------------------------------------*/

static void
amp_project_dispose (GObject *object)
{
	AmpProject *project;
	
	g_return_if_fail (AMP_IS_PROJECT (object));

	project = AMP_PROJECT (object);
	amp_project_unload (project);
	if (project->root) amp_root_free (project->root);

	G_OBJECT_CLASS (parent_class)->dispose (object);	
}

static void
amp_project_instance_init (AmpProject *project)
{
	g_return_if_fail (project != NULL);
	g_return_if_fail (AMP_IS_PROJECT (project));
	
	/* project data */
	project->root = NULL;
	project->configure_file = NULL;
	project->configure_token = NULL;
	project->properties = amp_get_project_property_list ();
	project->ac_init = NULL;
	project->args = NULL;

	project->am_space_list = NULL;
	project->ac_space_list = NULL;
	project->arg_list = NULL;
}

static void
amp_project_class_init (AmpProjectClass *klass)
{
	GObjectClass *object_class;
	
	parent_class = g_type_class_peek_parent (klass);

	object_class = G_OBJECT_CLASS (klass);
	object_class->dispose = amp_project_dispose;
}

ANJUTA_TYPE_BEGIN(AmpProject, amp_project, G_TYPE_OBJECT);
ANJUTA_TYPE_ADD_INTERFACE(iproject, IANJUTA_TYPE_PROJECT);
ANJUTA_TYPE_END;