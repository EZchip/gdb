/* Generic static probe support for GDB.

   Copyright (C) 2012 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "probe.h"
#include "command.h"
#include "cli/cli-cmds.h"
#include "cli/cli-utils.h"
#include "objfiles.h"
#include "symtab.h"
#include "progspace.h"
#include "filenames.h"
#include "exceptions.h"
#include "linespec.h"
#include "gdb_regex.h"
#include "frame.h"
#include "arch-utils.h"
#include <ctype.h>



/* See definition in probe.h.  */

struct symtabs_and_lines
parse_probes (char **argptr, struct linespec_result *canonical)
{
  char *arg_start, *arg_end, *arg;
  char *objfile_name = NULL, *provider = NULL, *name, *p;
  struct cleanup *cleanup;
  struct symtabs_and_lines result;
  struct objfile *objfile;
  struct program_space *pspace;
  const struct probe_ops *probe_ops;
  const char *cs;

  result.sals = NULL;
  result.nelts = 0;

  arg_start = *argptr;

  cs = *argptr;
  probe_ops = probe_linespec_to_ops (&cs);
  gdb_assert (probe_ops != NULL);

  arg = (char *) cs;
  arg = skip_spaces (arg);
  if (!*arg)
    error (_("argument to `%s' missing"), arg_start);

  arg_end = skip_to_space (arg);

  /* We make a copy here so we can write over parts with impunity.  */
  arg = savestring (arg, arg_end - arg);
  cleanup = make_cleanup (xfree, arg);

  /* Extract each word from the argument, separated by ":"s.  */
  p = strchr (arg, ':');
  if (p == NULL)
    {
      /* This is `-p name'.  */
      name = arg;
    }
  else
    {
      char *hold = p + 1;

      *p = '\0';
      p = strchr (hold, ':');
      if (p == NULL)
	{
	  /* This is `-p provider:name'.  */
	  provider = arg;
	  name = hold;
	}
      else
	{
	  /* This is `-p objfile:provider:name'.  */
	  *p = '\0';
	  objfile_name = arg;
	  provider = hold;
	  name = p + 1;
	}
    }

  if (*name == '\0')
    error (_("no probe name specified"));
  if (provider && *provider == '\0')
    error (_("invalid provider name"));
  if (objfile_name && *objfile_name == '\0')
    error (_("invalid objfile name"));

  ALL_PSPACES (pspace)
    ALL_PSPACE_OBJFILES (pspace, objfile)
      {
	VEC (probe_p) *probes;
	struct probe *probe;
	int ix;

	if (!objfile->sf || !objfile->sf->sym_probe_fns)
	  continue;

	if (objfile_name
	    && FILENAME_CMP (objfile->name, objfile_name) != 0
	    && FILENAME_CMP (lbasename (objfile->name), objfile_name) != 0)
	  continue;

	probes = objfile->sf->sym_probe_fns->sym_get_probes (objfile);

	for (ix = 0; VEC_iterate (probe_p, probes, ix, probe); ix++)
	  {
	    struct symtab_and_line *sal;

	    if (probe_ops != &probe_ops_any && probe->pops != probe_ops)
	      continue;

	    if (provider && strcmp (probe->provider, provider) != 0)
	      continue;

	    if (strcmp (probe->name, name) != 0)
	      continue;

	    ++result.nelts;
	    result.sals = xrealloc (result.sals,
				    result.nelts
				    * sizeof (struct symtab_and_line));
	    sal = &result.sals[result.nelts - 1];

	    init_sal (sal);

	    sal->pc = probe->address;
	    sal->explicit_pc = 1;
	    sal->section = find_pc_overlay (sal->pc);
	    sal->pspace = pspace;
	    sal->probe = probe;
	  }
      }

  if (result.nelts == 0)
    {
      throw_error (NOT_FOUND_ERROR,
		   _("No probe matching objfile=`%s', provider=`%s', name=`%s'"),
		   objfile_name ? objfile_name : _("<any>"),
		   provider ? provider : _("<any>"),
		   name);
    }

  if (canonical)
    {
      canonical->special_display = 1;
      canonical->pre_expanded = 1;
      canonical->addr_string = savestring (*argptr, arg_end - *argptr);
    }

  *argptr = arg_end;
  do_cleanups (cleanup);

  return result;
}

/* See definition in probe.h.  */

