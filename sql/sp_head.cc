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

#ifdef __GNUC__
#pragma implementation
#endif

#include "mysql_priv.h"
#include "sql_acl.h"
#include "sp_head.h"
#include "sp.h"
#include "sp_pcontext.h"
#include "sp_rcontext.h"

Item_result
sp_map_result_type(enum enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
    return INT_RESULT;
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    return REAL_RESULT;
  default:
    return STRING_RESULT;
  }
}

/* Evaluate a (presumed) func item. Always returns an item, the parameter
** if nothing else.
*/
Item *
sp_eval_func_item(THD *thd, Item *it, enum enum_field_types type)
{
  DBUG_ENTER("sp_eval_func_item");
  it= it->this_item();
  DBUG_PRINT("info", ("type: %d", type));

  if (!it->fixed && it->fix_fields(thd, 0, &it))
  {
    DBUG_PRINT("info", ("fix_fields() failed"));
    DBUG_RETURN(it);		// Shouldn't happen?
  }

  /* QQ How do we do this? Is there some better way? */
  if (type == MYSQL_TYPE_NULL)
    it= new Item_null();
  else
  {
    switch (sp_map_result_type(type)) {
    case INT_RESULT:
      {
	longlong i= it->val_int();

	if (it->null_value)
	{
	  DBUG_PRINT("info", ("INT_RESULT: null"));
	  it= new Item_null();
	}
	else
	{
	  DBUG_PRINT("info", ("INT_RESULT: %d", i));
	  it= new Item_int(it->val_int());
	}
	break;
      }
    case REAL_RESULT:
      {
	double d= it->val();

	if (it->null_value)
	{
	  DBUG_PRINT("info", ("REAL_RESULT: null"));
	  it= new Item_null();
	}
	else
	{
	  /* There's some difference between Item::new_item() and the
	   * constructor; the former crashes, the latter works... weird. */
	  uint8 decimals= it->decimals;
	  uint32 max_length= it->max_length;
	  DBUG_PRINT("info", ("REAL_RESULT: %g", d));
	  it= new Item_real(it->val());
	  it->decimals= decimals;
	  it->max_length= max_length;
	}
	break;
      }
    default:
      {
	char buffer[MAX_FIELD_WIDTH];
	String tmp(buffer, sizeof(buffer), it->collation.collation);
	String *s= it->val_str(&tmp);

	if (it->null_value)
	{
	  DBUG_PRINT("info", ("default result: null"));
	  it= new Item_null();
	}
	else
	{
	  DBUG_PRINT("info",("default result: %*s",s->length(),s->c_ptr_quick()));
	  it= new Item_string(thd->strmake(s->c_ptr_quick(), s->length()),
			      s->length(), it->collation.collation);
	}
	break;
      }
    }
  }

  DBUG_RETURN(it);
}

void *
sp_head::operator new(size_t size)
{
  DBUG_ENTER("sp_head::operator new");
  MEM_ROOT own_root;
  sp_head *sp;

  bzero((char *)&own_root, sizeof(own_root));
  init_alloc_root(&own_root, MEM_ROOT_BLOCK_SIZE, MEM_ROOT_PREALLOC);
  sp= (sp_head *)alloc_root(&own_root, size);
  sp->m_mem_root= own_root;
  
  DBUG_RETURN(sp);
}

void 
sp_head::operator delete(void *ptr, size_t size)
{
  DBUG_ENTER("sp_head::operator delete");
  MEM_ROOT own_root;
  sp_head *sp= (sp_head *)ptr;

  DBUG_PRINT("info", ("root: %lx", &sp->m_mem_root));
  memcpy(&own_root, (const void *)&sp->m_mem_root, sizeof(MEM_ROOT));
  free_root(&own_root, MYF(0));

  DBUG_VOID_RETURN;
}

sp_head::sp_head()
  : Sql_alloc(), m_has_return(FALSE), m_simple_case(FALSE),
    m_multi_results(FALSE), m_free_list(NULL)
{
  DBUG_ENTER("sp_head::sp_head");

  m_backpatch.empty();
  m_lex.empty();
  DBUG_VOID_RETURN;
}

