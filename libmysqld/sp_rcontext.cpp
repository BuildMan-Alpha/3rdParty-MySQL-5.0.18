/* Copyright (C) 2002 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "mysql_priv.h"
#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation
#endif

#if defined(WIN32) || defined(__WIN__)
#undef SAFEMALLOC				/* Problems with threads */
#endif

#include "mysql.h"
#include "sp_head.h"
#include "sql_cursor.h"
#include "sp_rcontext.h"
#include "sp_pcontext.h"


sp_rcontext::sp_rcontext(sp_pcontext *root_parsing_ctx,
                         Field *return_value_fld,
                         sp_rcontext *prev_runtime_ctx)
  :m_root_parsing_ctx(root_parsing_ctx),
   m_var_table(0),
   m_var_items(0),
   m_return_value_fld(return_value_fld),
   m_return_value_set(FALSE),
   m_hcount(0),
   m_hsp(0),
   m_ihsp(0),
   m_hfound(-1),
   m_ccount(0),
   m_case_expr_holders(0),
   m_prev_runtime_ctx(prev_runtime_ctx)
{
}


sp_rcontext::~sp_rcontext()
{
  if (m_var_table)
    free_blobs(m_var_table);
}


/*
  Initialize sp_rcontext instance.

  SYNOPSIS
    thd   Thread handle
  RETURN
    FALSE   on success
    TRUE    on error
*/

bool sp_rcontext::init(THD *thd)
{
  if (init_var_table(thd) || init_var_items())
    return TRUE;

  return
    !(m_handler=
      (sp_handler_t*)thd->alloc(m_root_parsing_ctx->max_handlers() *
                                sizeof(sp_handler_t))) ||
    !(m_hstack=
      (uint*)thd->alloc(m_root_parsing_ctx->max_handlers() *
                        sizeof(uint))) ||
    !(m_in_handler=
      (uint*)thd->alloc(m_root_parsing_ctx->max_handlers() *
                        sizeof(uint))) ||
    !(m_cstack=
      (sp_cursor**)thd->alloc(m_root_parsing_ctx->max_cursors() *
                              sizeof(sp_cursor*))) ||
    !(m_case_expr_holders=
      (Item_cache**)thd->calloc(m_root_parsing_ctx->get_num_case_exprs() *
                               sizeof (Item_cache*)));
}


/*
  Create and initialize a table to store SP-vars.

  SYNOPSIS
    thd   Thread handler.
  RETURN
    FALSE   on success
    TRUE    on error
*/

bool
sp_rcontext::init_var_table(THD *thd)
{
  List<create_field> field_def_lst;

  if (!m_root_parsing_ctx->total_pvars())
    return FALSE;

  m_root_parsing_ctx->retrieve_field_definitions(&field_def_lst);

  DBUG_ASSERT(field_def_lst.elements == m_root_parsing_ctx->total_pvars());
  
  if (!(m_var_table= create_virtual_tmp_table(thd, field_def_lst)))
    return TRUE;

  m_var_table->copy_blobs= TRUE;
  m_var_table->alias= "";

  return FALSE;
}


/*
  Create and initialize an Item-adapter (Item_field) for each SP-var field.

  RETURN
    FALSE   on success
    TRUE    on error
*/

bool
sp_rcontext::init_var_items()
{
  uint idx;
  uint num_vars= m_root_parsing_ctx->total_pvars();

  if (!(m_var_items= (Item**) sql_alloc(num_vars * sizeof (Item *))))
    return TRUE;

  for (idx = 0; idx < num_vars; ++idx)
  {
    if (!(m_var_items[idx]= new Item_field(m_var_table->field[idx])))
      return TRUE;
  }

  return FALSE;
}


bool
sp_rcontext::set_return_value(THD *thd, Item *return_value_item)
{
  DBUG_ASSERT(m_return_value_fld);

  m_return_value_set = TRUE;

  return sp_eval_expr(thd, m_return_value_fld, return_value_item);
}