VEC (probe_p) *
find_probes_in_objfile (struct objfile *objfile, const char *provider,
			const char *name)
{
  VEC (probe_p) *probes, *result = NULL;
  int ix;
  struct probe *probe;

  if (!objfile->sf || !objfile->sf->sym_probe_fns)
    return NULL;

  probes = objfile->sf->sym_probe_fns->sym_get_probes (objfile);
  for (ix = 0; VEC_iterate (probe_p, probes, ix, probe); ix++)
    {
      if (strcmp (probe->provider, provider) != 0)
	continue;

      if (strcmp (probe->name, name) != 0)
	continue;

      VEC_safe_push (probe_p, result, probe);
    }

  return result;
}

/* See definition in probe.h.  */

struct probe *
find_probe_by_pc (CORE_ADDR pc, struct objfile **objfile_out)
{
  struct objfile *objfile;

  ALL_OBJFILES (objfile)
  {
    VEC (probe_p) *probes;
    int ix;
    struct probe *probe;

    if (!objfile->sf || !objfile->sf->sym_probe_fns)
      continue;

    /* If this proves too inefficient, we can replace with a hash.  */
    probes = objfile->sf->sym_probe_fns->sym_get_probes (objfile);
    for (ix = 0; VEC_iterate (probe_p, probes, ix, probe); ix++)
      if (probe->address == pc)
	{
	  *objfile_out = objfile;
	  return probe;
	}
  }

  return NULL;
}



/* A utility structure.  A VEC of these is built when handling "info
   probes".  */

struct probe_and_objfile
{
  /* The probe.  */
  struct probe *probe;

  /* The probe's objfile.  */
  struct objfile *objfile;
};

typedef struct probe_and_objfile probe_and_objfile_s;
DEF_VEC_O (probe_and_objfile_s);

/* A helper function for collect_probes that compiles a regexp and
   throws an exception on error.  This installs a cleanup to free the
   resulting pattern on success.  If RX is NULL, this does nothing.  */

static void
compile_rx_or_error (regex_t *pattern, const char *rx, const char *message)
{
  int code;

  if (!rx)
    return;

  code = regcomp (pattern, rx, REG_NOSUB);
  if (code == 0)
    make_regfree_cleanup (pattern);
  else
    {
      char *err = get_regcomp_error (code, pattern);

      make_cleanup (xfree, err);
      error (("%s: %s"), message, err);
    }
}

/* Make a vector of probes matching OBJNAME, PROVIDER, and PROBE_NAME.
   If POPS is not NULL, only probes of this certain probe_ops will match.
   Each argument is a regexp, or NULL, which matches anything.  */

static VEC (probe_and_objfile_s) *
collect_probes (char *objname, char *provider, char *probe_name,
		const struct probe_ops *pops)
{
  struct objfile *objfile;
  VEC (probe_and_objfile_s) *result = NULL;
  struct cleanup *cleanup, *cleanup_temps;
  regex_t obj_pat, prov_pat, probe_pat;

  cleanup = make_cleanup (VEC_cleanup (probe_and_objfile_s), &result);

  cleanup_temps = make_cleanup (null_cleanup, NULL);
  compile_rx_or_error (&prov_pat, provider, _("Invalid provider regexp"));
  compile_rx_or_error (&probe_pat, probe_name, _("Invalid probe regexp"));
  compile_rx_or_error (&obj_pat, objname, _("Invalid object file regexp"));

  ALL_OBJFILES (objfile)
    {
      VEC (probe_p) *probes;
      struct probe *probe;
      int ix;

      if (! objfile->sf || ! objfile->sf->sym_probe_fns)
	continue;

      if (objname)
	{
	  if (regexec (&obj_pat, objfile->name, 0, NULL, 0) != 0)
	    continue;
	}

      probes = objfile->sf->sym_probe_fns->sym_get_probes (objfile);

      for (ix = 0; VEC_iterate (probe_p, probes, ix, probe); ix++)
	{
	  probe_and_objfile_s entry;

	  if (pops != NULL && probe->pops != pops)
	    continue;

	  if (provider
	      && regexec (&prov_pat, probe->provider, 0, NULL, 0) != 0)
	    continue;

	  if (probe_name
	      && regexec (&probe_pat, probe->name, 0, NULL, 0) != 0)
	    continue;

	  entry.probe = probe;
	  entry.objfile = objfile;
	  VEC_safe_push (probe_and_objfile_s, result, &entry);
	}
    }

  do_cleanups (cleanup_temps);
  discard_cleanups (cleanup);
  return result;
}

/* A qsort comparison function for probe_and_objfile_s objects.  */