void
sp_head::init(LEX *lex)
{
  DBUG_ENTER("sp_head::init");

  lex->spcont= m_pcont= new sp_pcontext();
  my_init_dynamic_array(&m_instr, sizeof(sp_instr *), 16, 8);
  m_param_begin= m_param_end= m_returns_begin= m_returns_end= m_body_begin= 0;
  m_name.str= m_params.str= m_retstr.str= m_body.str= m_defstr.str= 0;
  m_name.length= m_params.length= m_retstr.length= m_body.length=
    m_defstr.length= 0;
  DBUG_VOID_RETURN;
}

void
sp_head::init_strings(THD *thd, LEX *lex, LEX_STRING *name)
{
  DBUG_ENTER("sp_head::init_strings");
  /* During parsing, we must use thd->mem_root */
  MEM_ROOT *root= &thd->mem_root;

  DBUG_PRINT("info", ("name: %*s", name->length, name->str));
  m_name.length= name->length;
  m_name.str= strmake_root(root, name->str, name->length);
  m_params.length= m_param_end- m_param_begin;
  m_params.str= strmake_root(root,
			     (char *)m_param_begin, m_params.length);
  if (m_returns_begin && m_returns_end)
  {
    /* QQ KLUDGE: We can't seem to cut out just the type in the parser
       (without the RETURNS), so we'll have to do it here. :-( */
    char *p= (char *)m_returns_begin+strspn((char *)m_returns_begin,"\t\n\r ");
    p+= strcspn(p, "\t\n\r ");
    p+= strspn(p, "\t\n\r ");
    if (p < (char *)m_returns_end)
      m_returns_begin= (uchar *)p;
    /* While we're at it, trim the end too. */
    p= (char *)m_returns_end-1;
    while (p > (char *)m_returns_begin &&
	   (*p == '\t' || *p == '\n' || *p == '\r' || *p == ' '))
      p-= 1;
    m_returns_end= (uchar *)p+1;
    m_retstr.length=  m_returns_end - m_returns_begin;
    m_retstr.str= strmake_root(root,
			       (char *)m_returns_begin, m_retstr.length);
  }
  m_body.length= lex->end_of_query - m_body_begin;
  m_body.str= strmake_root(root, (char *)m_body_begin, m_body.length);
  m_defstr.length= lex->end_of_query - lex->buf;
  m_defstr.str= strmake_root(root, (char *)lex->buf, m_defstr.length);
  DBUG_VOID_RETURN;
}

int
sp_head::create(THD *thd)
{
  DBUG_ENTER("sp_head::create");
  int ret;

  DBUG_PRINT("info", ("type: %d name: %s params: %s body: %s",
		      m_type, m_name.str, m_params.str, m_body.str));
  if (m_type == TYPE_ENUM_FUNCTION)
    ret= sp_create_function(thd, this);
  else
    ret= sp_create_procedure(thd, this);

  DBUG_RETURN(ret);
}

sp_head::~sp_head()
{
  destroy();
  if (m_thd)
    restore_thd_mem_root(m_thd);
}

void
sp_head::destroy()
{
  DBUG_ENTER("sp_head::destroy");
  DBUG_PRINT("info", ("name: %s", m_name.str));
  sp_instr *i;
  LEX *lex;

  for (uint ip = 0 ; (i = get_instr(ip)) ; ip++)
    delete i;
  delete_dynamic(&m_instr);
  m_pcont->destroy();
  free_items(m_free_list);
  while ((lex= (LEX *)m_lex.pop()))
  {
    if (lex != &m_thd->main_lex) // We got interrupted and have lex'es left
      delete lex;
  }
  DBUG_VOID_RETURN;
}