bool
sp_rcontext::find_handler(uint sql_errno,
                          MYSQL_ERROR::enum_warning_level level)
{
  if (m_hfound >= 0)
    return 1;			// Already got one

  const char *sqlstate= mysql_errno_to_sqlstate(sql_errno);
  int i= m_hcount, found= -1;

  while (i--)
  {
    sp_cond_type_t *cond= m_handler[i].cond;
    int j= m_ihsp;

    while (j--)
      if (m_in_handler[j] == m_handler[i].handler)
	break;
    if (j >= 0)
      continue;                 // Already executing this handler

    switch (cond->type)
    {
    case sp_cond_type_t::number:
      if (sql_errno == cond->mysqlerr)
	found= i;		// Always the most specific
      break;
    case sp_cond_type_t::state:
      if (strcmp(sqlstate, cond->sqlstate) == 0 &&
	  (found < 0 || m_handler[found].cond->type > sp_cond_type_t::state))
	found= i;
      break;
    case sp_cond_type_t::warning:
      if ((sqlstate[0] == '0' && sqlstate[1] == '1' ||
	   level == MYSQL_ERROR::WARN_LEVEL_WARN) &&
	  found < 0)
	found= i;
      break;
    case sp_cond_type_t::notfound:
      if (sqlstate[0] == '0' && sqlstate[1] == '2' &&
	  found < 0)
	found= i;
      break;
    case sp_cond_type_t::exception:
      if ((sqlstate[0] != '0' || sqlstate[1] > '2') &&
	  level == MYSQL_ERROR::WARN_LEVEL_ERROR &&
	  found < 0)
	found= i;
      break;
    }
  }
  if (found < 0)
  {
    if (m_prev_runtime_ctx)
      return m_prev_runtime_ctx->find_handler(sql_errno, level);
    return FALSE;
  }
  m_hfound= found;
  return TRUE;
}


void
sp_rcontext::push_cursor(sp_lex_keeper *lex_keeper, sp_instr_cpush *i)
{
  m_cstack[m_ccount++]= new sp_cursor(lex_keeper, i);
}


void
sp_rcontext::pop_cursors(uint count)
{
  while (count--)
  {
    delete m_cstack[--m_ccount];
  }
}


int
sp_rcontext::set_variable(THD *thd, uint var_idx, Item *value)
{
  return set_variable(thd, m_var_table->field[var_idx], value);
}


int
sp_rcontext::set_variable(THD *thd, Field *field, Item *value)
{
  if (!value)
  {
    field->set_null();
    return 0;
  }

  return sp_eval_expr(thd, field, value);
}


Item *
sp_rcontext::get_item(uint var_idx)
{
  return m_var_items[var_idx];
}


Item **
sp_rcontext::get_item_addr(uint var_idx)
{
  return m_var_items + var_idx;
}


/*
 *
 *  sp_cursor
 *
 */

sp_cursor::sp_cursor(sp_lex_keeper *lex_keeper, sp_instr_cpush *i)
  :m_lex_keeper(lex_keeper),
   server_side_cursor(NULL),
   m_i(i)
{
  /*
    currsor can't be stored in QC, so we should prevent opening QC for
    try to write results which are absent.
  */
  lex_keeper->disable_query_cache();
}


/*
  Open an SP cursor

  SYNOPSIS
    open()
    THD		         Thread handler


  RETURN
   0 in case of success, -1 otherwise
*/

int
sp_cursor::open(THD *thd)
{
  if (server_side_cursor)
  {
    my_message(ER_SP_CURSOR_ALREADY_OPEN, ER(ER_SP_CURSOR_ALREADY_OPEN),
               MYF(0));
    return -1;
  }
  if (mysql_open_cursor(thd, (uint) ALWAYS_MATERIALIZED_CURSOR, &result,
                        &server_side_cursor))
    return -1;
  return 0;
}


int
sp_cursor::close(THD *thd)
{
  if (! server_side_cursor)
  {
    my_message(ER_SP_CURSOR_NOT_OPEN, ER(ER_SP_CURSOR_NOT_OPEN), MYF(0));
    return -1;
  }
  destroy();
  return 0;
}


void
sp_cursor::destroy()
{
  delete server_side_cursor;
  server_side_cursor= 0;
}