static int
compare_entries (const void *a, const void *b)
{
  const probe_and_objfile_s *ea = a;
  const probe_and_objfile_s *eb = b;
  int v;

  v = strcmp (ea->probe->provider, eb->probe->provider);
  if (v)
    return v;

  v = strcmp (ea->probe->name, eb->probe->name);
  if (v)
    return v;

  if (ea->probe->address < eb->probe->address)
    return -1;
  if (ea->probe->address > eb->probe->address)
    return 1;

  return strcmp (ea->objfile->name, eb->objfile->name);
}

/* Helper function that generate entries in the ui_out table being
   crafted by `info_probes_for_ops'.  */

static void
gen_ui_out_table_header_info (VEC (probe_and_objfile_s) *probes,
			      const struct probe_ops *p)
{
  /* `headings' refers to the names of the columns when printing `info
     probes'.  */
  VEC (info_probe_column_s) *headings = NULL;
  struct cleanup *c;
  info_probe_column_s *column;
  size_t headings_size;
  int ix;

  gdb_assert (p != NULL);

  if (p->gen_info_probes_table_header == NULL
      && p->gen_info_probes_table_values == NULL)
    return;

  gdb_assert (p->gen_info_probes_table_header != NULL
	      && p->gen_info_probes_table_values != NULL);

  c = make_cleanup (VEC_cleanup (info_probe_column_s), &headings);
  p->gen_info_probes_table_header (&headings);

  headings_size = VEC_length (info_probe_column_s, headings);

  for (ix = 0;
       VEC_iterate (info_probe_column_s, headings, ix, column);
       ++ix)
    {
      probe_and_objfile_s *entry;
      int jx;
      size_t size_max = strlen (column->print_name);

      for (jx = 0; VEC_iterate (probe_and_objfile_s, probes, jx, entry); ++jx)
	{
	  /* `probe_fields' refers to the values of each new field that this
	     probe will display.  */
	  VEC (const_char_ptr) *probe_fields = NULL;
	  struct cleanup *c2;
	  const char *val;
	  int kx;

	  if (entry->probe->pops != p)
	    continue;

	  c2 = make_cleanup (VEC_cleanup (const_char_ptr), &probe_fields);
	  p->gen_info_probes_table_values (entry->probe, entry->objfile,
					   &probe_fields);

	  gdb_assert (VEC_length (const_char_ptr, probe_fields)
		      == headings_size);

	  for (kx = 0; VEC_iterate (const_char_ptr, probe_fields, kx, val);
	       ++kx)
	    {
	      /* It is valid to have a NULL value here, which means that the
		 backend does not have something to write and this particular
		 field should be skipped.  */
	      if (val == NULL)
		continue;

	      size_max = max (strlen (val), size_max);
	    }
	  do_cleanups (c2);
	}

      ui_out_table_header (current_uiout, size_max, ui_left,
			   column->field_name, column->print_name);
    }

  do_cleanups (c);
}

/* Helper function to print extra information about a probe and an objfile
   represented by ENTRY.  */

static void
print_ui_out_info (probe_and_objfile_s *entry)
{
  int ix;
  int j = 0;
  /* `values' refers to the actual values of each new field in the output
     of `info probe'.  `headings' refers to the names of each new field.  */
  VEC (const_char_ptr) *values = NULL;
  VEC (info_probe_column_s) *headings = NULL;
  info_probe_column_s *column;
  struct cleanup *c;

  gdb_assert (entry != NULL);
  gdb_assert (entry->probe != NULL);
  gdb_assert (entry->probe->pops != NULL);

  if (entry->probe->pops->gen_info_probes_table_header == NULL
      && entry->probe->pops->gen_info_probes_table_values == NULL)
    return;

  gdb_assert (entry->probe->pops->gen_info_probes_table_header != NULL
	      && entry->probe->pops->gen_info_probes_table_values != NULL);

  c = make_cleanup (VEC_cleanup (info_probe_column_s), &headings);
  make_cleanup (VEC_cleanup (const_char_ptr), &values);

  entry->probe->pops->gen_info_probes_table_header (&headings);
  entry->probe->pops->gen_info_probes_table_values (entry->probe,
						    entry->objfile, &values);

  gdb_assert (VEC_length (info_probe_column_s, headings)
	      == VEC_length (const_char_ptr, values));

  for (ix = 0;
       VEC_iterate (info_probe_column_s, headings, ix, column);
       ++ix)
    {
      const char *val = VEC_index (const_char_ptr, values, j++);

      if (val == NULL)
	ui_out_field_skip (current_uiout, column->field_name);
      else
	ui_out_field_string (current_uiout, column->field_name, val);
    }

  do_cleanups (c);
}