int
sp_head::execute(THD *thd)
{
  DBUG_ENTER("sp_head::execute");
  char olddbname[128];
  char *olddbptr= thd->db;
  sp_rcontext *ctx= thd->spcont;
  int ret= 0;
  uint ip= 0;

#ifndef EMBEDDED_LIBRARY
  if (check_stack_overrun(thd, olddbptr))
  {
    DBUG_RETURN(-1);
  }
#endif
  if (olddbptr)
  {
    uint i= 0;
    char *p= olddbptr;

    /* Fast inline strncpy without padding... */
    while (*p && i < sizeof(olddbname))
      olddbname[i++]= *p++;
    if (i == sizeof(olddbname))
      i-= 1;			// QQ Error or warning for truncate?
    olddbname[i]= '\0';
  }

  if (ctx)
    ctx->clear_handler();
  thd->query_error= 0;
  do
  {
    sp_instr *i;
    uint hip;			// Handler ip

    i = get_instr(ip);	// Returns NULL when we're done.
    if (i == NULL)
      break;
    DBUG_PRINT("execute", ("Instruction %u", ip));
    ret= i->execute(thd, &ip);
    // Check if an exception has occurred and a handler has been found
    // Note: We havo to check even if ret==0, since warnings (and some
    //       errors don't return a non-zero value.
    if (!thd->killed && ctx)
    {
      uint hf;

      switch (ctx->found_handler(&hip, &hf))
      {
      case SP_HANDLER_NONE:
	break;
      case SP_HANDLER_CONTINUE:
	ctx->save_variables(hf);
	ctx->push_hstack(ip);
	// Fall through
      default:
	ip= hip;
	ret= 0;
	ctx->clear_handler();
	continue;
      }
    }
  } while (ret == 0 && !thd->killed && !thd->query_error);

  DBUG_PRINT("info", ("ret=%d killed=%d query_error=%d",
		      ret, thd->killed, thd->query_error));
  if (thd->killed || thd->query_error)
    ret= -1;
  /* If the DB has changed, the pointer has changed too, but the
     original thd->db will then have been freed */
  if (olddbptr && olddbptr != thd->db)
  {
    /* QQ Maybe we should issue some special error message or warning here,
       if this fails?? */
    if (! thd->killed)
      ret= mysql_change_db(thd, olddbname);
  }
  DBUG_RETURN(ret);
}


int
sp_head::execute_function(THD *thd, Item **argp, uint argcount, Item **resp)
{
  DBUG_ENTER("sp_head::execute_function");
  DBUG_PRINT("info", ("function %s", m_name.str));
  uint csize = m_pcont->max_framesize();
  uint params = m_pcont->params();
  uint hmax = m_pcont->handlers();
  uint cmax = m_pcont->cursors();
  sp_rcontext *octx = thd->spcont;
  sp_rcontext *nctx = NULL;
  uint i;
  int ret;

  if (argcount != params)
  {
    // Need to use my_printf_error here, or it will not terminate the
    // invoking query properly.
    my_printf_error(ER_SP_WRONG_NO_OF_ARGS, ER(ER_SP_WRONG_NO_OF_ARGS), MYF(0),
		    "FUNCTION", m_name.str, params, argcount);
    DBUG_RETURN(-1);
  }

  // QQ Should have some error checking here? (types, etc...)
  nctx= new sp_rcontext(csize, hmax, cmax);
  for (i= 0 ; i < params && i < argcount ; i++)
  {
    sp_pvar_t *pvar = m_pcont->find_pvar(i);

    nctx->push_item(sp_eval_func_item(thd, *argp++, pvar->type));
  }
  // Close tables opened for subselect in argument list
  close_thread_tables(thd);

  // The rest of the frame are local variables which are all IN.
  // Default all variables to null (those with default clauses will
  // be set by an set instruction).
  {
    Item_null *nit= NULL;	// Re-use this, and only create if needed
    for (; i < csize ; i++)
    {
      if (! nit)
	nit= new Item_null();
      nctx->push_item(nit);
    }
  }
  thd->spcont= nctx;

  ret= execute(thd);
  if (ret == 0)
  {
    Item *it= nctx->get_result();

    if (it)
      *resp= it;
    else
    {
      my_printf_error(ER_SP_NORETURNEND, ER(ER_SP_NORETURNEND), MYF(0),
		      m_name.str);
      ret= -1;
    }
  }

  nctx->pop_all_cursors();	// To avoid memory leaks after an error
  thd->spcont= octx;
  DBUG_RETURN(ret);
}