int
sp_cursor::fetch(THD *thd, List<struct sp_pvar> *vars)
{
  if (! server_side_cursor)
  {
    my_message(ER_SP_CURSOR_NOT_OPEN, ER(ER_SP_CURSOR_NOT_OPEN), MYF(0));
    return -1;
  }
  if (vars->elements != result.get_field_count())
  {
    my_message(ER_SP_WRONG_NO_OF_FETCH_ARGS,
               ER(ER_SP_WRONG_NO_OF_FETCH_ARGS), MYF(0));
    return -1;
  }

  result.set_spvar_list(vars);

  /* Attempt to fetch one row */
  if (server_side_cursor->is_open())
    server_side_cursor->fetch(1);

  /*
    If the cursor was pointing after the last row, the fetch will
    close it instead of sending any rows.
  */
  if (! server_side_cursor->is_open())
  {
    my_message(ER_SP_FETCH_NO_DATA, ER(ER_SP_FETCH_NO_DATA), MYF(0));
    return -1;
  }

  return 0;
}


/*
  Create an instance of appropriate Item_cache class depending on the
  specified type in the callers arena.

  SYNOPSIS
    thd           thread handler
    result_type   type of the expression

  RETURN
    Pointer to valid object     on success
    NULL                        on error

  NOTE
    We should create cache items in the callers arena, as they are used
    between in several instructions.
*/

Item_cache *
sp_rcontext::create_case_expr_holder(THD *thd, Item_result result_type)
{
  Item_cache *holder;
  Query_arena current_arena;

  thd->set_n_backup_active_arena(thd->spcont->callers_arena, &current_arena);

  holder= Item_cache::get_cache(result_type);

  thd->restore_active_arena(thd->spcont->callers_arena, &current_arena);

  return holder;
}


/*
  Set CASE expression to the specified value.

  SYNOPSIS
    thd             thread handler
    case_expr_id    identifier of the CASE expression
    case_expr_item  a value of the CASE expression

  RETURN
    FALSE   on success
    TRUE    on error

  NOTE
    The idea is to reuse Item_cache for the expression of the one CASE
    statement. This optimization takes place when there is CASE statement
    inside of a loop. So, in other words, we will use the same object on each
    iteration instead of creating a new one for each iteration.

  TODO
    Hypothetically, a type of CASE expression can be different for each
    iteration. For instance, this can happen if the expression contains a
    session variable (something like @@VAR) and its type is changed from one
    iteration to another.
    
    In order to cope with this problem, we check type each time, when we use
    already created object. If the type does not match, we re-create Item.
    This also can (should?) be optimized.
*/

int
sp_rcontext::set_case_expr(THD *thd, int case_expr_id, Item *case_expr_item)
{
  if (!(case_expr_item= sp_prepare_func_item(thd, &case_expr_item)))
    return TRUE;

  if (!m_case_expr_holders[case_expr_id] ||
      m_case_expr_holders[case_expr_id]->result_type() !=
        case_expr_item->result_type())
  {
    m_case_expr_holders[case_expr_id]=
      create_case_expr_holder(thd, case_expr_item->result_type());
  }

  m_case_expr_holders[case_expr_id]->store(case_expr_item);

  return FALSE;
}


Item *
sp_rcontext::get_case_expr(int case_expr_id)
{
  return m_case_expr_holders[case_expr_id];
}


Item **
sp_rcontext::get_case_expr_addr(int case_expr_id)
{
  return (Item**) m_case_expr_holders + case_expr_id;
}


/***************************************************************************
 Select_fetch_into_spvars
****************************************************************************/

int Select_fetch_into_spvars::prepare(List<Item> &fields, SELECT_LEX_UNIT *u)
{
  /*
    Cache the number of columns in the result set in order to easily
    return an error if column count does not match value count.
  */
  field_count= fields.elements;
  return select_result_interceptor::prepare(fields, u);
}


bool Select_fetch_into_spvars::send_data(List<Item> &items)
{
  List_iterator_fast<struct sp_pvar> pv_iter(*spvar_list);
  List_iterator_fast<Item> item_iter(items);
  sp_pvar_t *pv;
  Item *item;

  /* Must be ensured by the caller */
  DBUG_ASSERT(spvar_list->elements == items.elements);

  /*
    Assign the row fetched from a server side cursor to stored
    procedure variables.
  */
  for (; pv= pv_iter++, item= item_iter++; )
  {
    if (thd->spcont->set_variable(thd, pv->offset, item))
      return TRUE;
  }
  return FALSE;
}