/* Helper function that returns the number of extra fields which POPS will
   need.  */

static int
get_number_extra_fields (const struct probe_ops *pops)
{
  VEC (info_probe_column_s) *headings = NULL;
  struct cleanup *c;
  int n;

  if (pops->gen_info_probes_table_header == NULL)
    return 0;

  c = make_cleanup (VEC_cleanup (info_probe_column_s), &headings);
  pops->gen_info_probes_table_header (&headings);

  n = VEC_length (info_probe_column_s, headings);

  do_cleanups (c);

  return n;
}

/* See comment in probe.h.  */

void
info_probes_for_ops (char *arg, int from_tty, const struct probe_ops *pops)
{
  char *provider, *probe = NULL, *objname = NULL;
  struct cleanup *cleanup = make_cleanup (null_cleanup, NULL);
  VEC (probe_and_objfile_s) *items;
  int i, any_found;
  int ui_out_extra_fields = 0;
  size_t size_addr;
  size_t size_name = strlen ("Name");
  size_t size_objname = strlen ("Object");
  size_t size_provider = strlen ("Provider");
  probe_and_objfile_s *entry;
  struct gdbarch *gdbarch = get_current_arch ();

  /* Do we have a `provider:probe:objfile' style of linespec?  */
  provider = extract_arg (&arg);
  if (provider)
    {
      make_cleanup (xfree, provider);

      probe = extract_arg (&arg);
      if (probe)
	{
	  make_cleanup (xfree, probe);

	  objname = extract_arg (&arg);
	  if (objname)
	    make_cleanup (xfree, objname);
	}
    }

  if (pops == NULL)
    {
      const struct probe_ops *po;
      int ix;

      /* If the probe_ops is NULL, it means the user has requested a "simple"
	 `info probes', i.e., she wants to print all information about all
	 probes.  For that, we have to identify how many extra fields we will
	 need to add in the ui_out table.

	 To do that, we iterate over all probe_ops, querying each one about
	 its extra fields, and incrementing `ui_out_extra_fields' to reflect
	 that number.  */

      for (ix = 0; VEC_iterate (probe_ops_cp, all_probe_ops, ix, po); ++ix)
	ui_out_extra_fields += get_number_extra_fields (po);
    }
  else
    ui_out_extra_fields = get_number_extra_fields (pops);

  items = collect_probes (objname, provider, probe, pops);
  make_cleanup (VEC_cleanup (probe_and_objfile_s), &items);
  make_cleanup_ui_out_table_begin_end (current_uiout,
				       4 + ui_out_extra_fields,
				       VEC_length (probe_and_objfile_s, items),
				       "StaticProbes");

  if (!VEC_empty (probe_and_objfile_s, items))
    qsort (VEC_address (probe_and_objfile_s, items),
	   VEC_length (probe_and_objfile_s, items),
	   sizeof (probe_and_objfile_s), compare_entries);

  /* What's the size of an address in our architecture?  */
  size_addr = gdbarch_addr_bit (gdbarch) == 64 ? 18 : 10;

  /* Determining the maximum size of each field (`provider', `name' and
     `objname').  */
  for (i = 0; VEC_iterate (probe_and_objfile_s, items, i, entry); ++i)
    {
      size_name = max (strlen (entry->probe->name), size_name);
      size_provider = max (strlen (entry->probe->provider), size_provider);
      size_objname = max (strlen (entry->objfile->name), size_objname);
    }

  ui_out_table_header (current_uiout, size_provider, ui_left, "provider",
		       _("Provider"));
  ui_out_table_header (current_uiout, size_name, ui_left, "name", _("Name"));
  ui_out_table_header (current_uiout, size_addr, ui_left, "addr", _("Where"));

  if (pops == NULL)
    {
      const struct probe_ops *po;
      int ix;

      /* We have to generate the table header for each new probe type that we
	 will print.  */
      for (ix = 0; VEC_iterate (probe_ops_cp, all_probe_ops, ix, po); ++ix)
	gen_ui_out_table_header_info (items, po);
    }
  else
    gen_ui_out_table_header_info (items, pops);

  ui_out_table_header (current_uiout, size_objname, ui_left, "object",
		       _("Object"));
  ui_out_table_body (current_uiout);

  for (i = 0; VEC_iterate (probe_and_objfile_s, items, i, entry); ++i)
    {
      struct cleanup *inner;

      inner = make_cleanup_ui_out_tuple_begin_end (current_uiout, "probe");

      ui_out_field_string (current_uiout, "provider", entry->probe->provider);
      ui_out_field_string (current_uiout, "name", entry->probe->name);
      ui_out_field_core_addr (current_uiout, "addr",
			      get_objfile_arch (entry->objfile),
			      entry->probe->address);

      if (pops == NULL)
	{
	  const struct probe_ops *po;
	  int ix;

	  for (ix = 0; VEC_iterate (probe_ops_cp, all_probe_ops, ix, po);
	       ++ix)
	    if (entry->probe->pops == po)
	      print_ui_out_info (entry);
	}
      else
	print_ui_out_info (entry);

      ui_out_field_string (current_uiout, "object", entry->objfile->name);
      ui_out_text (current_uiout, "\n");

      do_cleanups (inner);
    }

  any_found = !VEC_empty (probe_and_objfile_s, items);
  do_cleanups (cleanup);

  if (!any_found)
    ui_out_message (current_uiout, 0, _("No probes matched.\n"));
}