int
sp_head::execute_procedure(THD *thd, List<Item> *args)
{
  DBUG_ENTER("sp_head::execute_procedure");
  DBUG_PRINT("info", ("procedure %s", m_name.str));
  int ret;
  uint csize = m_pcont->max_framesize();
  uint params = m_pcont->params();
  uint hmax = m_pcont->handlers();
  uint cmax = m_pcont->cursors();
  sp_rcontext *octx = thd->spcont;
  sp_rcontext *nctx = NULL;
  my_bool tmp_octx = FALSE;	// True if we have allocated a temporary octx

  if (args->elements != params)
  {
    net_printf(thd, ER_SP_WRONG_NO_OF_ARGS, "PROCEDURE", m_name.str,
	       params, args->elements);
    DBUG_RETURN(-1);
  }

  if (csize > 0 || hmax > 0 || cmax > 0)
  {
    Item_null *nit= NULL;	// Re-use this, and only create if needed
    uint i;
    List_iterator_fast<Item> li(*args);
    Item *it;

    nctx= new sp_rcontext(csize, hmax, cmax);
    if (! octx)
    {				// Create a temporary old context
      octx= new sp_rcontext(csize, hmax, cmax);
      tmp_octx= TRUE;
    }
    // QQ: Should do type checking?
    for (i = 0 ; (it= li++) && i < params ; i++)
    {
      sp_pvar_t *pvar = m_pcont->find_pvar(i);

      if (! pvar)
	nctx->set_oindex(i, -1); // Shouldn't happen
      else
      {
	if (pvar->mode == sp_param_out)
	{
	  if (! nit)
	    nit= new Item_null();
	  nctx->push_item(nit); // OUT
	}
	else
	  nctx->push_item(sp_eval_func_item(thd, it,pvar->type)); // IN or INOUT
	// Note: If it's OUT or INOUT, it must be a variable.
	// QQ: We can check for global variables here, or should we do it
	//     while parsing?
	if (pvar->mode == sp_param_in)
	  nctx->set_oindex(i, -1); // IN
	else			// OUT or INOUT
	  nctx->set_oindex(i, static_cast<Item_splocal *>(it)->get_offset());
      }
    }
    // Close tables opened for subselect in argument list
    close_thread_tables(thd);

    // The rest of the frame are local variables which are all IN.
    // Default all variables to null (those with default clauses will
    // be set by an set instruction).
    for (; i < csize ; i++)
    {
      if (! nit)
	nit= new Item_null();
      nctx->push_item(nit);
    }
    thd->spcont= nctx;
  }

  ret= execute(thd);

  // Don't copy back OUT values if we got an error
  if (ret == 0 && csize > 0)
  {
    List_iterator_fast<Item> li(*args);
    Item *it;

    // Copy back all OUT or INOUT values to the previous frame, or
    // set global user variables
    for (uint i = 0 ; (it= li++) && i < params ; i++)
    {
      int oi = nctx->get_oindex(i);

      if (oi >= 0)
      {
	if (! tmp_octx)
	  octx->set_item(nctx->get_oindex(i), nctx->get_item(i));
	else
	{
	  // QQ Currently we just silently ignore non-user-variable arguments.
	  //    We should check this during parsing, when setting up the call
	  //    above
	  if (it->type() == Item::FUNC_ITEM)
	  {
	    Item_func *fi= static_cast<Item_func*>(it);

	    if (fi->functype() == Item_func::GUSERVAR_FUNC)
	    {			// A global user variable
	      Item *item= nctx->get_item(i);
	      Item_func_set_user_var *suv;
	      Item_func_get_user_var *guv=
		static_cast<Item_func_get_user_var*>(fi);

	      suv= new Item_func_set_user_var(guv->get_name(), item);
	      suv->fix_fields(thd, NULL, &item);
	      suv->fix_length_and_dec();
	      suv->check();
	      suv->update();
	    }
	  }
	}
      }
    }
  }

  if (tmp_octx)
    octx= NULL;
  if (nctx)
    nctx->pop_all_cursors();	// To avoid memory leaks after an error
  thd->spcont= octx;

  DBUG_RETURN(ret);
}