/* Implementation of the `info probes' command.  */

static void
info_probes_command (char *arg, int from_tty)
{
  info_probes_for_ops (arg, from_tty, NULL);
}

/* See comments in probe.h.  */

struct value *
probe_safe_evaluate_at_pc (struct frame_info *frame, unsigned n)
{
  struct probe *probe;
  struct objfile *objfile;
  unsigned n_probes;

  probe = find_probe_by_pc (get_frame_pc (frame), &objfile);
  if (!probe)
    return NULL;
  gdb_assert (objfile->sf && objfile->sf->sym_probe_fns);

  n_probes
    = objfile->sf->sym_probe_fns->sym_get_probe_argument_count (objfile,
								probe);
  if (n >= n_probes)
    return NULL;

  return objfile->sf->sym_probe_fns->sym_evaluate_probe_argument (objfile,
								  probe,
								  n);
}

/* See comment in probe.h.  */

const struct probe_ops *
probe_linespec_to_ops (const char **linespecp)
{
  int ix;
  const struct probe_ops *probe_ops;

  for (ix = 0; VEC_iterate (probe_ops_cp, all_probe_ops, ix, probe_ops); ix++)
    if (probe_ops->is_linespec (linespecp))
      return probe_ops;

  return NULL;
}

/* See comment in probe.h.  */

int
probe_is_linespec_by_keyword (const char **linespecp, const char *const *keywords)
{
  const char *s = *linespecp;
  const char *const *csp;

  for (csp = keywords; *csp; csp++)
    {
      const char *keyword = *csp;
      size_t len = strlen (keyword);

      if (strncmp (s, keyword, len) == 0 && isspace (s[len]))
	{
	  *linespecp += len + 1;
	  return 1;
	}
    }

  return 0;
}

/* Implementation of `is_linespec' method for `struct probe_ops'.  */

static int
probe_any_is_linespec (const char **linespecp)
{
  static const char *const keywords[] = { "-p", "-probe", NULL };

  return probe_is_linespec_by_keyword (linespecp, keywords);
}

/* Dummy method used for `probe_ops_any'.  */

static void
probe_any_get_probes (VEC (probe_p) **probesp, struct objfile *objfile)
{
  /* No probes can be provided by this dummy backend.  */
}

/* Operations associated with a generic probe.  */

const struct probe_ops probe_ops_any =
{
  probe_any_is_linespec,
  probe_any_get_probes,
};

/* See comments in probe.h.  */

struct cmd_list_element **
info_probes_cmdlist_get (void)
{
  static struct cmd_list_element *info_probes_cmdlist;

  if (info_probes_cmdlist == NULL)
    add_prefix_cmd ("probes", class_info, info_probes_command,
		    _("\
Show available static probes.\n\
Usage: info probes [all|TYPE [ARGS]]\n\
TYPE specifies the type of the probe, and can be one of the following:\n\
  - stap\n\
If you specify TYPE, there may be additional arguments needed by the\n\
subcommand.\n\
If you do not specify any argument, or specify `all', then the command\n\
will show information about all types of probes."),
		    &info_probes_cmdlist, "info probes ",
		    0/*allow-unknown*/, &infolist);

  return &info_probes_cmdlist;
}

VEC (probe_ops_cp) *all_probe_ops;

void _initialize_probe (void);

void
_initialize_probe (void)
{
  VEC_safe_push (probe_ops_cp, all_probe_ops, &probe_ops_any);

  add_cmd ("all", class_info, info_probes_command,
	   _("\
Show information about all type of probes."),
	   info_probes_cmdlist_get ());
}