// Reset lex during parsing, before we parse a sub statement.
void
sp_head::reset_lex(THD *thd)
{
  DBUG_ENTER("sp_head::reset_lex");
  LEX *sublex;
  LEX *oldlex= thd->lex;

  (void)m_lex.push_front(oldlex);
  thd->lex= sublex= new st_lex;
  sublex->yylineno= oldlex->yylineno;
  /* Reset most stuff. The length arguments doesn't matter here. */
  lex_start(thd, oldlex->buf, oldlex->end_of_query - oldlex->ptr);
  /* We must reset ptr and end_of_query again */
  sublex->ptr= oldlex->ptr;
  sublex->end_of_query= oldlex->end_of_query;
  sublex->tok_start= oldlex->tok_start;
  /* And keep the SP stuff too */
  sublex->sphead= oldlex->sphead;
  sublex->spcont= oldlex->spcont;
  mysql_init_query(thd, true);	// Only init lex
  sublex->sp_lex_in_use= FALSE;
  DBUG_VOID_RETURN;
}

// Restore lex during parsing, after we have parsed a sub statement.
void
sp_head::restore_lex(THD *thd)
{
  DBUG_ENTER("sp_head::restore_lex");
  LEX *sublex= thd->lex;
  LEX *oldlex= (LEX *)m_lex.pop();
  SELECT_LEX *sl;

  if (! oldlex)
    return;			// Nothing to restore

  // Update some state in the old one first
  oldlex->ptr= sublex->ptr;
  oldlex->next_state= sublex->next_state;
  for (sl= sublex->all_selects_list ;
       sl ;
       sl= sl->next_select_in_list())
  {
    // Save WHERE clause pointers to avoid damaging by optimisation
    sl->prep_where= sl->where;
    if (sl->with_wild)
    {
      // Copy item_list. We will restore it before calling the
      // sub-statement, so it's ok to pop them.
      sl->item_list_copy.empty();
      while (Item *it= sl->item_list.pop())
	sl->item_list_copy.push_back(it);
    }
  }

  // Collect some data from the sub statement lex.
  sp_merge_funs(oldlex, sublex);
#ifdef NOT_USED_NOW
  // QQ We're not using this at the moment.
  if (sublex.sql_command == SQLCOM_CALL)
  {
    // It would be slightly faster to keep the list sorted, but we need
    // an "insert before" method to do that.
    char *proc= sublex.udf.name.str;

    List_iterator_fast<char *> li(m_calls);
    char **it;

    while ((it= li++))
      if (my_strcasecmp(system_charset_info, proc, *it) == 0)
	break;
    if (! it)
      m_calls.push_back(&proc);

  }
  // Merge used tables
  // QQ ...or just open tables in thd->open_tables?
  //    This is not entirerly clear at the moment, but for now, we collect
  //    tables here.
  for (sl= sublex.all_selects_list ;
       sl ;
       sl= sl->next_select())
  {
    for (TABLE_LIST *tables= sl->get_table_list() ;
	 tables ;
	 tables= tables->next)
    {
      List_iterator_fast<char *> li(m_tables);
      char **tb;

      while ((tb= li++))
	if (my_strcasecmp(system_charset_info, tables->real_name, *tb) == 0)
	  break;
      if (! tb)
	m_tables.push_back(&tables->real_name);
    }
  }
#endif
  if (! sublex->sp_lex_in_use)
    delete sublex;
  thd->lex= oldlex;
  DBUG_VOID_RETURN;
}

void
sp_head::push_backpatch(sp_instr *i, sp_label_t *lab)
{
  bp_t *bp= (bp_t *)sql_alloc(sizeof(bp_t));

  if (bp)
  {
    bp->lab= lab;
    bp->instr= i;
    (void)m_backpatch.push_front(bp);
  }
}

void
sp_head::backpatch(sp_label_t *lab)
{
  bp_t *bp;
  uint dest= instructions();
  List_iterator_fast<bp_t> li(m_backpatch);

  while ((bp= li++))
    if (bp->lab == lab)
    {
      sp_instr_jump *i= static_cast<sp_instr_jump *>(bp->instr);

      i->set_destination(dest);
    }
}

void
sp_head::set_info(char *definer, uint definerlen,
		  longlong created, longlong modified,
		  st_sp_chistics *chistics)
{
  char *p= strchr(definer, '@');
  uint len;

  if (! p)
    p= definer;		// Weird...
  len= p-definer;
  m_definer_user.str= strmake_root(&m_mem_root, definer, len);
  m_definer_user.length= len;
  len= definerlen-len-1;
  m_definer_host.str= strmake_root(&m_mem_root, p+1, len);
  m_definer_host.length= len;
  m_created= created;
  m_modified= modified;
  m_chistics= (st_sp_chistics *)alloc_root(&m_mem_root, sizeof(st_sp_chistics));
  memcpy(m_chistics, chistics, sizeof(st_sp_chistics));
  if (m_chistics->comment.length == 0)
    m_chistics->comment.str= 0;
  else
    m_chistics->comment.str= strmake_root(&m_mem_root,
					  m_chistics->comment.str,
					  m_chistics->comment.length);
}

int
sp_head::show_create_procedure(THD *thd)
{
  Protocol *protocol= thd->protocol;
  char buff[2048];
  String buffer(buff, sizeof(buff), system_charset_info);
  int res;
  List<Item> field_list;

  DBUG_ENTER("sp_head::show_create_procedure");
  DBUG_PRINT("info", ("procedure %s", m_name.str));

  field_list.push_back(new Item_empty_string("Procedure",NAME_LEN));
  // 1024 is for not to confuse old clients
  field_list.push_back(new Item_empty_string("Create Procedure",
					     max(buffer.length(),1024)));
  if (protocol->send_fields(&field_list, 1))
    DBUG_RETURN(1);
  protocol->prepare_for_resend();
  protocol->store(m_name.str, m_name.length, system_charset_info);
  protocol->store(m_defstr.str, m_defstr.length, system_charset_info);
  res= protocol->write();
  send_eof(thd);
  DBUG_RETURN(res);
}

int
sp_head::show_create_function(THD *thd)
{
  Protocol *protocol= thd->protocol;
  char buff[2048];
  String buffer(buff, sizeof(buff), system_charset_info);
  int res;
  List<Item> field_list;

  DBUG_ENTER("sp_head::show_create_function");
  DBUG_PRINT("info", ("procedure %s", m_name.str));

  field_list.push_back(new Item_empty_string("Function",NAME_LEN));
  field_list.push_back(new Item_empty_string("Create Function",
					     max(buffer.length(),1024)));
  if (protocol->send_fields(&field_list, 1))
    DBUG_RETURN(1);
  protocol->prepare_for_resend();
  protocol->store(m_name.str, m_name.length, system_charset_info);
  protocol->store(m_defstr.str, m_defstr.length, system_charset_info);
  res= protocol->write();
  send_eof(thd);
  DBUG_RETURN(res);
}
// ------------------------------------------------------------------

//
// sp_instr_stmt
//
sp_instr_stmt::~sp_instr_stmt()
{
  if (m_lex)
    delete m_lex;
}

int
sp_instr_stmt::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_stmt::execute");
  DBUG_PRINT("info", ("command: %d", m_lex->sql_command));
  int res= exec_stmt(thd, m_lex);
  *nextp = m_ip+1;
  DBUG_RETURN(res);
}

int
sp_instr_stmt::exec_stmt(THD *thd, LEX *lex)
{
  LEX *olex;			// The other lex
  Item *freelist;
  SELECT_LEX *sl;
  int res;

  olex= thd->lex;		// Save the other lex
  thd->lex= lex;		// Use my own lex
  thd->lex->thd = thd;		// QQ Not reentrant!
  thd->lex->unit.thd= thd;	// QQ Not reentrant
  freelist= thd->free_list;
  thd->free_list= NULL;
  thd->query_id= query_id++;

  // Copy WHERE clause pointers to avoid damaging by optimisation
  // Also clear ref_pointer_arrays.
  for (sl= lex->all_selects_list ;
       sl ;
       sl= sl->next_select_in_list())
  {
    if (lex->sql_command == SQLCOM_CREATE_TABLE ||
	lex->sql_command == SQLCOM_INSERT_SELECT)
    {				// Destroys sl->table_list.first
      sl->table_list_first_copy= sl->table_list.first;
    }
    if (sl->with_wild)
    {
      // Restore item_list
      // Note: We have to do this before executing the sub-statement,
      //       to make sure that the list nodes are in the right
      //       memroot.
      List_iterator_fast<Item> li(sl->item_list_copy);

      sl->item_list.empty();
      while (Item *it= li++)
	sl->item_list.push_back(it);
    }
    sl->ref_pointer_array= 0;
    if (sl->prep_where)
      sl->where= sl->prep_where->copy_andor_structure(thd);
    for (ORDER *order= (ORDER *)sl->order_list.first ;
	 order ;
	 order= order->next)
    {
      order->item_copy= order->item;
    }
    for (ORDER *group= (ORDER *)sl->group_list.first ;
	 group ;
	 group= group->next)
    {
      group->item_copy= group->item;
    }
  }

  res= mysql_execute_command(thd);

  if (thd->lock || thd->open_tables || thd->derived_tables)
  {
    thd->proc_info="closing tables";
    close_thread_tables(thd);			/* Free tables */
  }

  for (sl= lex->all_selects_list ;
       sl ;
       sl= sl->next_select_in_list())
  {
    TABLE_LIST *tabs;

    // We have closed all tables, get rid of pointers to them
    for (tabs=(TABLE_LIST *)sl->table_list.first ;
	 tabs ;
	 tabs= tabs->next)
    {
      tabs->table= NULL;
    }
    if (lex->sql_command == SQLCOM_CREATE_TABLE ||
	lex->sql_command == SQLCOM_INSERT_SELECT)
    {				// Restore sl->table_list.first
      sl->table_list.first= sl->table_list_first_copy;
    }
    for (ORDER *order= (ORDER *)sl->order_list.first ;
	 order ;
	 order= order->next)
    {
      order->item= order->item_copy;
    }
    for (ORDER *group= (ORDER *)sl->group_list.first ;
	 group ;
	 group= group->next)
    {
      group->item= group->item_copy;
    }
  }
  thd->lex= olex;		// Restore the other lex
  thd->free_list= freelist;

  return res;
}

//
// sp_instr_set
//
int
sp_instr_set::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_set::execute");
  DBUG_PRINT("info", ("offset: %u", m_offset));
  thd->spcont->set_item(m_offset, sp_eval_func_item(thd, m_value, m_type));
  *nextp = m_ip+1;
  DBUG_RETURN(0);
}

//
// sp_instr_jump
//
int
sp_instr_jump::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_jump::execute");
  DBUG_PRINT("info", ("destination: %u", m_dest));

  *nextp= m_dest;
  DBUG_RETURN(0);
}

//
// sp_instr_jump_if
//
int
sp_instr_jump_if::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_jump_if::execute");
  DBUG_PRINT("info", ("destination: %u", m_dest));
  Item *it= sp_eval_func_item(thd, m_expr, MYSQL_TYPE_TINY);

  if (it->val_int())
    *nextp = m_dest;
  else
    *nextp = m_ip+1;
  DBUG_RETURN(0);
}

//
// sp_instr_jump_if_not
//
int
sp_instr_jump_if_not::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_jump_if_not::execute");
  DBUG_PRINT("info", ("destination: %u", m_dest));
  Item *it= sp_eval_func_item(thd, m_expr, MYSQL_TYPE_TINY);

  if (! it->val_int())
    *nextp = m_dest;
  else
    *nextp = m_ip+1;
  DBUG_RETURN(0);
}

//
// sp_instr_freturn
//
int
sp_instr_freturn::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_freturn::execute");
  thd->spcont->set_result(sp_eval_func_item(thd, m_value, m_type));
  *nextp= UINT_MAX;
  DBUG_RETURN(0);
}

//
// sp_instr_hpush_jump
//
int
sp_instr_hpush_jump::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_hpush_jump::execute");
  List_iterator_fast<sp_cond_type_t> li(m_cond);
  sp_cond_type_t *p;

  while ((p= li++))
    thd->spcont->push_handler(p, m_handler, m_type, m_frame);

  *nextp= m_dest;
  DBUG_RETURN(0);
}

//
// sp_instr_hpop
//
int
sp_instr_hpop::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_hpop::execute");
  thd->spcont->pop_handlers(m_count);
  *nextp= m_ip+1;
  DBUG_RETURN(0);
}

//
// sp_instr_hreturn
//
int
sp_instr_hreturn::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_hreturn::execute");
  thd->spcont->restore_variables(m_frame);
  *nextp= thd->spcont->pop_hstack();
  DBUG_RETURN(0);
}

//
// sp_instr_cpush
//
int
sp_instr_cpush::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cpush::execute");
  thd->spcont->push_cursor(m_lex);
  *nextp= m_ip+1;
  DBUG_RETURN(0);
}

sp_instr_cpush::~sp_instr_cpush()
{
  if (m_lex)
    delete m_lex;
}

//
// sp_instr_cpop
//
int
sp_instr_cpop::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cpop::execute");
  thd->spcont->pop_cursors(m_count);
  *nextp= m_ip+1;
  DBUG_RETURN(0);
}

//
// sp_instr_copen
//
int
sp_instr_copen::execute(THD *thd, uint *nextp)
{
  sp_cursor *c= thd->spcont->get_cursor(m_cursor);
  int res;
  DBUG_ENTER("sp_instr_copen::execute");

  if (! c)
    res= -1;
  else
  {
    LEX *lex= c->pre_open(thd);

    if (! lex)
      res= -1;
    else
      res= exec_stmt(thd, lex);
    c->post_open(thd, (lex ? TRUE : FALSE));
  }

  *nextp= m_ip+1;
  DBUG_RETURN(res);
}

//
// sp_instr_cclose
//
int
sp_instr_cclose::execute(THD *thd, uint *nextp)
{
  sp_cursor *c= thd->spcont->get_cursor(m_cursor);
  int res;
  DBUG_ENTER("sp_instr_cclose::execute");

  if (! c)
    res= -1;
  else
    res= c->close(thd);
  *nextp= m_ip+1;
  DBUG_RETURN(res);
}

//
// sp_instr_cfetch
//
int
sp_instr_cfetch::execute(THD *thd, uint *nextp)
{
  sp_cursor *c= thd->spcont->get_cursor(m_cursor);
  int res;
  DBUG_ENTER("sp_instr_cfetch::execute");

  if (! c)
    res= -1;
  else
    res= c->fetch(thd, &m_varlist);
  *nextp= m_ip+1;
  DBUG_RETURN(res);
}


//
// Security context swapping
//
#ifndef NO_EMBEDDED_ACCESS_CHECKS
void
sp_change_security_context(THD *thd, sp_head *sp, st_sp_security_context *ctxp)
{
  ctxp->changed= (sp->m_chistics->suid != IS_NOT_SUID &&
		   (strcmp(sp->m_definer_user.str, thd->priv_user) ||
		    strcmp(sp->m_definer_host.str, thd->priv_host)));

  if (ctxp->changed)
  {
    ctxp->master_access= thd->master_access;
    ctxp->db_access= thd->db_access;
    ctxp->db= thd->db;
    ctxp->db_length= thd->db_length;
    ctxp->priv_user= thd->priv_user;
    strncpy(ctxp->priv_host, thd->priv_host, sizeof(ctxp->priv_host));
    ctxp->user= thd->user;
    ctxp->host= thd->host;
    ctxp->ip= thd->ip;

    /* Change thise just to do the acl_getroot_no_password */
    thd->user= sp->m_definer_user.str;
    thd->host= thd->ip = sp->m_definer_host.str;

    if (acl_getroot_no_password(thd))
    {			// Failed, run as invoker for now
      ctxp->changed= FALSE;
      thd->master_access= ctxp->master_access;
      thd->db_access= ctxp->db_access;
      thd->db= ctxp->db;
      thd->db_length= ctxp->db_length;
      thd->priv_user= ctxp->priv_user;
      strncpy(thd->priv_host, ctxp->priv_host, sizeof(thd->priv_host));
    }

    /* Restore these immiediately */
    thd->user= ctxp->user;
    thd->host= ctxp->host;
    thd->ip= ctxp->ip;
  }
}

void
sp_restore_security_context(THD *thd, sp_head *sp, st_sp_security_context *ctxp)
{
  if (ctxp->changed)
  {
    ctxp->changed= FALSE;
    thd->master_access= ctxp->master_access;
    thd->db_access= ctxp->db_access;
    thd->db= ctxp->db;
    thd->db_length= ctxp->db_length;
    thd->priv_user= ctxp->priv_user;
    strncpy(thd->priv_host, ctxp->priv_host, sizeof(thd->priv_host));
  }
}

#endif /* NO_EMBEDDED_ACCESS_CHECKS */
