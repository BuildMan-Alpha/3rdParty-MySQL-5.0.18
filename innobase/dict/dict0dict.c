/**********************************************************************
Data dictionary system

(c) 1996 Innobase Oy

Created 1/8/1996 Heikki Tuuri
***********************************************************************/

#include "dict0dict.h"

#ifdef UNIV_NONINL
#include "dict0dict.ic"
#endif

#include "buf0buf.h"
#include "data0type.h"
#include "mach0data.h"
#include "dict0boot.h"
#include "dict0mem.h"
#include "dict0crea.h"
#include "trx0undo.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "pars0pars.h"
#include "pars0sym.h"
#include "que0que.h"
#include "rem0cmp.h"

dict_sys_t*	dict_sys	= NULL;	/* the dictionary system */

rw_lock_t	dict_operation_lock;	/* table create, drop, etc. reserve
					this in X-mode; implicit or backround
					operations purge, rollback, foreign
					key checks reserve this in S-mode; we
					cannot trust that MySQL protects
					implicit or background operations
					a table drop since MySQL does not
					know of them; therefore we need this;
					NOTE: a transaction which reserves
					this must keep book on the mode in
					trx->dict_operation_lock_mode */

#define	DICT_HEAP_SIZE		100	/* initial memory heap size when
					creating a table or index object */
#define DICT_POOL_PER_TABLE_HASH 512	/* buffer pool max size per table
					hash table fixed size in bytes */
#define DICT_POOL_PER_COL_HASH	128	/* buffer pool max size per column
					hash table fixed size in bytes */
#define DICT_POOL_PER_VARYING	4	/* buffer pool max size per data
					dictionary varying size in bytes */

/* Identifies generated InnoDB foreign key names */
static char	dict_ibfk[] = "_ibfk_";

#ifndef UNIV_HOTBACKUP
/**********************************************************************
Compares NUL-terminated UTF-8 strings case insensitively.

NOTE: the prototype of this function is copied from ha_innodb.cc! If you change
this function, you MUST change also the prototype here! */
extern
int
innobase_strcasecmp(
/*================*/
				/* out: 0 if a=b, <0 if a<b, >1 if a>b */
	const char*	a,	/* in: first string to compare */
	const char*	b);	/* in: second string to compare */

/**********************************************************************
Makes all characters in a NUL-terminated UTF-8 string lower case.

NOTE: the prototype of this function is copied from ha_innodb.cc! If you change
this function, you MUST change also the prototype here! */
extern
void
innobase_casedn_str(
/*================*/
	char*	a);	/* in/out: string to put in lower case */
#endif /* !UNIV_HOTBACKUP */

/**************************************************************************
Adds a column to the data dictionary hash table. */
static
void
dict_col_add_to_cache(
/*==================*/
	dict_table_t*	table,	/* in: table */
	dict_col_t*	col);	/* in: column */
/**************************************************************************
Repositions a column in the data dictionary hash table when the table name
changes. */
static
void
dict_col_reposition_in_cache(
/*=========================*/
	dict_table_t*	table,		/* in: table */
	dict_col_t*	col,		/* in: column */
	const char*	new_name);	/* in: new table name */
/**************************************************************************
Removes a column from the data dictionary hash table. */
static
void
dict_col_remove_from_cache(
/*=======================*/
	dict_table_t*	table,	/* in: table */
	dict_col_t*	col);	/* in: column */
/**************************************************************************
Removes an index from the dictionary cache. */
static
void
dict_index_remove_from_cache(
/*=========================*/
	dict_table_t*	table,	/* in: table */
	dict_index_t*	index);	/* in, own: index */
/***********************************************************************
Copies fields contained in index2 to index1. */
static
void
dict_index_copy(
/*============*/
	dict_index_t*	index1,	/* in: index to copy to */
	dict_index_t*	index2,	/* in: index to copy from */
	ulint		start,	/* in: first position to copy */
	ulint		end);	/* in: last position to copy */
/***********************************************************************
Tries to find column names for the index in the column hash table and
sets the col field of the index. */
static
ibool
dict_index_find_cols(
/*=================*/
				/* out: TRUE if success */
	dict_table_t*	table,	/* in: table */
	dict_index_t*	index);	/* in: index */	
/***********************************************************************
Builds the internal dictionary cache representation for a clustered
index, containing also system fields not defined by the user. */
static
dict_index_t*
dict_index_build_internal_clust(
/*============================*/
				/* out, own: the internal representation
				of the clustered index */
	dict_table_t*	table,	/* in: table */
	dict_index_t*	index);	/* in: user representation of a clustered
				index */	
/***********************************************************************
Builds the internal dictionary cache representation for a non-clustered
index, containing also system fields not defined by the user. */
static
dict_index_t*
dict_index_build_internal_non_clust(
/*================================*/
				/* out, own: the internal representation
				of the non-clustered index */
	dict_table_t*	table,	/* in: table */
	dict_index_t*	index);	/* in: user representation of a non-clustered
				index */	
/**************************************************************************
Removes a foreign constraint struct from the dictionary cache. */
static
void
dict_foreign_remove_from_cache(
/*===========================*/
	dict_foreign_t*	foreign);	/* in, own: foreign constraint */
/**************************************************************************
Prints a column data. */
static
void
dict_col_print_low(
/*===============*/
	dict_col_t*	col);	/* in: column */
/**************************************************************************
Prints an index data. */
static
void
dict_index_print_low(
/*=================*/
	dict_index_t*	index);	/* in: index */
/**************************************************************************
Prints a field data. */
static
void
dict_field_print_low(
/*=================*/
	dict_field_t*	field);	/* in: field */
/*************************************************************************
Frees a foreign key struct. */
static
void
dict_foreign_free(
/*==============*/
	dict_foreign_t*	foreign);	/* in, own: foreign key struct */

/* Stream for storing detailed information about the latest foreign key
and unique key errors */
FILE*	dict_foreign_err_file		= NULL;
mutex_t	dict_foreign_err_mutex; 	/* mutex protecting the foreign
					and unique error buffers */
	
/**********************************************************************
Makes all characters in a NUL-terminated UTF-8 string lower case. */

void
dict_casedn_str(
/*============*/
	char*	a)	/* in/out: string to put in lower case */
{
	innobase_casedn_str(a);
}

/************************************************************************
Checks if the database name in two table names is the same. */

ibool
dict_tables_have_same_db(
/*=====================*/
				/* out: TRUE if same db name */
	const char*	name1,	/* in: table name in the form
				dbname '/' tablename */
	const char*	name2)	/* in: table name in the form
				dbname '/' tablename */
{
	for (; *name1 == *name2; name1++, name2++) {
		if (*name1 == '/') {
			return(TRUE);
		}
		ut_a(*name1); /* the names must contain '/' */
	}
	return(FALSE);
}

/************************************************************************
Return the end of table name where we have removed dbname and '/'. */
static
const char*
dict_remove_db_name(
/*================*/
				/* out: table name */
	const char*	name)	/* in: table name in the form
				dbname '/' tablename */
{
	const char*	s;
	s = strchr(name, '/');
	ut_a(s);
	if (s) s++;
	return(s);
}

/************************************************************************
Get the database name length in a table name. */

ulint
dict_get_db_name_len(
/*=================*/
				/* out: database name length */
	const char*	name)	/* in: table name in the form
				dbname '/' tablename */
{
	const char*	s;
	s = strchr(name, '/');
	ut_a(s);
	return(s - name);
}
	
/************************************************************************
Reserves the dictionary system mutex for MySQL. */

void
dict_mutex_enter_for_mysql(void)
/*============================*/
{
	mutex_enter(&(dict_sys->mutex));
}
	
/************************************************************************
Releases the dictionary system mutex for MySQL. */

void
dict_mutex_exit_for_mysql(void)
/*===========================*/
{
	mutex_exit(&(dict_sys->mutex));
}
	
/************************************************************************
Decrements the count of open MySQL handles to a table. */

void
dict_table_decrement_handle_count(
/*==============================*/
	dict_table_t*	table)	/* in: table */
{
	mutex_enter(&(dict_sys->mutex));

	ut_a(table->n_mysql_handles_opened > 0);

	table->n_mysql_handles_opened--;
	
	mutex_exit(&(dict_sys->mutex));
}

/************************************************************************
Gets the nth column of a table. */

dict_col_t*
dict_table_get_nth_col_noninline(
/*=============================*/
				/* out: pointer to column object */
	dict_table_t*	table,	/* in: table */
	ulint		pos)	/* in: position of column */
{
	return(dict_table_get_nth_col(table, pos));
}

/************************************************************************
Gets the first index on the table (the clustered index). */

dict_index_t*
dict_table_get_first_index_noninline(
/*=================================*/
				/* out: index, NULL if none exists */
	dict_table_t*	table)	/* in: table */
{
	return(dict_table_get_first_index(table));
}

/************************************************************************
Gets the next index on the table. */

dict_index_t*
dict_table_get_next_index_noninline(
/*================================*/
				/* out: index, NULL if none left */
	dict_index_t*	index)	/* in: index */
{
	return(dict_table_get_next_index(index));
}

/**************************************************************************
Returns an index object. */

dict_index_t*
dict_table_get_index_noninline(
/*===========================*/
				/* out: index, NULL if does not exist */
	dict_table_t*	table,	/* in: table */
	const char*	name)	/* in: index name */
{
	return(dict_table_get_index(table, name));
}
	
/************************************************************************
Initializes the autoinc counter. It is not an error to initialize an already
initialized counter. */

void
dict_table_autoinc_initialize(
/*==========================*/
	dict_table_t*	table,	/* in: table */
	ib_longlong	value)	/* in: next value to assign to a row */
{
	mutex_enter(&(table->autoinc_mutex));

	table->autoinc_inited = TRUE;
	table->autoinc = value;

	mutex_exit(&(table->autoinc_mutex));
}

/************************************************************************
Gets the next autoinc value (== autoinc counter value), 0 if not yet
initialized. If initialized, increments the counter by 1. */

ib_longlong
dict_table_autoinc_get(
/*===================*/
				/* out: value for a new row, or 0 */
	dict_table_t*	table)	/* in: table */
{
	ib_longlong	value;

	mutex_enter(&(table->autoinc_mutex));

	if (!table->autoinc_inited) {

		value = 0;
	} else {
		value = table->autoinc;
		table->autoinc = table->autoinc + 1;
	}
	
	mutex_exit(&(table->autoinc_mutex));

	return(value);
}

/************************************************************************
Decrements the autoinc counter value by 1. */

void
dict_table_autoinc_decrement(
/*=========================*/
	dict_table_t*	table)	/* in: table */
{
	mutex_enter(&(table->autoinc_mutex));

	table->autoinc = table->autoinc - 1;
	
	mutex_exit(&(table->autoinc_mutex));
}

/************************************************************************
Reads the next autoinc value (== autoinc counter value), 0 if not yet
initialized. */

ib_longlong
dict_table_autoinc_read(
/*====================*/
				/* out: value for a new row, or 0 */
	dict_table_t*	table)	/* in: table */
{
	ib_longlong	value;

	mutex_enter(&(table->autoinc_mutex));

	if (!table->autoinc_inited) {

		value = 0;
	} else {
		value = table->autoinc;
	}
	
	mutex_exit(&(table->autoinc_mutex));

	return(value);
}

/************************************************************************
Peeks the autoinc counter value, 0 if not yet initialized. Does not
increment the counter. The read not protected by any mutex! */

ib_longlong
dict_table_autoinc_peek(
/*====================*/
				/* out: value of the counter */
	dict_table_t*	table)	/* in: table */
{
	ib_longlong	value;

	if (!table->autoinc_inited) {

		value = 0;
	} else {
		value = table->autoinc;
	}

	return(value);
}

/************************************************************************
Updates the autoinc counter if the value supplied is equal or bigger than the
current value. If not inited, does nothing. */

void
dict_table_autoinc_update(
/*======================*/

	dict_table_t*	table,	/* in: table */
	ib_longlong	value)	/* in: value which was assigned to a row */
{
	mutex_enter(&(table->autoinc_mutex));

	if (table->autoinc_inited) {
		if (value >= table->autoinc) {
			table->autoinc = value + 1;
		}
	}	

	mutex_exit(&(table->autoinc_mutex));
}

/************************************************************************
Looks for column n in an index. */

ulint
dict_index_get_nth_col_pos(
/*=======================*/
				/* out: position in internal representation
				of the index; if not contained, returns
				ULINT_UNDEFINED */
	dict_index_t*	index,	/* in: index */
	ulint		n)	/* in: column number */
{
	dict_field_t*	field;
	dict_col_t*	col;
	ulint		pos;
	ulint		n_fields;
	
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	col = dict_table_get_nth_col(index->table, n);

	if (index->type & DICT_CLUSTERED) {

		return(col->clust_pos);
	}

	n_fields = dict_index_get_n_fields(index);
	
	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (col == field->col && field->prefix_len == 0) {

			return(pos);
		}
	}

	return(ULINT_UNDEFINED);
}

/************************************************************************
Returns TRUE if the index contains a column or a prefix of that column. */

ibool
dict_index_contains_col_or_prefix(
/*==============================*/
				/* out: TRUE if contains the column or its
				prefix */
	dict_index_t*	index,	/* in: index */
	ulint		n)	/* in: column number */
{
	dict_field_t*	field;
	dict_col_t*	col;
	ulint		pos;
	ulint		n_fields;
	
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	if (index->type & DICT_CLUSTERED) {

		return(TRUE);
	}

	col = dict_table_get_nth_col(index->table, n);

	n_fields = dict_index_get_n_fields(index);
	
	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (col == field->col) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/************************************************************************
Looks for a matching field in an index. The column has to be the same. The
column in index must be complete, or must contain a prefix longer than the
column in index2. That is, we must be able to construct the prefix in index2
from the prefix in index. */

ulint
dict_index_get_nth_field_pos(
/*=========================*/
				/* out: position in internal representation
				of the index; if not contained, returns
				ULINT_UNDEFINED */
	dict_index_t*	index,	/* in: index from which to search */
	dict_index_t*	index2,	/* in: index */
	ulint		n)	/* in: field number in index2 */
{
	dict_field_t*	field;
	dict_field_t*	field2;
	ulint		n_fields;
	ulint		pos;
	
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	field2 = dict_index_get_nth_field(index2, n);

	n_fields = dict_index_get_n_fields(index);
	
	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (field->col == field2->col
		    && (field->prefix_len == 0
			|| (field->prefix_len >= field2->prefix_len
			    && field2->prefix_len != 0))) {

			return(pos);
		}
	}

	return(ULINT_UNDEFINED);
}

/**************************************************************************
Returns a table object, based on table id, and memoryfixes it. */

dict_table_t*
dict_table_get_on_id(
/*=================*/
				/* out: table, NULL if does not exist */
	dulint	table_id,	/* in: table id */
	trx_t*	trx)		/* in: transaction handle */
{
	dict_table_t*	table;
	
	if (ut_dulint_cmp(table_id, DICT_FIELDS_ID) <= 0
	   || trx->dict_operation_lock_mode == RW_X_LATCH) {
		/* It is a system table which will always exist in the table
		cache: we avoid acquiring the dictionary mutex, because
		if we are doing a rollback to handle an error in TABLE
		CREATE, for example, we already have the mutex! */

#ifdef UNIV_SYNC_DEBUG
		ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

		return(dict_table_get_on_id_low(table_id, trx));
	}

	mutex_enter(&(dict_sys->mutex));

	table = dict_table_get_on_id_low(table_id, trx);
	
	mutex_exit(&(dict_sys->mutex));

	return(table);
}

/************************************************************************
Looks for column n position in the clustered index. */

ulint
dict_table_get_nth_col_pos(
/*=======================*/
				/* out: position in internal representation
				of the clustered index */
	dict_table_t*	table,	/* in: table */
	ulint		n)	/* in: column number */
{
	return(dict_index_get_nth_col_pos(dict_table_get_first_index(table),
								n));
}

/************************************************************************
Checks if a column is in the ordering columns of the clustered index of a
table. Column prefixes are treated like whole columns. */

ibool
dict_table_col_in_clustered_key(
/*============================*/
				/* out: TRUE if the column, or its prefix, is
				in the clustered key */
	dict_table_t*	table,	/* in: table */
	ulint		n)	/* in: column number */
{
	dict_index_t*	index;
	dict_field_t*	field;
	dict_col_t*	col;
	ulint		pos;
	ulint		n_fields;
	
	ut_ad(table);

	col = dict_table_get_nth_col(table, n);

	index = dict_table_get_first_index(table);

	n_fields = dict_index_get_n_unique(index);
	
	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (col == field->col) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/**************************************************************************
Inits the data dictionary module. */

void
dict_init(void)
/*===========*/
{
	dict_sys = mem_alloc(sizeof(dict_sys_t));

	mutex_create(&(dict_sys->mutex));
	mutex_set_level(&(dict_sys->mutex), SYNC_DICT);

	dict_sys->table_hash = hash_create(buf_pool_get_max_size() /
					(DICT_POOL_PER_TABLE_HASH *
					UNIV_WORD_SIZE));
	dict_sys->table_id_hash = hash_create(buf_pool_get_max_size() /
					(DICT_POOL_PER_TABLE_HASH *
					UNIV_WORD_SIZE));
	dict_sys->col_hash = hash_create(buf_pool_get_max_size() /
					(DICT_POOL_PER_COL_HASH *
					UNIV_WORD_SIZE));
	dict_sys->size = 0;

	UT_LIST_INIT(dict_sys->table_LRU);

	rw_lock_create(&dict_operation_lock);
	rw_lock_set_level(&dict_operation_lock, SYNC_DICT_OPERATION);

	dict_foreign_err_file = os_file_create_tmpfile();
	ut_a(dict_foreign_err_file);
	mutex_create(&dict_foreign_err_mutex);
	mutex_set_level(&dict_foreign_err_mutex, SYNC_ANY_LATCH);
}

/**************************************************************************
Returns a table object and memoryfixes it. NOTE! This is a high-level
function to be used mainly from outside the 'dict' directory. Inside this
directory dict_table_get_low is usually the appropriate function. */

dict_table_t*
dict_table_get(
/*===========*/
					/* out: table, NULL if
					does not exist */
	const char*	table_name,	/* in: table name */
	trx_t*		trx)		/* in: transaction handle or NULL */
{
	dict_table_t*	table;

	UT_NOT_USED(trx);

	mutex_enter(&(dict_sys->mutex));
	
	table = dict_table_get_low(table_name);

	mutex_exit(&(dict_sys->mutex));

	if (table != NULL) {
	        if (!table->stat_initialized) {
			dict_update_statistics(table);
		}
	}
	
	return(table);
}

/**************************************************************************
Returns a table object and increments MySQL open handle count on the table. */

dict_table_t*
dict_table_get_and_increment_handle_count(
/*======================================*/
					/* out: table, NULL if
					does not exist */
	const char*	table_name,	/* in: table name */
	trx_t*		trx)		/* in: transaction handle or NULL */
{
	dict_table_t*	table;

	UT_NOT_USED(trx);

	mutex_enter(&(dict_sys->mutex));
	
	table = dict_table_get_low(table_name);

	if (table != NULL) {

	        table->n_mysql_handles_opened++;
	}

	mutex_exit(&(dict_sys->mutex));

	if (table != NULL) {
	        if (!table->stat_initialized && !table->ibd_file_missing) {
			dict_update_statistics(table);
		}
	}
	
	return(table);
}

/**************************************************************************
Adds a table object to the dictionary cache. */

void
dict_table_add_to_cache(
/*====================*/
	dict_table_t*	table)	/* in: table */
{
	ulint	fold;
	ulint	id_fold;
	ulint	i;
	
	ut_ad(table);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(table->n_def == table->n_cols - DATA_N_SYS_COLS);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(table->cached == FALSE);
	
	fold = ut_fold_string(table->name);
	id_fold = ut_fold_dulint(table->id);
	
	table->cached = TRUE;
	
	/* NOTE: the system columns MUST be added in the following order
	(so that they can be indexed by the numerical value of DATA_ROW_ID,
	etc.) and as the last columns of the table memory object.
	The clustered index will not always physically contain all
	system columns. */

	dict_mem_table_add_col(table, "DB_ROW_ID", DATA_SYS,
			DATA_ROW_ID | DATA_NOT_NULL, DATA_ROW_ID_LEN, 0);
#if DATA_ROW_ID != 0
#error "DATA_ROW_ID != 0"
#endif
	dict_mem_table_add_col(table, "DB_TRX_ID", DATA_SYS,
			DATA_TRX_ID | DATA_NOT_NULL, DATA_TRX_ID_LEN, 0);
#if DATA_TRX_ID != 1
#error "DATA_TRX_ID != 1"
#endif
	dict_mem_table_add_col(table, "DB_ROLL_PTR", DATA_SYS,
			DATA_ROLL_PTR | DATA_NOT_NULL, DATA_ROLL_PTR_LEN, 0);
#if DATA_ROLL_PTR != 2
#error "DATA_ROLL_PTR != 2"
#endif
	dict_mem_table_add_col(table, "DB_MIX_ID", DATA_SYS,
			DATA_MIX_ID | DATA_NOT_NULL, DATA_MIX_ID_LEN, 0);
#if DATA_MIX_ID != 3
#error "DATA_MIX_ID != 3"
#endif

	/* This check reminds that if a new system column is added to
	the program, it should be dealt with here */ 
#if DATA_N_SYS_COLS != 4
#error "DATA_N_SYS_COLS != 4"
#endif

	/* Look for a table with the same name: error if such exists */
	{
		dict_table_t*	table2;
		HASH_SEARCH(name_hash, dict_sys->table_hash, fold, table2,
				(ut_strcmp(table2->name, table->name) == 0));
		ut_a(table2 == NULL);
	}

	/* Look for a table with the same id: error if such exists */
	{
		dict_table_t*	table2;
		HASH_SEARCH(id_hash, dict_sys->table_id_hash, id_fold, table2,
				(ut_dulint_cmp(table2->id, table->id) == 0));
		ut_a(table2 == NULL);
	}

	if (table->type == DICT_TABLE_CLUSTER_MEMBER) {

		table->mix_id_len = mach_dulint_get_compressed_size(
								table->mix_id);
		mach_dulint_write_compressed(table->mix_id_buf, table->mix_id);
	}

	/* Add the columns to the column hash table */
	for (i = 0; i < table->n_cols; i++) {
		dict_col_add_to_cache(table, dict_table_get_nth_col(table, i));
	}

	/* Add table to hash table of tables */
	HASH_INSERT(dict_table_t, name_hash, dict_sys->table_hash, fold,
								   table);

	/* Add table to hash table of tables based on table id */
	HASH_INSERT(dict_table_t, id_hash, dict_sys->table_id_hash, id_fold,
								   table);
	/* Add table to LRU list of tables */
	UT_LIST_ADD_FIRST(table_LRU, dict_sys->table_LRU, table);

	/* If the dictionary cache grows too big, trim the table LRU list */

	dict_sys->size += mem_heap_get_size(table->heap);
	/* dict_table_LRU_trim(); */
}

/**************************************************************************
Looks for an index with the given id. NOTE that we do not reserve
the dictionary mutex: this function is for emergency purposes like
printing info of a corrupt database page! */

dict_index_t*
dict_index_find_on_id_low(
/*======================*/
			/* out: index or NULL if not found from cache */
	dulint	id)	/* in: index id */
{
	dict_table_t*	table;
	dict_index_t*	index;
	
	table = UT_LIST_GET_FIRST(dict_sys->table_LRU);

	while (table) {
		index = dict_table_get_first_index(table);

		while (index) {
			if (0 == ut_dulint_cmp(id, index->tree->id)) {
				/* Found */

				return(index);
			}

			index = dict_table_get_next_index(index);
		}

		table = UT_LIST_GET_NEXT(table_LRU, table);
	}

	return(NULL);
}

/**************************************************************************
Renames a table object. */

ibool
dict_table_rename_in_cache(
/*=======================*/
					/* out: TRUE if success */
	dict_table_t*	table,		/* in: table */
	const char*	new_name,	/* in: new name */
	ibool		rename_also_foreigns)/* in: in ALTER TABLE we want
					to preserve the original table name
					in constraints which reference it */
{
	dict_foreign_t*	foreign;
	dict_index_t*	index;
	ulint		fold;
	ulint		old_size;
	char*		old_name;
	ibool		success;
	ulint		i;
	
	ut_ad(table);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	old_size = mem_heap_get_size(table->heap);
	
	fold = ut_fold_string(new_name);
	
	/* Look for a table with the same name: error if such exists */
	{
		dict_table_t*	table2;
		HASH_SEARCH(name_hash, dict_sys->table_hash, fold, table2,
				(ut_strcmp(table2->name, new_name) == 0));
		if (table2) {
			fprintf(stderr,
"InnoDB: Error: dictionary cache already contains a table of name %s\n",
	 							     new_name);
			return(FALSE);
		}
	}

	/* If the table is stored in a single-table tablespace, rename the
	.ibd file */

	if (table->space != 0) {
		if (table->dir_path_of_temp_table != NULL) {
			fprintf(stderr,
"InnoDB: Error: trying to rename a table %s (%s) created with CREATE\n"
"InnoDB: TEMPORARY TABLE\n", table->name, table->dir_path_of_temp_table);
			success = FALSE;
		} else {
			success = fil_rename_tablespace(table->name,
						table->space, new_name);
		}

		if (!success) {

			return(FALSE);
		}
	}

	/* Reposition the columns in the column hash table; they are hashed
	according to the pair (table name, column name) */

	for (i = 0; i < table->n_cols; i++) {
		dict_col_reposition_in_cache(table,
				dict_table_get_nth_col(table, i), new_name);
	}

	/* Remove table from the hash tables of tables */
	HASH_DELETE(dict_table_t, name_hash, dict_sys->table_hash,
					ut_fold_string(table->name), table);
	old_name = mem_heap_strdup(table->heap, table->name);
	table->name = mem_heap_strdup(table->heap, new_name);

	/* Add table to hash table of tables */
	HASH_INSERT(dict_table_t, name_hash, dict_sys->table_hash, fold,
								   table);
	dict_sys->size += (mem_heap_get_size(table->heap) - old_size);

	/* Update the table_name field in indexes */
	index = dict_table_get_first_index(table);

	while (index != NULL) {
		index->table_name = table->name;
		
		index = dict_table_get_next_index(index);
	}

	if (!rename_also_foreigns) {
		/* In ALTER TABLE we think of the rename table operation
		in the direction table -> temporary table (#sql...)
		as dropping the table with the old name and creating
		a new with the new name. Thus we kind of drop the
		constraints from the dictionary cache here. The foreign key
		constraints will be inherited to the new table from the
		system tables through a call of dict_load_foreigns. */
	
		/* Remove the foreign constraints from the cache */
		foreign = UT_LIST_GET_LAST(table->foreign_list);

		while (foreign != NULL) {
			dict_foreign_remove_from_cache(foreign);
			foreign = UT_LIST_GET_LAST(table->foreign_list);
		}

		/* Reset table field in referencing constraints */

		foreign = UT_LIST_GET_FIRST(table->referenced_list);

		while (foreign != NULL) {
			foreign->referenced_table = NULL;
			foreign->referenced_index = NULL;
		
			foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
		}

		/* Make the list of referencing constraints empty */

		UT_LIST_INIT(table->referenced_list);
		
		return(TRUE);
	}

	/* Update the table name fields in foreign constraints, and update also
	the constraint id of new format >= 4.0.18 constraints. Note that at
	this point we have already changed table->name to the new name. */

	foreign = UT_LIST_GET_FIRST(table->foreign_list);

	while (foreign != NULL) {
		if (ut_strlen(foreign->foreign_table_name) <
						ut_strlen(table->name)) {
			/* Allocate a longer name buffer;
			TODO: store buf len to save memory */

			foreign->foreign_table_name = mem_heap_alloc(
					foreign->heap,
					ut_strlen(table->name) + 1);
		}

		strcpy(foreign->foreign_table_name, table->name);

		if (strchr(foreign->id, '/')) {
			ulint	db_len;
			char*	old_id;

			/* This is a >= 4.0.18 format id */

			old_id = mem_strdup(foreign->id);

			if (ut_strlen(foreign->id) > ut_strlen(old_name)
						+ ((sizeof dict_ibfk) - 1)
			    && 0 == ut_memcmp(foreign->id, old_name,
						ut_strlen(old_name))
			    && 0 == ut_memcmp(
					foreign->id + ut_strlen(old_name),
					dict_ibfk, (sizeof dict_ibfk) - 1)) {

				/* This is a generated >= 4.0.18 format id */

				if (ut_strlen(table->name) > ut_strlen(old_name)) {
					foreign->id = mem_heap_alloc(
					     foreign->heap,
						ut_strlen(table->name)
						+ ut_strlen(old_id) + 1);
				}
				
				/* Replace the prefix 'databasename/tablename'
				with the new names */
				strcpy(foreign->id, table->name);
				strcat(foreign->id,
						old_id + ut_strlen(old_name));
			} else {
				/* This is a >= 4.0.18 format id where the user
				gave the id name */
				db_len = dict_get_db_name_len(table->name) + 1;

				if (dict_get_db_name_len(table->name)
			    	    > dict_get_db_name_len(foreign->id)) {

					foreign->id = mem_heap_alloc(
					     foreign->heap,
				 	     db_len + ut_strlen(old_id) + 1);
				}

				/* Replace the database prefix in id with the
				one from table->name */
			
				ut_memcpy(foreign->id, table->name, db_len);

				strcpy(foreign->id + db_len,
						dict_remove_db_name(old_id));
			}

			mem_free(old_id);
		}

		foreign = UT_LIST_GET_NEXT(foreign_list, foreign);
	}

	foreign = UT_LIST_GET_FIRST(table->referenced_list);

	while (foreign != NULL) {
		if (ut_strlen(foreign->referenced_table_name) <
						ut_strlen(table->name)) {
			/* Allocate a longer name buffer;
			TODO: store buf len to save memory */

			foreign->referenced_table_name = mem_heap_alloc(
					foreign->heap,
					ut_strlen(table->name) + 1);
		}

		strcpy(foreign->referenced_table_name, table->name);

		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}

	return(TRUE);
}

/**************************************************************************
Change the id of a table object in the dictionary cache. This is used in
DISCARD TABLESPACE. */

void
dict_table_change_id_in_cache(
/*==========================*/
	dict_table_t*	table,	/* in: table object already in cache */
	dulint		new_id)	/* in: new id to set */
{
	ut_ad(table);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/* Remove the table from the hash table of id's */

	HASH_DELETE(dict_table_t, id_hash, dict_sys->table_id_hash,
					ut_fold_dulint(table->id), table);
	table->id = new_id;

	/* Add the table back to the hash table */
	HASH_INSERT(dict_table_t, id_hash, dict_sys->table_id_hash,
					ut_fold_dulint(table->id), table);
}

/**************************************************************************
Removes a table object from the dictionary cache. */

void
dict_table_remove_from_cache(
/*=========================*/
	dict_table_t*	table)	/* in, own: table */
{
	dict_foreign_t*	foreign;
	dict_index_t*	index;
	ulint		size;
	ulint		i;
	
	ut_ad(table);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

#if 0
	fputs("Removing table ", stderr);
	ut_print_name(stderr, table->name, ULINT_UNDEFINED);
	fputs(" from dictionary cache\n", stderr);
#endif

	/* Remove the foreign constraints from the cache */
	foreign = UT_LIST_GET_LAST(table->foreign_list);

	while (foreign != NULL) {
		dict_foreign_remove_from_cache(foreign);
		foreign = UT_LIST_GET_LAST(table->foreign_list);
	}

	/* Reset table field in referencing constraints */

	foreign = UT_LIST_GET_FIRST(table->referenced_list);

	while (foreign != NULL) {
		foreign->referenced_table = NULL;
		foreign->referenced_index = NULL;
		
		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}

	/* Remove the indexes from the cache */
	index = UT_LIST_GET_LAST(table->indexes);

	while (index != NULL) {
		dict_index_remove_from_cache(table, index);
		index = UT_LIST_GET_LAST(table->indexes);
	}

	/* Remove the columns of the table from the cache */
	for (i = 0; i < table->n_cols; i++) {
		dict_col_remove_from_cache(table,
					   dict_table_get_nth_col(table, i));
	}

	/* Remove table from the hash tables of tables */
	HASH_DELETE(dict_table_t, name_hash, dict_sys->table_hash,
					ut_fold_string(table->name), table);
	HASH_DELETE(dict_table_t, id_hash, dict_sys->table_id_hash,
					ut_fold_dulint(table->id), table);

	/* Remove table from LRU list of tables */
	UT_LIST_REMOVE(table_LRU, dict_sys->table_LRU, table);

	mutex_free(&(table->autoinc_mutex));

	size = mem_heap_get_size(table->heap);

	ut_ad(dict_sys->size >= size);

	dict_sys->size -= size;

	mem_heap_free(table->heap);
}

/**************************************************************************
Frees tables from the end of table_LRU if the dictionary cache occupies
too much space. Currently not used! */

void
dict_table_LRU_trim(void)
/*=====================*/
{
	dict_table_t*	table;
	dict_table_t*	prev_table;

	ut_error;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	table = UT_LIST_GET_LAST(dict_sys->table_LRU);

	while (table && (dict_sys->size >
			 buf_pool_get_max_size() / DICT_POOL_PER_VARYING)) {

		prev_table = UT_LIST_GET_PREV(table_LRU, table);

		if (table->mem_fix == 0) {
			dict_table_remove_from_cache(table);
		}

		table = prev_table;
	}
}

/**************************************************************************
Adds a column to the data dictionary hash table. */
static
void
dict_col_add_to_cache(
/*==================*/
	dict_table_t*	table,	/* in: table */
	dict_col_t*	col)	/* in: column */
{
	ulint	fold;

	ut_ad(table && col);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	
	fold = ut_fold_ulint_pair(ut_fold_string(table->name),
				  ut_fold_string(col->name));

	/* Look for a column with same table name and column name: error */
	{
		dict_col_t*	col2;
		HASH_SEARCH(hash, dict_sys->col_hash, fold, col2,
			(ut_strcmp(col->name, col2->name) == 0)
			&& (ut_strcmp((col2->table)->name, table->name)
							== 0));  
		ut_a(col2 == NULL);
	}

	HASH_INSERT(dict_col_t, hash, dict_sys->col_hash, fold, col);
}

/**************************************************************************
Removes a column from the data dictionary hash table. */
static
void
dict_col_remove_from_cache(
/*=======================*/
	dict_table_t*	table,	/* in: table */
	dict_col_t*	col)	/* in: column */
{
	ulint		fold;

	ut_ad(table && col);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	
	fold = ut_fold_ulint_pair(ut_fold_string(table->name),
				  ut_fold_string(col->name));

	HASH_DELETE(dict_col_t, hash, dict_sys->col_hash, fold, col);
}

/**************************************************************************
Repositions a column in the data dictionary hash table when the table name
changes. */
static
void
dict_col_reposition_in_cache(
/*=========================*/
	dict_table_t*	table,		/* in: table */
	dict_col_t*	col,		/* in: column */
	const char*	new_name)	/* in: new table name */
{
	ulint		fold;

	ut_ad(table && col);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	
	fold = ut_fold_ulint_pair(ut_fold_string(table->name),
				  ut_fold_string(col->name));

	HASH_DELETE(dict_col_t, hash, dict_sys->col_hash, fold, col);

	fold = ut_fold_ulint_pair(ut_fold_string(new_name),
				  ut_fold_string(col->name));
				  
	HASH_INSERT(dict_col_t, hash, dict_sys->col_hash, fold, col);
}

/**************************************************************************
Adds an index to the dictionary cache. */

ibool
dict_index_add_to_cache(
/*====================*/
				/* out: TRUE if success */
	dict_table_t*	table,	/* in: table on which the index is */
	dict_index_t*	index,	/* in, own: index; NOTE! The index memory
				object is freed in this function! */
	ulint		page_no)/* in: root page number of the index */
{
	dict_index_t*	new_index;
	dict_tree_t*	tree;
	dict_table_t*	cluster;
	dict_field_t*	field;
	ulint		n_ord;
	ibool		success;
	ulint		i;
	
	ut_ad(index);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(index->n_def == index->n_fields);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	
	ut_ad(mem_heap_validate(index->heap));

	{
		dict_index_t*	index2;
		index2 = UT_LIST_GET_FIRST(table->indexes);

		while (index2 != NULL) {
			ut_ad(ut_strcmp(index->name, index2->name) != 0);

			index2 = UT_LIST_GET_NEXT(indexes, index2);
		}

		ut_a(UT_LIST_GET_LEN(table->indexes) == 0
	      			|| (index->type & DICT_CLUSTERED) == 0);
	}

	success = dict_index_find_cols(table, index);

	if (!success) {
		dict_mem_index_free(index);

		return(FALSE);
	}
	
	/* Build the cache internal representation of the index,
	containing also the added system fields */

	if (index->type & DICT_CLUSTERED) {
		new_index = dict_index_build_internal_clust(table, index);
	} else {
		new_index = dict_index_build_internal_non_clust(table, index);
	}

	new_index->search_info = btr_search_info_create(new_index->heap);
	
	/* Set the n_fields value in new_index to the actual defined
	number of fields in the cache internal representation */

	new_index->n_fields = new_index->n_def;
	
	/* Add the new index as the last index for the table */

	UT_LIST_ADD_LAST(indexes, table->indexes, new_index);	
	new_index->table = table;
	new_index->table_name = table->name;

	/* Increment the ord_part counts in columns which are ordering */

	if (UNIV_UNLIKELY(index->type & DICT_UNIVERSAL)) {
		n_ord = new_index->n_fields;
	} else {
		n_ord = dict_index_get_n_unique(new_index);
	}

	for (i = 0; i < n_ord; i++) {

		field = dict_index_get_nth_field(new_index, i);

		dict_field_get_col(field)->ord_part++;
	}

	if (table->type == DICT_TABLE_CLUSTER_MEMBER) {
		/* The index tree is found from the cluster object */
	    
		cluster = dict_table_get_low(table->cluster_name);

		tree = dict_index_get_tree(
					UT_LIST_GET_FIRST(cluster->indexes));
		new_index->tree = tree;
	} else {
		/* Create an index tree memory object for the index */
		tree = dict_tree_create(new_index, page_no);
		ut_ad(tree);

		new_index->tree = tree;
	}

	if (!UNIV_UNLIKELY(new_index->type & DICT_UNIVERSAL)) {

		new_index->stat_n_diff_key_vals =
			mem_heap_alloc(new_index->heap,
				(1 + dict_index_get_n_unique(new_index))
				* sizeof(ib_longlong));
		/* Give some sensible values to stat_n_... in case we do
		not calculate statistics quickly enough */

		for (i = 0; i <= dict_index_get_n_unique(new_index); i++) {

			new_index->stat_n_diff_key_vals[i] = 100;
		}
	}
	
	/* Add the index to the list of indexes stored in the tree */
	UT_LIST_ADD_LAST(tree_indexes, tree->tree_indexes, new_index); 
	
	/* If the dictionary cache grows too big, trim the table LRU list */

	dict_sys->size += mem_heap_get_size(new_index->heap);
	/* dict_table_LRU_trim(); */

	dict_mem_index_free(index);

	return(TRUE);
}

/**************************************************************************
Removes an index from the dictionary cache. */
static
void
dict_index_remove_from_cache(
/*=========================*/
	dict_table_t*	table,	/* in: table */
	dict_index_t*	index)	/* in, own: index */
{
	dict_field_t*	field;
	ulint		size;
	ulint		i;

	ut_ad(table && index);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	ut_ad(UT_LIST_GET_LEN((index->tree)->tree_indexes) == 1);
	dict_tree_free(index->tree);

	/* Decrement the ord_part counts in columns which are ordering */
	for (i = 0; i < dict_index_get_n_unique(index); i++) {

		field = dict_index_get_nth_field(index, i);

		ut_ad(dict_field_get_col(field)->ord_part > 0);
		(dict_field_get_col(field)->ord_part)--;
	}

	/* Remove the index from the list of indexes of the table */
	UT_LIST_REMOVE(indexes, table->indexes, index);

	size = mem_heap_get_size(index->heap);

	ut_ad(dict_sys->size >= size);

	dict_sys->size -= size;

	mem_heap_free(index->heap);
}

/***********************************************************************
Tries to find column names for the index in the column hash table and
sets the col field of the index. */
static
ibool
dict_index_find_cols(
/*=================*/
				/* out: TRUE if success */
	dict_table_t*	table,	/* in: table */
	dict_index_t*	index)	/* in: index */	
{
	dict_col_t*	col;
	dict_field_t*	field;
	ulint		fold;
	ulint		i;
	
	ut_ad(table && index);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	for (i = 0; i < index->n_fields; i++) {
		field = dict_index_get_nth_field(index, i);

		fold = ut_fold_ulint_pair(ut_fold_string(table->name),
				  	       ut_fold_string(field->name));
			
		HASH_SEARCH(hash, dict_sys->col_hash, fold, col,
				(ut_strcmp(col->name, field->name) == 0)
				&& (ut_strcmp((col->table)->name, table->name)
								== 0));  
		if (col == NULL) {

 			return(FALSE);
		} else {
			field->col = col;
		}
	}

	return(TRUE);
}
	
/***********************************************************************
Adds a column to index. */

void
dict_index_add_col(
/*===============*/
	dict_index_t*	index,		/* in: index */
	dict_col_t*	col,		/* in: column */
	ulint		order,		/* in: order criterion */
	ulint		prefix_len)	/* in: column prefix length */
{
	dict_field_t*	field;

	dict_mem_index_add_field(index, col->name, order, prefix_len);

	field = dict_index_get_nth_field(index, index->n_def - 1);

	field->col = col;
	field->fixed_len = dtype_get_fixed_size(&col->type);

	if (prefix_len && field->fixed_len > prefix_len) {
		field->fixed_len = prefix_len;
	}

	/* Long fixed-length fields that need external storage are treated as
	variable-length fields, so that the extern flag can be embedded in
	the length word. */

	if (field->fixed_len > DICT_MAX_INDEX_COL_LEN) {
		field->fixed_len = 0;
	}

	if (!(dtype_get_prtype(&col->type) & DATA_NOT_NULL)) {
		index->n_nullable++;
	}

	if (index->n_def > 1) {
		const dict_field_t*	field2 =
			dict_index_get_nth_field(index, index->n_def - 2);
		field->fixed_offs = (!field2->fixed_len ||
					field2->fixed_offs == ULINT_UNDEFINED)
				? ULINT_UNDEFINED
				: field2->fixed_len + field2->fixed_offs;
	} else {
		field->fixed_offs = 0;
	}
}

/***********************************************************************
Copies fields contained in index2 to index1. */
static
void
dict_index_copy(
/*============*/
	dict_index_t*	index1,	/* in: index to copy to */
	dict_index_t*	index2,	/* in: index to copy from */
	ulint		start,	/* in: first position to copy */
	ulint		end)	/* in: last position to copy */
{
	dict_field_t*	field;
	ulint		i;
	
	/* Copy fields contained in index2 */

	for (i = start; i < end; i++) {

		field = dict_index_get_nth_field(index2, i);
		dict_index_add_col(index1, field->col, field->order,
						      field->prefix_len);
	}
}

/***********************************************************************
Copies types of fields contained in index to tuple. */

void
dict_index_copy_types(
/*==================*/
	dtuple_t*	tuple,		/* in: data tuple */
	dict_index_t*	index,		/* in: index */
	ulint		n_fields)	/* in: number of field types to copy */
{
	dtype_t*	dfield_type;
	dtype_t*	type;
	ulint		i;

	if (UNIV_UNLIKELY(index->type & DICT_UNIVERSAL)) {
		dtuple_set_types_binary(tuple, n_fields);

		return;
	}

	for (i = 0; i < n_fields; i++) {
		dfield_type = dfield_get_type(dtuple_get_nth_field(tuple, i));
		type = dict_col_get_type(dict_field_get_col(
				dict_index_get_nth_field(index, i)));
		*dfield_type = *type;
	}
}

/***********************************************************************
Copies types of columns contained in table to tuple. */

void
dict_table_copy_types(
/*==================*/
	dtuple_t*	tuple,	/* in: data tuple */
	dict_table_t*	table)	/* in: index */
{
	dtype_t*	dfield_type;
	dtype_t*	type;
	ulint		i;

	ut_ad(!(table->type & DICT_UNIVERSAL));

	for (i = 0; i < dtuple_get_n_fields(tuple); i++) {

		dfield_type = dfield_get_type(dtuple_get_nth_field(tuple, i));
		type = dict_col_get_type(dict_table_get_nth_col(table, i));

		*dfield_type = *type;
	}
}

/***********************************************************************
Builds the internal dictionary cache representation for a clustered
index, containing also system fields not defined by the user. */
static
dict_index_t*
dict_index_build_internal_clust(
/*============================*/
				/* out, own: the internal representation
				of the clustered index */
	dict_table_t*	table,	/* in: table */
	dict_index_t*	index)	/* in: user representation of a clustered
				index */	
{
	dict_index_t*	new_index;
	dict_field_t*	field;
	dict_col_t*	col;
	ulint		fixed_size;
	ulint		trx_id_pos;
	ulint		i;

	ut_ad(table && index);
	ut_ad(index->type & DICT_CLUSTERED);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/* Create a new index object with certainly enough fields */
	new_index = dict_mem_index_create(table->name,
				     index->name,
				     table->space,
				     index->type,
				     index->n_fields + table->n_cols);

	/* Copy other relevant data from the old index struct to the new
	struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;
	
	new_index->id = index->id;

	if (table->type != DICT_TABLE_ORDINARY) {
		/* The index is mixed: copy common key prefix fields */
		
		dict_index_copy(new_index, index, 0, table->mix_len);

		/* Add the mix id column */
		dict_index_add_col(new_index,
			  dict_table_get_sys_col(table, DATA_MIX_ID), 0, 0);

		/* Copy the rest of fields */
		dict_index_copy(new_index, index, table->mix_len,
							index->n_fields);
	} else {
		/* Copy the fields of index */
		dict_index_copy(new_index, index, 0, index->n_fields);
	}

	if (UNIV_UNLIKELY(index->type & DICT_UNIVERSAL)) {
		/* No fixed number of fields determines an entry uniquely */

		new_index->n_uniq = ULINT_MAX;
		
	} else if (index->type & DICT_UNIQUE) {
		/* Only the fields defined so far are needed to identify
		the index entry uniquely */

		new_index->n_uniq = new_index->n_def;
	} else {
		/* Also the row id is needed to identify the entry */
		new_index->n_uniq = 1 + new_index->n_def;
	}

	new_index->trx_id_offset = 0;

	if (!(index->type & DICT_IBUF)) {
		/* Add system columns, trx id first */

		trx_id_pos = new_index->n_def;

		ut_ad(DATA_ROW_ID == 0);
		ut_ad(DATA_TRX_ID == 1);
		ut_ad(DATA_ROLL_PTR == 2);

		if (!(index->type & DICT_UNIQUE)) {
			dict_index_add_col(new_index,
			   dict_table_get_sys_col(table, DATA_ROW_ID), 0, 0);
			trx_id_pos++;
		}

		dict_index_add_col(new_index,
			   dict_table_get_sys_col(table, DATA_TRX_ID), 0, 0);
	
		dict_index_add_col(new_index,
			   dict_table_get_sys_col(table, DATA_ROLL_PTR), 0, 0);

		for (i = 0; i < trx_id_pos; i++) {

			fixed_size = dtype_get_fixed_size(
				dict_index_get_nth_type(new_index, i));

			if (fixed_size == 0) {
				new_index->trx_id_offset = 0;

				break;
			}

			if (dict_index_get_nth_field(new_index, i)->prefix_len
			    > 0) {
				new_index->trx_id_offset = 0;

				break;
			}

			new_index->trx_id_offset += fixed_size;
		}

	}

	/* Set auxiliary variables in table columns as undefined */
	for (i = 0; i < table->n_cols; i++) {

		col = dict_table_get_nth_col(table, i);
		col->aux = ULINT_UNDEFINED;
	}

	/* Mark with 0 the table columns already contained in new_index */
	for (i = 0; i < new_index->n_def; i++) {

		field = dict_index_get_nth_field(new_index, i);

		/* If there is only a prefix of the column in the index
		field, do not mark the column as contained in the index */

		if (field->prefix_len == 0) {

		        field->col->aux = 0;
		}
	}
	
	/* Add to new_index non-system columns of table not yet included
	there */
	for (i = 0; i < table->n_cols - DATA_N_SYS_COLS; i++) {

		col = dict_table_get_nth_col(table, i);
		ut_ad(col->type.mtype != DATA_SYS);

		if (col->aux == ULINT_UNDEFINED) {
			dict_index_add_col(new_index, col, 0, 0);
		}
	}

	ut_ad((index->type & DICT_IBUF)
				|| (UT_LIST_GET_LEN(table->indexes) == 0));

	/* Store to the column structs the position of the table columns
	in the clustered index */

	for (i = 0; i < new_index->n_def; i++) {
		field = dict_index_get_nth_field(new_index, i);

		if (field->prefix_len == 0) {

		        field->col->clust_pos = i;
		}
	}
	
	new_index->cached = TRUE;

	return(new_index);
}	

/***********************************************************************
Builds the internal dictionary cache representation for a non-clustered
index, containing also system fields not defined by the user. */
static
dict_index_t*
dict_index_build_internal_non_clust(
/*================================*/
				/* out, own: the internal representation
				of the non-clustered index */
	dict_table_t*	table,	/* in: table */
	dict_index_t*	index)	/* in: user representation of a non-clustered
				index */	
{
	dict_field_t*	field;
	dict_index_t*	new_index;
	dict_index_t*	clust_index;
	ulint		i;

	ut_ad(table && index);
	ut_ad(0 == (index->type & DICT_CLUSTERED));
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/* The clustered index should be the first in the list of indexes */
	clust_index = UT_LIST_GET_FIRST(table->indexes);
	
	ut_ad(clust_index);
	ut_ad(clust_index->type & DICT_CLUSTERED);
	ut_ad(!(clust_index->type & DICT_UNIVERSAL));

	/* Create a new index */
	new_index = dict_mem_index_create(table->name,
				     index->name,
				     index->space,
				     index->type,
				     index->n_fields
				     + 1 + clust_index->n_uniq);

	/* Copy other relevant data from the old index
	struct to the new struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;
	
	new_index->id = index->id;

	/* Copy fields from index to new_index */
	dict_index_copy(new_index, index, 0, index->n_fields);

	/* Set the auxiliary variables in the clust_index unique columns
	as undefined */
	for (i = 0; i < clust_index->n_uniq; i++) {

		field = dict_index_get_nth_field(clust_index, i);
		field->col->aux = ULINT_UNDEFINED;
	}

	/* Mark with 0 table columns already contained in new_index */
	for (i = 0; i < new_index->n_def; i++) {

		field = dict_index_get_nth_field(new_index, i);

		/* If there is only a prefix of the column in the index
		field, do not mark the column as contained in the index */

		if (field->prefix_len == 0) {

		        field->col->aux = 0;
		}
	}

	/* Add to new_index the columns necessary to determine the clustered
	index entry uniquely */

	for (i = 0; i < clust_index->n_uniq; i++) {

		field = dict_index_get_nth_field(clust_index, i);

		if (field->col->aux == ULINT_UNDEFINED) {
			dict_index_add_col(new_index, field->col, 0,
						      field->prefix_len);
		}
	}

	if ((index->type) & DICT_UNIQUE) {
		new_index->n_uniq = index->n_fields;
	} else {
		new_index->n_uniq = new_index->n_def;
	}

	/* Set the n_fields value in new_index to the actual defined
	number of fields */

	new_index->n_fields = new_index->n_def;

	new_index->cached = TRUE;

	return(new_index);
}	

/*====================== FOREIGN KEY PROCESSING ========================*/

/*************************************************************************
Checks if a table is referenced by foreign keys. */

ibool
dict_table_referenced_by_foreign_key(
/*=================================*/
				/* out: TRUE if table is referenced by a
				foreign key */
	dict_table_t*	table)	/* in: InnoDB table */
{
	if (UT_LIST_GET_LEN(table->referenced_list) > 0) {
		
		return(TRUE);
	}

	return(FALSE);
}

/*************************************************************************
Frees a foreign key struct. */
static
void
dict_foreign_free(
/*==============*/
	dict_foreign_t*	foreign)	/* in, own: foreign key struct */
{
	mem_heap_free(foreign->heap);
}

/**************************************************************************
Removes a foreign constraint struct from the dictionary cache. */
static
void
dict_foreign_remove_from_cache(
/*===========================*/
	dict_foreign_t*	foreign)	/* in, own: foreign constraint */
{
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	ut_a(foreign);
	
	if (foreign->referenced_table) {
		UT_LIST_REMOVE(referenced_list,
			foreign->referenced_table->referenced_list, foreign);
	}

	if (foreign->foreign_table) {
		UT_LIST_REMOVE(foreign_list,
			foreign->foreign_table->foreign_list, foreign);
	}

	dict_foreign_free(foreign);
}

/**************************************************************************
Looks for the foreign constraint from the foreign and referenced lists
of a table. */
static
dict_foreign_t*
dict_foreign_find(
/*==============*/
				/* out: foreign constraint */
	dict_table_t*	table,	/* in: table object */
	const char*	id)	/* in: foreign constraint id */
{
	dict_foreign_t*	foreign;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	foreign = UT_LIST_GET_FIRST(table->foreign_list);

	while (foreign) {
		if (ut_strcmp(id, foreign->id) == 0) {

			return(foreign);
		}

		foreign = UT_LIST_GET_NEXT(foreign_list, foreign);
	}
	
	foreign = UT_LIST_GET_FIRST(table->referenced_list);

	while (foreign) {
		if (ut_strcmp(id, foreign->id) == 0) {

			return(foreign);
		}

		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}

	return(NULL);
}	

/*************************************************************************
Tries to find an index whose first fields are the columns in the array,
in the same order. */
static
dict_index_t*
dict_foreign_find_index(
/*====================*/
				/* out: matching index, NULL if not found */
	dict_table_t*	table,	/* in: table */
	const char**	columns,/* in: array of column names */
	ulint		n_cols,	/* in: number of columns */
	dict_index_t*	types_idx, /* in: NULL or an index to whose types the
				   column types must match */
	ibool		check_charsets)	/* in: whether to check charsets.
					only has an effect if types_idx !=
					NULL. */
{
#ifndef UNIV_HOTBACKUP
	dict_index_t*	index;
	const char*	col_name;
	ulint		i;
	
	index = dict_table_get_first_index(table);

	while (index != NULL) {
		if (dict_index_get_n_fields(index) >= n_cols) {

			for (i = 0; i < n_cols; i++) {
				col_name = dict_index_get_nth_field(index, i)
							->col->name;
				if (dict_index_get_nth_field(index, i)
						->prefix_len != 0) {
					/* We do not accept column prefix
					indexes here */
					
					break;
				}

				if (0 != innobase_strcasecmp(columns[i],
								col_name)) {
				  	break;
				}

				if (types_idx && !cmp_types_are_equal(
				     dict_index_get_nth_type(index, i),
				     dict_index_get_nth_type(types_idx, i),
				     check_charsets)) {

				  	break;
				}		
			}

			if (i == n_cols) {
				/* We found a matching index */

				return(index);
			}
		}

		index = dict_table_get_next_index(index);
	}

	return(NULL);
#else /* UNIV_HOTBACKUP */
	/* This function depends on MySQL code that is not included in
	InnoDB Hot Backup builds.  Besides, this function should never
	be called in InnoDB Hot Backup. */
	ut_error;
#endif /* UNIV_HOTBACKUP */
}

/**************************************************************************
Report an error in a foreign key definition. */
static
void
dict_foreign_error_report_low(
/*==========================*/
	FILE*		file,	/* in: output stream */
	const char*	name)	/* in: table name */
{
	rewind(file);
	ut_print_timestamp(file);
	fprintf(file, " Error in foreign key constraint of table %s:\n",
		name);
}

/**************************************************************************
Report an error in a foreign key definition. */
static
void
dict_foreign_error_report(
/*======================*/
	FILE*		file,	/* in: output stream */
	dict_foreign_t*	fk,	/* in: foreign key constraint */
	const char*	msg)	/* in: the error message */
{
	mutex_enter(&dict_foreign_err_mutex);
	dict_foreign_error_report_low(file, fk->foreign_table_name);
	fputs(msg, file);
	fputs(" Constraint:\n", file);
	dict_print_info_on_foreign_key_in_create_format(file, NULL, fk, TRUE);
	if (fk->foreign_index) {
		fputs("\nThe index in the foreign key in table is ", file);
		ut_print_name(file, NULL, fk->foreign_index->name);
		fputs(
"\nSee http://dev.mysql.com/doc/mysql/en/InnoDB_foreign_key_constraints.html\n"
"for correct foreign key definition.\n",
		file);
	}
	mutex_exit(&dict_foreign_err_mutex);
}

/**************************************************************************
Adds a foreign key constraint object to the dictionary cache. May free
the object if there already is an object with the same identifier in.
At least one of the foreign table and the referenced table must already
be in the dictionary cache! */

ulint
dict_foreign_add_to_cache(
/*======================*/
					/* out: DB_SUCCESS or error code */
	dict_foreign_t*	foreign,	/* in, own: foreign key constraint */
	ibool		check_charsets)	/* in: TRUE=check charset
					compatibility */
{
	dict_table_t*	for_table;
	dict_table_t*	ref_table;
	dict_foreign_t*	for_in_cache		= NULL;
	dict_index_t*	index;
	ibool		added_to_referenced_list= FALSE;
	FILE*		ef 			= dict_foreign_err_file;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	for_table = dict_table_check_if_in_cache_low(
					foreign->foreign_table_name);
	
	ref_table = dict_table_check_if_in_cache_low(
					foreign->referenced_table_name);
	ut_a(for_table || ref_table);

	if (for_table) {
		for_in_cache = dict_foreign_find(for_table, foreign->id);
	}

	if (!for_in_cache && ref_table) {
		for_in_cache = dict_foreign_find(ref_table, foreign->id);
	}

	if (for_in_cache) {
		/* Free the foreign object */
		mem_heap_free(foreign->heap);
	} else {
		for_in_cache = foreign;
	}

	if (for_in_cache->referenced_table == NULL && ref_table) {
		index = dict_foreign_find_index(ref_table,
			(const char**) for_in_cache->referenced_col_names,
			for_in_cache->n_fields,
			for_in_cache->foreign_index, check_charsets);

		if (index == NULL) {
			dict_foreign_error_report(ef, for_in_cache,
"there is no index in referenced table which would contain\n"
"the columns as the first columns, or the data types in the\n"
"referenced table do not match to the ones in table.");

			if (for_in_cache == foreign) {
				mem_heap_free(foreign->heap);
			}

		    	return(DB_CANNOT_ADD_CONSTRAINT);
		}

		for_in_cache->referenced_table = ref_table;
		for_in_cache->referenced_index = index;
		UT_LIST_ADD_LAST(referenced_list,
					ref_table->referenced_list,
					for_in_cache);
		added_to_referenced_list = TRUE;
	}

	if (for_in_cache->foreign_table == NULL && for_table) {
		index = dict_foreign_find_index(for_table,
			(const char**) for_in_cache->foreign_col_names,
			for_in_cache->n_fields,
			for_in_cache->referenced_index, check_charsets);

		if (index == NULL) {
			dict_foreign_error_report(ef, for_in_cache,
"there is no index in the table which would contain\n"
"the columns as the first columns, or the data types in the\n"
"table do not match to the ones in the referenced table.");

			if (for_in_cache == foreign) {
				if (added_to_referenced_list) {
					UT_LIST_REMOVE(referenced_list,
						ref_table->referenced_list,
						for_in_cache);
				}
			
				mem_heap_free(foreign->heap);
			}

		    	return(DB_CANNOT_ADD_CONSTRAINT);
		}

		for_in_cache->foreign_table = for_table;
		for_in_cache->foreign_index = index;
		UT_LIST_ADD_LAST(foreign_list,
					for_table->foreign_list,
					for_in_cache);
	}

	return(DB_SUCCESS);
}

/*************************************************************************
Scans from pointer onwards. Stops if is at the start of a copy of
'string' where characters are compared without case sensitivity, and
only outside `` or "" quotes. Stops also at '\0'. */

const char*
dict_scan_to(
/*=========*/
				/* out: scanned up to this */
	const char*	ptr,	/* in: scan from */
	const char*	string)	/* in: look for this */
{
	char	quote	= '\0';

	for (; *ptr; ptr++) {
		if (*ptr == quote) {
			/* Closing quote character: do not look for
			starting quote or the keyword. */
			quote = '\0';
		} else if (quote) {
			/* Within quotes: do nothing. */
		} else if (*ptr == '`' || *ptr == '"') {
			/* Starting quote: remember the quote character. */
			quote = *ptr;
		} else {
			/* Outside quotes: look for the keyword. */
			ulint	i;
			for (i = 0; string[i]; i++) {
				if (toupper((int)(unsigned char)(ptr[i]))
						!= toupper((int)(unsigned char)
						(string[i]))) {
					goto nomatch;
				}
			}
			break;
		nomatch:
			;
		}
	}

	return(ptr);
}

/*************************************************************************
Accepts a specified string. Comparisons are case-insensitive. */

const char*
dict_accept(
/*========*/
				/* out: if string was accepted, the pointer
				is moved after that, else ptr is returned */
	const char*	ptr,	/* in: scan from this */
	const char*	string,	/* in: accept only this string as the next
				non-whitespace string */
	ibool*		success)/* out: TRUE if accepted */
{
	const char*	old_ptr = ptr;
	const char*	old_ptr2;

	*success = FALSE;
	
	while (isspace(*ptr)) {
		ptr++;
	}

	old_ptr2 = ptr;
	
	ptr = dict_scan_to(ptr, string);
	
	if (*ptr == '\0' || old_ptr2 != ptr) {
		return(old_ptr);
	}

	*success = TRUE;

	return(ptr + ut_strlen(string));
}

/*************************************************************************
Scans an id. For the lexical definition of an 'id', see the code below.
Strips backquotes or double quotes from around the id. */
static
const char*
dict_scan_id(
/*=========*/
				/* out: scanned to */
	const char*	ptr,	/* in: scanned to */
	mem_heap_t*	heap,	/* in: heap where to allocate the id
				(NULL=id will not be allocated, but it
				will point to string near ptr) */
	const char**	id,	/* out,own: the id; NULL if no id was
				scannable */
	ibool		accept_also_dot)
				/* in: TRUE if also a dot can appear in a
				non-quoted id; in a quoted id it can appear
				always */
{
	char		quote	= '\0';
	ulint		len	= 0;
	const char*	s;
	char*		d;
	ulint		id_len;
	byte*		b;

	*id = NULL;

	while (isspace(*ptr)) {
		ptr++;
	}

	if (*ptr == '\0') {

		return(ptr);
	}

	if (*ptr == '`' || *ptr == '"') {
		quote = *ptr++;
	}

	s = ptr;

	if (quote) {
		for (;;) {
			if (!*ptr) {
				/* Syntax error */
				return(ptr);
			}
			if (*ptr == quote) {
				ptr++;
				if (*ptr != quote) {
					break;
				}
			}
			ptr++;
			len++;
		}
	} else {
		while (!isspace(*ptr) && *ptr != '(' && *ptr != ')'
		       && (accept_also_dot || *ptr != '.')
		       && *ptr != ',' && *ptr != '\0') {

			ptr++;
		}

		len = ptr - s;
	}

	if (quote && heap) {
		*id = d = mem_heap_alloc(heap, len + 1);
		while (len--) {
			if ((*d++ = *s++) == quote) {
				s++;
			}
		}
		*d++ = 0;
		ut_a(*s == quote);
		ut_a(s + 1 == ptr);
	} else if (heap) {
		*id = mem_heap_strdupl(heap, s, len);
	} else {
		/* no heap given: id will point to source string */
		*id = s;
	}

	if (heap && !quote) {
		/* EMS MySQL Manager sometimes adds characters 0xA0 (in
		latin1, a 'non-breakable space') to the end of a table name.
		But isspace(0xA0) is not true, which confuses our foreign key
		parser. After the UTF-8 conversion in ha_innodb.cc, bytes 0xC2
		and 0xA0 are at the end of the string.

		TODO: we should lex the string using thd->charset_info, and
		my_isspace(). Only after that, convert id names to UTF-8. */

		b = (byte*)(*id);
		id_len = strlen((char*) b);
		
		if (id_len >= 3 && b[id_len - 1] == 0xA0
			       && b[id_len - 2] == 0xC2) {

			/* Strip the 2 last bytes */

			b[id_len - 2] = '\0';
		}
	}

	return(ptr);
}

/*************************************************************************
Tries to scan a column name. */
static
const char*
dict_scan_col(
/*==========*/
				/* out: scanned to */
	const char*	ptr,	/* in: scanned to */
	ibool*		success,/* out: TRUE if success */
	dict_table_t*	table,	/* in: table in which the column is */
	dict_col_t**	column,	/* out: pointer to column if success */
	mem_heap_t*	heap,	/* in: heap where to allocate the name */
	const char**	name)	/* out,own: the column name; NULL if no name
				was scannable */
{
#ifndef UNIV_HOTBACKUP
	dict_col_t*	col;
	ulint		i;

	*success = FALSE;

	ptr = dict_scan_id(ptr, heap, name, TRUE);

	if (*name == NULL) {

		return(ptr);	/* Syntax error */
	}

	if (table == NULL) {
		*success = TRUE;
		*column = NULL;
	} else {
	    	for (i = 0; i < dict_table_get_n_cols(table); i++) {

			col = dict_table_get_nth_col(table, i);

			if (0 == innobase_strcasecmp(col->name, *name)) {
		    		/* Found */

		    		*success = TRUE;
		    		*column = col;
		    		strcpy((char*) *name, col->name);

		    		break;
			}
		}
	}
	
	return(ptr);
#else /* UNIV_HOTBACKUP */
	/* This function depends on MySQL code that is not included in
	InnoDB Hot Backup builds.  Besides, this function should never
	be called in InnoDB Hot Backup. */
	ut_error;
#endif /* UNIV_HOTBACKUP */
}

/*************************************************************************
Scans a table name from an SQL string. */
static
const char*
dict_scan_table_name(
/*=================*/
				/* out: scanned to */
	const char*	ptr,	/* in: scanned to */
	dict_table_t**	table,	/* out: table object or NULL */
	const char*	name,	/* in: foreign key table name */
	ibool*		success,/* out: TRUE if ok name found */
	mem_heap_t*	heap,	/* in: heap where to allocate the id */
	const char**	ref_name)/* out,own: the table name;
				NULL if no name was scannable */
{
#ifndef UNIV_HOTBACKUP
	const char*	database_name	= NULL;
	ulint		database_name_len = 0;
	const char*	table_name	= NULL;
	ulint		table_name_len;
	const char*	scan_name;
	char*		ref;

	*success = FALSE;
	*table = NULL;
	
	ptr = dict_scan_id(ptr, heap, &scan_name, FALSE);	

	if (scan_name == NULL) {
		
		return(ptr);	/* Syntax error */
	}

	if (*ptr == '.') {
		/* We scanned the database name; scan also the table name */

		ptr++;

		database_name = scan_name;
		database_name_len = strlen(database_name);

		ptr = dict_scan_id(ptr, heap, &table_name, FALSE);

		if (table_name == NULL) {

			return(ptr);	/* Syntax error */
		}
	} else {
		/* To be able to read table dumps made with InnoDB-4.0.17 or
		earlier, we must allow the dot separator between the database
		name and the table name also to appear within a quoted
		identifier! InnoDB used to print a constraint as:
			... REFERENCES `databasename.tablename` ...
		starting from 4.0.18 it is
			... REFERENCES `databasename`.`tablename` ... */
		const char* s;

		for (s = scan_name; *s; s++) {
			if (*s == '.') {
				database_name = scan_name;
				database_name_len = s - scan_name;
				scan_name = ++s;
				break;/* to do: multiple dots? */
			}
		}

		table_name = scan_name;
	}

	if (database_name == NULL) {
		/* Use the database name of the foreign key table */

		database_name = name;
		database_name_len = dict_get_db_name_len(name);
	}

	table_name_len = strlen(table_name);

	/* Copy database_name, '/', table_name, '\0' */
	ref = mem_heap_alloc(heap, database_name_len + table_name_len + 2);
	memcpy(ref, database_name, database_name_len);
	ref[database_name_len] = '/';
	memcpy(ref + database_name_len + 1, table_name, table_name_len + 1);
#ifndef __WIN__
	if (srv_lower_case_table_names) {
#endif /* !__WIN__ */
		/* The table name is always put to lower case on Windows. */
		innobase_casedn_str(ref);
#ifndef __WIN__
	}
#endif /* !__WIN__ */

	*success = TRUE;
	*ref_name = ref;
	*table = dict_table_get_low(ref);

	return(ptr);
#else /* UNIV_HOTBACKUP */
	/* This function depends on MySQL code that is not included in
	InnoDB Hot Backup builds.  Besides, this function should never
	be called in InnoDB Hot Backup. */
	ut_error;
#endif /* UNIV_HOTBACKUP */
}

/*************************************************************************
Skips one id. The id is allowed to contain also '.'. */
static
const char*
dict_skip_word(
/*===========*/
				/* out: scanned to */
	const char*	ptr,	/* in: scanned to */
	ibool*		success)/* out: TRUE if success, FALSE if just spaces
				left in string or a syntax error */
{
	const char*	start;
	
	*success = FALSE;

	ptr = dict_scan_id(ptr, NULL, &start, TRUE);

	if (start) {
		*success = TRUE;
	}
	
	return(ptr);
}

/*************************************************************************
Removes MySQL comments from an SQL string. A comment is either
(a) '#' to the end of the line,
(b) '--<space>' to the end of the line, or
(c) '<slash><asterisk>' till the next '<asterisk><slash>' (like the familiar
C comment syntax). */
static
char*
dict_strip_comments(
/*================*/
					/* out, own: SQL string stripped from
					comments; the caller must free this
					with mem_free()! */
	const char*	sql_string)	/* in: SQL string */
{
	char*		str;
	const char*	sptr;
	char*		ptr;
 	/* unclosed quote character (0 if none) */
 	char		quote	= 0;

	str = mem_alloc(strlen(sql_string) + 1);

	sptr = sql_string;
	ptr = str;

	for (;;) {
scan_more:
		if (*sptr == '\0') {
			*ptr = '\0';

			ut_a(ptr <= str + strlen(sql_string));

			return(str);
		}

		if (*sptr == quote) {
			/* Closing quote character: do not look for
			starting quote or comments. */
			quote = 0;
		} else if (quote) {
			/* Within quotes: do not look for
			starting quotes or comments. */
		} else if (*sptr == '"' || *sptr == '`') {
			/* Starting quote: remember the quote character. */
			quote = *sptr;
		} else if (*sptr == '#'
                           || (sptr[0] == '-' && sptr[1] == '-' &&
                               sptr[2] == ' ')) {
			for (;;) {
				/* In Unix a newline is 0x0A while in Windows
				it is 0x0D followed by 0x0A */

				if (*sptr == (char)0x0A
				    || *sptr == (char)0x0D
				    || *sptr == '\0') {

					goto scan_more;
				}

				sptr++;
			}
		} else if (!quote && *sptr == '/' && *(sptr + 1) == '*') {
			for (;;) {
				if (*sptr == '*' && *(sptr + 1) == '/') {

				     	sptr += 2;

					goto scan_more;
				}

				if (*sptr == '\0') {

					goto scan_more;
				}

				sptr++;
			}
		}

		*ptr = *sptr;

		ptr++;
		sptr++;
	}
}

/*************************************************************************
Finds the highest <number> for foreign key constraints of the table. Looks
only at the >= 4.0.18-format id's, which are of the form
databasename/tablename_ibfk_<number>. */
static
ulint
dict_table_get_highest_foreign_id(
/*==============================*/
				/* out: highest number, 0 if table has no new
				format foreign key constraints */
	dict_table_t*	table)	/* in: table in the dictionary memory cache */
{
	dict_foreign_t*	foreign;
	char*		endp;
	ulint		biggest_id	= 0;
	ulint		id;
	ulint		len;

	ut_a(table);

	len = ut_strlen(table->name);
	foreign = UT_LIST_GET_FIRST(table->foreign_list);

	while (foreign) {
		if (ut_strlen(foreign->id) > ((sizeof dict_ibfk) - 1) + len
		    && 0 == ut_memcmp(foreign->id, table->name, len)
		    && 0 == ut_memcmp(foreign->id + len,
				dict_ibfk, (sizeof dict_ibfk) - 1)) {
			/* It is of the >= 4.0.18 format */

			id = strtoul(foreign->id + len + ((sizeof dict_ibfk) - 1),
					&endp, 10);
			if (*endp == '\0') {
				ut_a(id != biggest_id);

				if (id > biggest_id) {
					biggest_id = id;
				}
			}
		}

		foreign = UT_LIST_GET_NEXT(foreign_list, foreign);
	}

	return(biggest_id);
}

/*************************************************************************
Reports a simple foreign key create clause syntax error. */
static
void
dict_foreign_report_syntax_err(
/*===========================*/
	const char*	name,		/* in: table name */
	const char*	start_of_latest_foreign,
					/* in: start of the foreign key clause
					in the SQL string */
	const char*	ptr)		/* in: place of the syntax error */
{
	FILE*	 ef = dict_foreign_err_file;

	mutex_enter(&dict_foreign_err_mutex);
	dict_foreign_error_report_low(ef, name);
	fprintf(ef, "%s:\nSyntax error close to:\n%s\n",
		start_of_latest_foreign, ptr);
	mutex_exit(&dict_foreign_err_mutex);
}

/*************************************************************************
Scans a table create SQL string and adds to the data dictionary the foreign
key constraints declared in the string. This function should be called after
the indexes for a table have been created. Each foreign key constraint must
be accompanied with indexes in both participating tables. The indexes are
allowed to contain more fields than mentioned in the constraint. */
static
ulint
dict_create_foreign_constraints_low(
/*================================*/
				/* out: error code or DB_SUCCESS */
	trx_t*		trx,	/* in: transaction */
	mem_heap_t*	heap,	/* in: memory heap */
	const char*	sql_string,
				/* in: CREATE TABLE or ALTER TABLE statement
				where foreign keys are declared like:
				FOREIGN KEY (a, b) REFERENCES table2(c, d),
				table2 can be written also with the database
				name before it: test.table2; the default
				database is the database of parameter name */
	const char*	name,	/* in: table full name in the normalized form
				database_name/table_name */
	ibool		reject_fks)
				/* in: if TRUE, fail with error code
				DB_CANNOT_ADD_CONSTRAINT if any foreign
				keys are found. */
{
	dict_table_t*	table;
	dict_table_t*	referenced_table;
	dict_table_t*	table_to_alter;
	ulint		highest_id_so_far	= 0;
	dict_index_t*	index;
	dict_foreign_t*	foreign;
 	const char*	ptr			= sql_string;
	const char*	start_of_latest_foreign	= sql_string;
	FILE*		ef			= dict_foreign_err_file;
	const char*	constraint_name;
	ibool		success;
	ulint		error;
	const char*	ptr1;
	const char*	ptr2;
	ulint		i;
	ulint		j;
	ibool		is_on_delete;
	ulint		n_on_deletes;
	ulint		n_on_updates;
	dict_col_t*	columns[500];
	const char*	column_names[500];
	const char*	referenced_table_name;
	
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	table = dict_table_get_low(name);

	if (table == NULL) {
		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, name);
		fprintf(ef,
"Cannot find the table in the internal data dictionary of InnoDB.\n"
"Create table statement:\n%s\n", sql_string);
		mutex_exit(&dict_foreign_err_mutex);

		return(DB_ERROR);
	}

	/* First check if we are actually doing an ALTER TABLE, and in that
	case look for the table being altered */

	ptr = dict_accept(ptr, "ALTER", &success);

	if (!success) {

		goto loop;
	}

	ptr = dict_accept(ptr, "TABLE", &success);

	if (!success) {

		goto loop;
	}

	/* We are doing an ALTER TABLE: scan the table name we are altering */

	ptr = dict_scan_table_name(ptr, &table_to_alter, name,
				&success, heap, &referenced_table_name);
	if (!success) {
		fprintf(stderr,
"InnoDB: Error: could not find the table being ALTERED in:\n%s\n", sql_string);

		return(DB_ERROR);
	}

	/* Starting from 4.0.18 and 4.1.2, we generate foreign key id's in the
	format databasename/tablename_ibfk_<number>, where <number> is local
	to the table; look for the highest <number> for table_to_alter, so
	that we can assign to new constraints higher numbers. */

	/* If we are altering a temporary table, the table name after ALTER
	TABLE does not correspond to the internal table name, and
	table_to_alter is NULL. TODO: should we fix this somehow? */

	if (table_to_alter == NULL) {
		highest_id_so_far = 0;
	} else {
		highest_id_so_far = dict_table_get_highest_foreign_id(
							table_to_alter);
	}

	/* Scan for foreign key declarations in a loop */
loop:
	/* Scan either to "CONSTRAINT" or "FOREIGN", whichever is closer */

	ptr1 = dict_scan_to(ptr, "CONSTRAINT");
	ptr2 = dict_scan_to(ptr, "FOREIGN");

	constraint_name = NULL;

	if (ptr1 < ptr2) {
		/* The user may have specified a constraint name. Pick it so
		that we can store 'databasename/constraintname' as the id of
		of the constraint to system tables. */
		ptr = ptr1;

		ptr = dict_accept(ptr, "CONSTRAINT", &success);

		ut_a(success);

		if (!isspace(*ptr) && *ptr != '"' && *ptr != '`') {
	        	goto loop;
		}

		while (isspace(*ptr)) {
			ptr++;
		}

		/* read constraint name unless got "CONSTRAINT FOREIGN" */
		if (ptr != ptr2) {
			ptr = dict_scan_id(ptr, heap, &constraint_name, FALSE);
		}
	} else {
		ptr = ptr2;
	}

	if (*ptr == '\0') {
		/* The proper way to reject foreign keys for temporary
		   tables would be to split the lexing and syntactical
		   analysis of foreign key clauses from the actual adding
		   of them, so that ha_innodb.cc could first parse the SQL
		   command, determine if there are any foreign keys, and
		   if so, immediately reject the command if the table is a
		   temporary one. For now, this kludge will work. */
		if (reject_fks && (UT_LIST_GET_LEN(table->foreign_list) > 0))
		{
			return DB_CANNOT_ADD_CONSTRAINT;
		}
		
		/**********************************************************/
		/* The following call adds the foreign key constraints
		to the data dictionary system tables on disk */
		
		error = dict_create_add_foreigns_to_dictionary(
						highest_id_so_far, table, trx);
		return(error);
	}

	start_of_latest_foreign = ptr;

	ptr = dict_accept(ptr, "FOREIGN", &success);		
	
	if (!success) {
		goto loop;
	}

	if (!isspace(*ptr)) {
	        goto loop;
	}

	ptr = dict_accept(ptr, "KEY", &success);

	if (!success) {
		goto loop;
	}

	ptr = dict_accept(ptr, "(", &success);

	if (!success) {
		/* MySQL allows also an index id before the '('; we
		skip it */
		ptr = dict_skip_word(ptr, &success);

		if (!success) {
			dict_foreign_report_syntax_err(name,
					start_of_latest_foreign, ptr);

			return(DB_CANNOT_ADD_CONSTRAINT);
		}

		ptr = dict_accept(ptr, "(", &success);

		if (!success) {
			/* We do not flag a syntax error here because in an
			ALTER TABLE we may also have DROP FOREIGN KEY abc */

		        goto loop;
		}
	}

	i = 0;

	/* Scan the columns in the first list */
col_loop1:
	ut_a(i < (sizeof column_names) / sizeof *column_names);
	ptr = dict_scan_col(ptr, &success, table, columns + i,
				heap, column_names + i);
	if (!success) {
		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, name);
		fprintf(ef, "%s:\nCannot resolve column name close to:\n%s\n",
					start_of_latest_foreign, ptr);
		mutex_exit(&dict_foreign_err_mutex);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	i++;
	
	ptr = dict_accept(ptr, ",", &success);

	if (success) {
		goto col_loop1;
	}
	
	ptr = dict_accept(ptr, ")", &success);

	if (!success) {
		dict_foreign_report_syntax_err(name, start_of_latest_foreign,
									ptr);
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Try to find an index which contains the columns
	as the first fields and in the right order */

	index = dict_foreign_find_index(table, column_names, i, NULL, TRUE);

	if (!index) {
		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, name);
		fputs("There is no index in table ", ef);
		ut_print_name(ef, NULL, name);
		fprintf(ef, " where the columns appear\n"
"as the first columns. Constraint:\n%s\n"
"See http://dev.mysql.com/doc/mysql/en/InnoDB_foreign_key_constraints.html\n"
"for correct foreign key definition.\n",
			start_of_latest_foreign);
		mutex_exit(&dict_foreign_err_mutex);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}
	ptr = dict_accept(ptr, "REFERENCES", &success);

	if (!success || !isspace(*ptr)) {
		dict_foreign_report_syntax_err(name, start_of_latest_foreign,
									ptr);
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Let us create a constraint struct */

	foreign = dict_mem_foreign_create();

	if (constraint_name) {
		ulint	db_len;	

		/* Catenate 'databasename/' to the constraint name specified
		by the user: we conceive the constraint as belonging to the
		same MySQL 'database' as the table itself. We store the name
		to foreign->id. */

		db_len = dict_get_db_name_len(table->name);

		foreign->id = mem_heap_alloc(foreign->heap,
				db_len + strlen(constraint_name) + 2);

		ut_memcpy(foreign->id, table->name, db_len);
		foreign->id[db_len] = '/';
		strcpy(foreign->id + db_len + 1, constraint_name);
	}

	foreign->foreign_table = table;
	foreign->foreign_table_name = mem_heap_strdup(foreign->heap,
							table->name);
	foreign->foreign_index = index;
	foreign->n_fields = i;
	foreign->foreign_col_names = mem_heap_alloc(foreign->heap,
							i * sizeof(void*));
	for (i = 0; i < foreign->n_fields; i++) {
		foreign->foreign_col_names[i] =
			mem_heap_strdup(foreign->heap, columns[i]->name);
	}
	
	ptr = dict_scan_table_name(ptr, &referenced_table, name,
				&success, heap, &referenced_table_name);

	/* Note that referenced_table can be NULL if the user has suppressed
	checking of foreign key constraints! */

	if (!success || (!referenced_table && trx->check_foreigns)) {
		dict_foreign_free(foreign);

		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, name);
		fprintf(ef, "%s:\nCannot resolve table name close to:\n"
			"%s\n",
			start_of_latest_foreign, ptr);
		mutex_exit(&dict_foreign_err_mutex);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}
	
	ptr = dict_accept(ptr, "(", &success);

	if (!success) {
		dict_foreign_free(foreign);
		dict_foreign_report_syntax_err(name, start_of_latest_foreign,
									ptr);
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Scan the columns in the second list */
	i = 0;

col_loop2:
	ptr = dict_scan_col(ptr, &success, referenced_table, columns + i,
				heap, column_names + i);
	i++;
	
	if (!success) {
		dict_foreign_free(foreign);

		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, name);
		fprintf(ef, "%s:\nCannot resolve column name close to:\n"
			"%s\n",
			start_of_latest_foreign, ptr);
		mutex_exit(&dict_foreign_err_mutex);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	ptr = dict_accept(ptr, ",", &success);

	if (success) {
		goto col_loop2;
	}
	
	ptr = dict_accept(ptr, ")", &success);

	if (!success || foreign->n_fields != i) {
		dict_foreign_free(foreign);
		
		dict_foreign_report_syntax_err(name, start_of_latest_foreign,
									ptr);
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	n_on_deletes = 0;
	n_on_updates = 0;
	
scan_on_conditions:
	/* Loop here as long as we can find ON ... conditions */

	ptr = dict_accept(ptr, "ON", &success);

	if (!success) {

		goto try_find_index;
	}

	ptr = dict_accept(ptr, "DELETE", &success);

	if (!success) {
		ptr = dict_accept(ptr, "UPDATE", &success);

		if (!success) {
			dict_foreign_free(foreign);
		
			dict_foreign_report_syntax_err(name,
						start_of_latest_foreign, ptr);
			return(DB_CANNOT_ADD_CONSTRAINT);
		}

		is_on_delete = FALSE;
		n_on_updates++;
	} else {
		is_on_delete = TRUE;
		n_on_deletes++;
	}

	ptr = dict_accept(ptr, "RESTRICT", &success);

	if (success) {
		goto scan_on_conditions;
	}

	ptr = dict_accept(ptr, "CASCADE", &success);

	if (success) {
		if (is_on_delete) {
			foreign->type |= DICT_FOREIGN_ON_DELETE_CASCADE;
		} else {
			foreign->type |= DICT_FOREIGN_ON_UPDATE_CASCADE;
		}

		goto scan_on_conditions;
	}

	ptr = dict_accept(ptr, "NO", &success);

	if (success) {
		ptr = dict_accept(ptr, "ACTION", &success);

		if (!success) {
			dict_foreign_free(foreign);
			dict_foreign_report_syntax_err(name,
					start_of_latest_foreign, ptr);
		
			return(DB_CANNOT_ADD_CONSTRAINT);
		}

		if (is_on_delete) {
			foreign->type |= DICT_FOREIGN_ON_DELETE_NO_ACTION;
		} else {
			foreign->type |= DICT_FOREIGN_ON_UPDATE_NO_ACTION;
		}

		goto scan_on_conditions;
	}

	ptr = dict_accept(ptr, "SET", &success);

	if (!success) {
		dict_foreign_free(foreign);
		dict_foreign_report_syntax_err(name, start_of_latest_foreign,
									ptr);
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	ptr = dict_accept(ptr, "NULL", &success);

	if (!success) {
		dict_foreign_free(foreign);
		dict_foreign_report_syntax_err(name, start_of_latest_foreign,
									ptr);
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	for (j = 0; j < foreign->n_fields; j++) {
		if ((dict_index_get_nth_type(
				foreign->foreign_index, j)->prtype)
				& DATA_NOT_NULL) {

			/* It is not sensible to define SET NULL
			if the column is not allowed to be NULL! */

			dict_foreign_free(foreign);

			mutex_enter(&dict_foreign_err_mutex);
			dict_foreign_error_report_low(ef, name);
			fprintf(ef, "%s:\n"
		"You have defined a SET NULL condition though some of the\n"
		"columns are defined as NOT NULL.\n", start_of_latest_foreign);
			mutex_exit(&dict_foreign_err_mutex);

			return(DB_CANNOT_ADD_CONSTRAINT);
		}
	}

	if (is_on_delete) {
		foreign->type |= DICT_FOREIGN_ON_DELETE_SET_NULL;
	} else {
		foreign->type |= DICT_FOREIGN_ON_UPDATE_SET_NULL;
	}
	
	goto scan_on_conditions;

try_find_index:
	if (n_on_deletes > 1 || n_on_updates > 1) {
		/* It is an error to define more than 1 action */
		
		dict_foreign_free(foreign);

		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, name);
		fprintf(ef, "%s:\n"
"You have twice an ON DELETE clause or twice an ON UPDATE clause.\n",
			start_of_latest_foreign);
		mutex_exit(&dict_foreign_err_mutex);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Try to find an index which contains the columns as the first fields
	and in the right order, and the types are the same as in
	foreign->foreign_index */

	if (referenced_table) {
		index = dict_foreign_find_index(referenced_table,
			column_names, i, foreign->foreign_index, TRUE);
		if (!index) {
			dict_foreign_free(foreign);
			mutex_enter(&dict_foreign_err_mutex);
			dict_foreign_error_report_low(ef, name);
			fprintf(ef, "%s:\n"
"Cannot find an index in the referenced table where the\n"
"referenced columns appear as the first columns, or column types\n"
"in the table and the referenced table do not match for constraint.\n"
"Note that the internal storage type of ENUM and SET changed in\n"
"tables created with >= InnoDB-4.1.12, and such columns in old tables\n"
"cannot be referenced by such columns in new tables.\n"
"See http://dev.mysql.com/doc/mysql/en/InnoDB_foreign_key_constraints.html\n"
"for correct foreign key definition.\n",
				start_of_latest_foreign);
			mutex_exit(&dict_foreign_err_mutex);

			return(DB_CANNOT_ADD_CONSTRAINT);
		}
	} else {
		ut_a(trx->check_foreigns == FALSE);
		index = NULL;
	}

	foreign->referenced_index = index;
	foreign->referenced_table = referenced_table;

	foreign->referenced_table_name = mem_heap_strdup(foreign->heap,
						referenced_table_name);
					
	foreign->referenced_col_names = mem_heap_alloc(foreign->heap,
							i * sizeof(void*));
	for (i = 0; i < foreign->n_fields; i++) {
		foreign->referenced_col_names[i]
			= mem_heap_strdup(foreign->heap, column_names[i]);
	}

	/* We found an ok constraint definition: add to the lists */
	
	UT_LIST_ADD_LAST(foreign_list, table->foreign_list, foreign);

	if (referenced_table) {
		UT_LIST_ADD_LAST(referenced_list,
					referenced_table->referenced_list,
								foreign);
	}

	goto loop;
}

/*************************************************************************
Scans a table create SQL string and adds to the data dictionary the foreign
key constraints declared in the string. This function should be called after
the indexes for a table have been created. Each foreign key constraint must
be accompanied with indexes in both participating tables. The indexes are
allowed to contain more fields than mentioned in the constraint. */

ulint
dict_create_foreign_constraints(
/*============================*/
					/* out: error code or DB_SUCCESS */
	trx_t*		trx,		/* in: transaction */
	const char*	sql_string,	/* in: table create statement where
					foreign keys are declared like:
					FOREIGN KEY (a, b) REFERENCES
					table2(c, d), table2 can be written
					also with the database
					name before it: test.table2; the
					default database id the database of
					parameter name */
	const char*	name,		/* in: table full name in the
					normalized form
					database_name/table_name */
	ibool		reject_fks)	/* in: if TRUE, fail with error
					code DB_CANNOT_ADD_CONSTRAINT if
					any foreign keys are found. */
{
	char*		str;
	ulint		err;
	mem_heap_t*	heap;

	str = dict_strip_comments(sql_string);
	heap = mem_heap_create(10000);

	err = dict_create_foreign_constraints_low(trx, heap, str, name,
		reject_fks);

	mem_heap_free(heap);
	mem_free(str);

	return(err);	
}

/**************************************************************************
Parses the CONSTRAINT id's to be dropped in an ALTER TABLE statement. */

ulint
dict_foreign_parse_drop_constraints(
/*================================*/
						/* out: DB_SUCCESS or
						DB_CANNOT_DROP_CONSTRAINT if
						syntax error or the constraint
						id does not match */
	mem_heap_t*	heap,			/* in: heap from which we can
						allocate memory */
	trx_t*		trx,			/* in: transaction */
	dict_table_t*	table,			/* in: table */
	ulint*		n,			/* out: number of constraints
						to drop */
	const char***	constraints_to_drop)	/* out: id's of the
						constraints to drop */
{
	dict_foreign_t*	foreign;
	ibool		success;
	char*		str;
	const char*	ptr;
	const char*	id;
	FILE*		ef	= dict_foreign_err_file;
	
	*n = 0;

	*constraints_to_drop = mem_heap_alloc(heap, 1000 * sizeof(char*));

	str = dict_strip_comments(*(trx->mysql_query_str));
	ptr = str;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
loop:
	ptr = dict_scan_to(ptr, "DROP");

	if (*ptr == '\0') {
		mem_free(str);
		
		return(DB_SUCCESS);
	}

	ptr = dict_accept(ptr, "DROP", &success);

	if (!isspace(*ptr)) {

	        goto loop;
	}

	ptr = dict_accept(ptr, "FOREIGN", &success);
	
	if (!success) {

	        goto loop;
	}

	ptr = dict_accept(ptr, "KEY", &success);

	if (!success) {

		goto syntax_error;
	}

	ptr = dict_scan_id(ptr, heap, &id, TRUE);

	if (id == NULL) {

		goto syntax_error;
	}

	ut_a(*n < 1000);
	(*constraints_to_drop)[*n] = id;
	(*n)++;
	
	/* Look for the given constraint id */

	foreign = UT_LIST_GET_FIRST(table->foreign_list);

	while (foreign != NULL) {
		if (0 == strcmp(foreign->id, id)
		    || (strchr(foreign->id, '/')
			&& 0 == strcmp(id,
					dict_remove_db_name(foreign->id)))) {
			/* Found */
			break;
		}
		
		foreign = UT_LIST_GET_NEXT(foreign_list, foreign);
	}

	if (foreign == NULL) {
		mutex_enter(&dict_foreign_err_mutex);
		rewind(ef);
		ut_print_timestamp(ef);
		fputs(
	" Error in dropping of a foreign key constraint of table ", ef);
		ut_print_name(ef, NULL, table->name);
		fputs(",\n"
			"in SQL command\n", ef);
		fputs(str, ef);
		fputs("\nCannot find a constraint with the given id ", ef);
		ut_print_name(ef, NULL, id);
		fputs(".\n", ef);
		mutex_exit(&dict_foreign_err_mutex);

		mem_free(str);

		return(DB_CANNOT_DROP_CONSTRAINT);
	}

	goto loop;	

syntax_error:
	mutex_enter(&dict_foreign_err_mutex);
	rewind(ef);
	ut_print_timestamp(ef);
	fputs(
	" Syntax error in dropping of a foreign key constraint of table ", ef);
	ut_print_name(ef, NULL, table->name);
	fprintf(ef, ",\n"
		"close to:\n%s\n in SQL command\n%s\n", ptr, str);
	mutex_exit(&dict_foreign_err_mutex);

	mem_free(str);

	return(DB_CANNOT_DROP_CONSTRAINT);
}

/*==================== END OF FOREIGN KEY PROCESSING ====================*/

/**************************************************************************
Returns an index object if it is found in the dictionary cache. */

dict_index_t*
dict_index_get_if_in_cache(
/*=======================*/
				/* out: index, NULL if not found */
	dulint	index_id)	/* in: index id */
{
	dict_table_t*	table;
	dict_index_t*	index;

	if (dict_sys == NULL) {
		return(NULL);
	}

	mutex_enter(&(dict_sys->mutex));
	
	table = UT_LIST_GET_FIRST(dict_sys->table_LRU);

	while (table) {
		index = UT_LIST_GET_FIRST(table->indexes);

		while (index) {
			if (0 == ut_dulint_cmp(index->id, index_id)) {

				goto found;
			}

			index = UT_LIST_GET_NEXT(indexes, index);
		}

		table = UT_LIST_GET_NEXT(table_LRU, table);
	}

	index = NULL;
found:
	mutex_exit(&(dict_sys->mutex));

	return(index);
}

/**************************************************************************
Creates an index tree struct. */

dict_tree_t*
dict_tree_create(
/*=============*/
				/* out, own: created tree */
	dict_index_t*	index,	/* in: the index for which to create: in the
				case of a mixed tree, this should be the
				index of the cluster object */
	ulint		page_no)/* in: root page number of the index */
{
	dict_tree_t*	tree;

	tree = mem_alloc(sizeof(dict_tree_t));

	/* Inherit info from the index */

	tree->type = index->type;
	tree->space = index->space;
	tree->page = page_no;

	tree->id = index->id;
	
	UT_LIST_INIT(tree->tree_indexes);

	tree->magic_n = DICT_TREE_MAGIC_N;

	rw_lock_create(&(tree->lock));

	rw_lock_set_level(&(tree->lock), SYNC_INDEX_TREE);

	return(tree);
}

/**************************************************************************
Frees an index tree struct. */

void
dict_tree_free(
/*===========*/
	dict_tree_t*	tree)	/* in, own: index tree */
{
	ut_a(tree);
	ut_ad(tree->magic_n == DICT_TREE_MAGIC_N);

	rw_lock_free(&(tree->lock));
	mem_free(tree);
}

/**************************************************************************
In an index tree, finds the index corresponding to a record in the tree. */
UNIV_INLINE
dict_index_t*
dict_tree_find_index_low(
/*=====================*/
				/* out: index */
	dict_tree_t*	tree,	/* in: index tree */
	rec_t*		rec)	/* in: record for which to find correct
				index */
{
	dict_index_t*	index;
	dict_table_t*	table;
	dulint		mix_id;
	ulint		len;
	
	index = UT_LIST_GET_FIRST(tree->tree_indexes);
	ut_ad(index);
	table = index->table;
	
	if ((index->type & DICT_CLUSTERED)
			&& UNIV_UNLIKELY(table->type != DICT_TABLE_ORDINARY)) {

		/* Get the mix id of the record */
		ut_a(!table->comp);

		mix_id = mach_dulint_read_compressed(
			rec_get_nth_field_old(rec, table->mix_len, &len));

		while (ut_dulint_cmp(table->mix_id, mix_id) != 0) {

			index = UT_LIST_GET_NEXT(tree_indexes, index);
			table = index->table;
			ut_ad(index);
		}
	}

	return(index);
}

/**************************************************************************
In an index tree, finds the index corresponding to a record in the tree. */

dict_index_t*
dict_tree_find_index(
/*=================*/
				/* out: index */
	dict_tree_t*	tree,	/* in: index tree */
	rec_t*		rec)	/* in: record for which to find correct
				index */
{
	dict_index_t*	index;
	
	index = dict_tree_find_index_low(tree, rec);
	
	return(index);
}

/**************************************************************************
In an index tree, finds the index corresponding to a dtuple which is used
in a search to a tree. */

dict_index_t*
dict_tree_find_index_for_tuple(
/*===========================*/
				/* out: index; NULL if the tuple does not
				contain the mix id field in a mixed tree */
	dict_tree_t*	tree,	/* in: index tree */
	dtuple_t*	tuple)	/* in: tuple for which to find index */
{
	dict_index_t*	index;
	dict_table_t*	table;
	dulint		mix_id;

	ut_ad(dtuple_check_typed(tuple));
	
	if (UT_LIST_GET_LEN(tree->tree_indexes) == 1) {

		return(UT_LIST_GET_FIRST(tree->tree_indexes));
	}

	index = UT_LIST_GET_FIRST(tree->tree_indexes);
	ut_ad(index);
	table = index->table;

	if (dtuple_get_n_fields(tuple) <= table->mix_len) {

		return(NULL);
	}

	/* Get the mix id of the record */

	mix_id = mach_dulint_read_compressed(
			dfield_get_data(
				dtuple_get_nth_field(tuple, table->mix_len)));

	while (ut_dulint_cmp(table->mix_id, mix_id) != 0) {

		index = UT_LIST_GET_NEXT(tree_indexes, index);
		table = index->table;
		ut_ad(index);
	}

	return(index);
}

/***********************************************************************
Checks if a table which is a mixed cluster member owns a record. */

ibool
dict_is_mixed_table_rec(
/*====================*/
				/* out: TRUE if the record belongs to this
				table */
	dict_table_t*	table,	/* in: table in a mixed cluster */
	rec_t*		rec)	/* in: user record in the clustered index */
{
	byte*	mix_id_field;
	ulint	len;

	ut_ad(!table->comp);

	mix_id_field = rec_get_nth_field_old(rec,
					table->mix_len, &len);

	return(len == table->mix_id_len
		&& !ut_memcmp(table->mix_id_buf, mix_id_field, len));
}

/**************************************************************************
Checks that a tuple has n_fields_cmp value in a sensible range, so that
no comparison can occur with the page number field in a node pointer. */

ibool
dict_tree_check_search_tuple(
/*=========================*/
				/* out: TRUE if ok */
	dict_tree_t*	tree,	/* in: index tree */
	dtuple_t*	tuple)	/* in: tuple used in a search */
{
	dict_index_t*	index;

	index = dict_tree_find_index_for_tuple(tree, tuple);

	if (index == NULL) {

		return(TRUE);
	}

	ut_a(dtuple_get_n_fields_cmp(tuple)
				<= dict_index_get_n_unique_in_tree(index));
	return(TRUE);
}

/**************************************************************************
Builds a node pointer out of a physical record and a page number. */

dtuple_t*
dict_tree_build_node_ptr(
/*=====================*/
				/* out, own: node pointer */
	dict_tree_t*	tree,	/* in: index tree */
	rec_t*		rec,	/* in: record for which to build node
				pointer */
	ulint		page_no,/* in: page number to put in node pointer */
	mem_heap_t*	heap,	/* in: memory heap where pointer created */
	ulint           level)  /* in: level of rec in tree: 0 means leaf
				level */
{
	dtuple_t*	tuple;
	dict_index_t*	ind;
	dfield_t*	field;
	byte*		buf;
	ulint		n_unique;

	ind = dict_tree_find_index_low(tree, rec);
	
	if (UNIV_UNLIKELY(tree->type & DICT_UNIVERSAL)) {
		/* In a universal index tree, we take the whole record as
		the node pointer if the reord is on the leaf level,
		on non-leaf levels we remove the last field, which
		contains the page number of the child page */

		ut_a(!ind->table->comp);
		n_unique = rec_get_n_fields_old(rec);

		if (level > 0) {
		        ut_a(n_unique > 1);
		        n_unique--;
		}
	} else {	
		n_unique = dict_index_get_n_unique_in_tree(ind);
	}

	tuple = dtuple_create(heap, n_unique + 1);

	/* When searching in the tree for the node pointer, we must not do
	comparison on the last field, the page number field, as on upper
	levels in the tree there may be identical node pointers with a
	different page number; therefore, we set the n_fields_cmp to one
	less: */
	
	dtuple_set_n_fields_cmp(tuple, n_unique);

	dict_index_copy_types(tuple, ind, n_unique);
	
	buf = mem_heap_alloc(heap, 4);

	mach_write_to_4(buf, page_no);
	
	field = dtuple_get_nth_field(tuple, n_unique);
	dfield_set_data(field, buf, 4);

	dtype_set(dfield_get_type(field), DATA_SYS_CHILD, DATA_NOT_NULL, 4, 0);

	rec_copy_prefix_to_dtuple(tuple, rec, ind, n_unique, heap);
	dtuple_set_info_bits(tuple, dtuple_get_info_bits(tuple) |
					REC_STATUS_NODE_PTR);

	ut_ad(dtuple_check_typed(tuple));

	return(tuple);
}	
	
/**************************************************************************
Copies an initial segment of a physical record, long enough to specify an
index entry uniquely. */

rec_t*
dict_tree_copy_rec_order_prefix(
/*============================*/
				/* out: pointer to the prefix record */
	dict_tree_t*	tree,	/* in: index tree */
	rec_t*		rec,	/* in: record for which to copy prefix */
	ulint*		n_fields,/* out: number of fields copied */
	byte**		buf,	/* in/out: memory buffer for the copied prefix,
				or NULL */
	ulint*		buf_size)/* in/out: buffer size */
{
	dict_index_t*	index;
	ulint		n;

	UNIV_PREFETCH_R(rec);
	index = dict_tree_find_index_low(tree, rec);

	if (UNIV_UNLIKELY(tree->type & DICT_UNIVERSAL)) {
		ut_a(!index->table->comp);
		n = rec_get_n_fields_old(rec);
	} else {
		n = dict_index_get_n_unique_in_tree(index);
	}

	*n_fields = n;
	return(rec_copy_prefix_to_buf(rec, index, n, buf, buf_size));
}

/**************************************************************************
Builds a typed data tuple out of a physical record. */

dtuple_t*
dict_tree_build_data_tuple(
/*=======================*/
				/* out, own: data tuple */
	dict_tree_t*	tree,	/* in: index tree */
	rec_t*		rec,	/* in: record for which to build data tuple */
	ulint		n_fields,/* in: number of data fields */
	mem_heap_t*	heap)	/* in: memory heap where tuple created */
{
	dtuple_t*	tuple;
	dict_index_t*	ind;

	ind = dict_tree_find_index_low(tree, rec);

	ut_ad(ind->table->comp || n_fields <= rec_get_n_fields_old(rec));
	
	tuple = dtuple_create(heap, n_fields); 

	dict_index_copy_types(tuple, ind, n_fields);

	rec_copy_prefix_to_dtuple(tuple, rec, ind, n_fields, heap);

	ut_ad(dtuple_check_typed(tuple));

	return(tuple);
}	
	
/*************************************************************************
Calculates the minimum record length in an index. */

ulint
dict_index_calc_min_rec_len(
/*========================*/
	dict_index_t*	index)	/* in: index */
{
	ulint	sum	= 0;
	ulint	i;

	if (UNIV_LIKELY(index->table->comp)) {
		ulint nullable = 0;
		sum = REC_N_NEW_EXTRA_BYTES;
		for (i = 0; i < dict_index_get_n_fields(index); i++) {
			dtype_t*t = dict_index_get_nth_type(index, i);
			ulint	size = dtype_get_fixed_size(t);
			sum += size;
			if (!size) {
				size = dtype_get_len(t);
				sum += size < 128 ? 1 : 2;
			}
			if (!(dtype_get_prtype(t) & DATA_NOT_NULL))
				nullable++;
		}

		/* round the NULL flags up to full bytes */
		sum += (nullable + 7) / 8;

		return(sum);
	}

	for (i = 0; i < dict_index_get_n_fields(index); i++) {
		sum += dtype_get_fixed_size(dict_index_get_nth_type(index, i));
	}

	if (sum > 127) {
		sum += 2 * dict_index_get_n_fields(index);
	} else {
		sum += dict_index_get_n_fields(index);
	}

	sum += REC_N_OLD_EXTRA_BYTES;

	return(sum);
}

/*************************************************************************
Calculates new estimates for table and index statistics. The statistics
are used in query optimization. */

void
dict_update_statistics_low(
/*=======================*/
	dict_table_t*	table,		/* in: table */
	ibool		has_dict_mutex __attribute__((unused)))
                                        /* in: TRUE if the caller has the
					dictionary mutex */	
{
	dict_index_t*	index;
	ulint		size;
	ulint		sum_of_index_sizes	= 0;

	if (table->ibd_file_missing) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"  InnoDB: cannot calculate statistics for table %s\n"
"InnoDB: because the .ibd file is missing.  For help, please refer to\n"
"InnoDB: "
"http://dev.mysql.com/doc/mysql/en/InnoDB_troubleshooting_datadict.html\n",
			table->name);

		return;
	}

	/* If we have set a high innodb_force_recovery level, do not calculate
	statistics, as a badly corrupted index can cause a crash in it. */

	if (srv_force_recovery >= SRV_FORCE_NO_IBUF_MERGE) {

		return;
	}

	/* Find out the sizes of the indexes and how many different values
	for the key they approximately have */

	index = dict_table_get_first_index(table);	

	if (index == NULL) {
		/* Table definition is corrupt */
	
		return;
	}

	while (index) {
		size = btr_get_size(index, BTR_TOTAL_SIZE);

		index->stat_index_size = size;

		sum_of_index_sizes += size;

		size = btr_get_size(index, BTR_N_LEAF_PAGES);

		if (size == 0) {
			/* The root node of the tree is a leaf */
			size = 1;
		}

		index->stat_n_leaf_pages = size;
		
		btr_estimate_number_of_different_key_vals(index);

		index = dict_table_get_next_index(index);
	}

	index = dict_table_get_first_index(table);

	table->stat_n_rows = index->stat_n_diff_key_vals[
					dict_index_get_n_unique(index)];

	table->stat_clustered_index_size = index->stat_index_size;

	table->stat_sum_of_other_index_sizes = sum_of_index_sizes
						- index->stat_index_size;

	table->stat_initialized = TRUE;

        table->stat_modified_counter = 0;
}

/*************************************************************************
Calculates new estimates for table and index statistics. The statistics
are used in query optimization. */

void
dict_update_statistics(
/*===================*/
	dict_table_t*	table)	/* in: table */
{
	dict_update_statistics_low(table, FALSE);
}

/**************************************************************************
Prints info of a foreign key constraint. */
static
void
dict_foreign_print_low(
/*===================*/
	dict_foreign_t*	foreign)	/* in: foreign key constraint */
{
	ulint	i;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	fprintf(stderr, "  FOREIGN KEY CONSTRAINT %s: %s (",
		foreign->id, foreign->foreign_table_name);

	for (i = 0; i < foreign->n_fields; i++) {
		fprintf(stderr, " %s", foreign->foreign_col_names[i]);
	}

	fprintf(stderr, " )\n"
		"             REFERENCES %s (",
		foreign->referenced_table_name);
	
	for (i = 0; i < foreign->n_fields; i++) {
		fprintf(stderr, " %s", foreign->referenced_col_names[i]);
	}

	fputs(" )\n", stderr);
}

/**************************************************************************
Prints a table data. */

void
dict_table_print(
/*=============*/
	dict_table_t*	table)	/* in: table */
{
	mutex_enter(&(dict_sys->mutex));
	dict_table_print_low(table);
	mutex_exit(&(dict_sys->mutex));
}

/**************************************************************************
Prints a table data when we know the table name. */

void
dict_table_print_by_name(
/*=====================*/
	const char*	name)
{
	dict_table_t*	table;

	mutex_enter(&(dict_sys->mutex));

	table = dict_table_get_low(name);

	ut_a(table);
	
	dict_table_print_low(table);
	mutex_exit(&(dict_sys->mutex));
}

/**************************************************************************
Prints a table data. */

void
dict_table_print_low(
/*=================*/
	dict_table_t*	table)	/* in: table */
{
	dict_index_t*	index;
	dict_foreign_t*	foreign;
	ulint		i;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	dict_update_statistics_low(table, TRUE);
	
	fprintf(stderr,
"--------------------------------------\n"
"TABLE: name %s, id %lu %lu, columns %lu, indexes %lu, appr.rows %lu\n"
"  COLUMNS: ",
			table->name,
			(ulong) ut_dulint_get_high(table->id),
			(ulong) ut_dulint_get_low(table->id),
			(ulong) table->n_cols,
		        (ulong) UT_LIST_GET_LEN(table->indexes),
			(ulong) table->stat_n_rows);

	for (i = 0; i < table->n_cols - 1; i++) {
		dict_col_print_low(dict_table_get_nth_col(table, i));
		fputs("; ", stderr);
	}

	putc('\n', stderr);

	index = UT_LIST_GET_FIRST(table->indexes);

	while (index != NULL) {
		dict_index_print_low(index);
		index = UT_LIST_GET_NEXT(indexes, index);
	}

	foreign = UT_LIST_GET_FIRST(table->foreign_list);

	while (foreign != NULL) {
		dict_foreign_print_low(foreign);
		foreign = UT_LIST_GET_NEXT(foreign_list, foreign);
	}

	foreign = UT_LIST_GET_FIRST(table->referenced_list);

	while (foreign != NULL) {
		dict_foreign_print_low(foreign);
		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}
}

/**************************************************************************
Prints a column data. */
static
void
dict_col_print_low(
/*===============*/
	dict_col_t*	col)	/* in: column */
{
	dtype_t*	type;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	type = dict_col_get_type(col);
	fprintf(stderr, "%s: ", col->name);

	dtype_print(type);
}

/**************************************************************************
Prints an index data. */
static
void
dict_index_print_low(
/*=================*/
	dict_index_t*	index)	/* in: index */
{
	dict_tree_t*	tree;
	ib_longlong	n_vals;
	ulint		i;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */

	tree = index->tree;

	if (index->n_user_defined_cols > 0) {
		n_vals = index->stat_n_diff_key_vals[
					index->n_user_defined_cols];
	} else {
		n_vals = index->stat_n_diff_key_vals[1];
	}

	fprintf(stderr,
		"  INDEX: name %s, id %lu %lu, fields %lu/%lu, type %lu\n"
		"   root page %lu, appr.key vals %lu,"
		" leaf pages %lu, size pages %lu\n"
		"   FIELDS: ",
		index->name,
		(ulong) ut_dulint_get_high(tree->id),
		(ulong) ut_dulint_get_low(tree->id),
		(ulong) index->n_user_defined_cols,
		(ulong) index->n_fields, (ulong) index->type,
		(ulong) tree->page,
		(ulong) n_vals,
		(ulong) index->stat_n_leaf_pages,
		(ulong) index->stat_index_size);
			
	for (i = 0; i < index->n_fields; i++) {
		dict_field_print_low(dict_index_get_nth_field(index, i));
	}

	putc('\n', stderr);

#ifdef UNIV_BTR_PRINT
	btr_print_size(tree);

	btr_print_tree(tree, 7);
#endif /* UNIV_BTR_PRINT */
}

/**************************************************************************
Prints a field data. */
static
void
dict_field_print_low(
/*=================*/
	dict_field_t*	field)	/* in: field */
{
#ifdef UNIV_SYNC_DEBUG
	ut_ad(mutex_own(&(dict_sys->mutex)));
#endif /* UNIV_SYNC_DEBUG */
	fprintf(stderr, " %s", field->name);

	if (field->prefix_len != 0) {
		fprintf(stderr, "(%lu)", (ulong) field->prefix_len);
	}
}

/**************************************************************************
Outputs info on a foreign key of a table in a format suitable for
CREATE TABLE. */

void
dict_print_info_on_foreign_key_in_create_format(
/*============================================*/
	FILE*		file,		/* in: file where to print */
	trx_t*		trx,		/* in: transaction */
	dict_foreign_t*	foreign,	/* in: foreign key constraint */
	ibool		add_newline)	/* in: whether to add a newline */
{
	const char*	stripped_id;
	ulint	i;
	
	if (strchr(foreign->id, '/')) {
		/* Strip the preceding database name from the constraint id */
		stripped_id = foreign->id + 1
				+ dict_get_db_name_len(foreign->id);
	} else {
		stripped_id = foreign->id;
	}

	putc(',', file);
	
	if (add_newline) {
		/* SHOW CREATE TABLE wants constraints each printed nicely
		on its own line, while error messages want no newlines
		inserted. */
		fputs("\n ", file);
	}
	
	fputs(" CONSTRAINT ", file);
	ut_print_name(file, trx, stripped_id);
	fputs(" FOREIGN KEY (", file);

	for (i = 0;;) {
		ut_print_name(file, trx, foreign->foreign_col_names[i]);
		if (++i < foreign->n_fields) {
			fputs(", ", file);
	        } else {
			break;
		}
	}

	fputs(") REFERENCES ", file);

	if (dict_tables_have_same_db(foreign->foreign_table_name,
					foreign->referenced_table_name)) {
		/* Do not print the database name of the referenced table */
		ut_print_name(file, trx, dict_remove_db_name(
					foreign->referenced_table_name));
	} else {
		/* Look for the '/' in the table name */

		i = 0;
		while (foreign->referenced_table_name[i] != '/') {
			i++;
		}

		ut_print_namel(file, trx, foreign->referenced_table_name, i);
		putc('.', file);
		ut_print_name(file, trx,
				foreign->referenced_table_name + i + 1);
	}

	putc(' ', file);
	putc('(', file);

	for (i = 0;;) {
		ut_print_name(file, trx, foreign->referenced_col_names[i]);
		if (++i < foreign->n_fields) {
			fputs(", ", file);
		} else {
			break;
		}
	}

	putc(')', file);

	if (foreign->type & DICT_FOREIGN_ON_DELETE_CASCADE) {
		fputs(" ON DELETE CASCADE", file);
	}
	
	if (foreign->type & DICT_FOREIGN_ON_DELETE_SET_NULL) {
		fputs(" ON DELETE SET NULL", file);
	}

	if (foreign->type & DICT_FOREIGN_ON_DELETE_NO_ACTION) {
		fputs(" ON DELETE NO ACTION", file);
	}

	if (foreign->type & DICT_FOREIGN_ON_UPDATE_CASCADE) {
		fputs(" ON UPDATE CASCADE", file);
	}
	
	if (foreign->type & DICT_FOREIGN_ON_UPDATE_SET_NULL) {
		fputs(" ON UPDATE SET NULL", file);
	}

	if (foreign->type & DICT_FOREIGN_ON_UPDATE_NO_ACTION) {
		fputs(" ON UPDATE NO ACTION", file);
	}
}

/**************************************************************************
Outputs info on foreign keys of a table. */

void
dict_print_info_on_foreign_keys(
/*============================*/
	ibool		create_table_format, /* in: if TRUE then print in
				a format suitable to be inserted into
				a CREATE TABLE, otherwise in the format
				of SHOW TABLE STATUS */
	FILE*		file,	/* in: file where to print */
	trx_t*		trx,	/* in: transaction */
	dict_table_t*	table)	/* in: table */
{
	dict_foreign_t*	foreign;

	mutex_enter(&(dict_sys->mutex));

	foreign = UT_LIST_GET_FIRST(table->foreign_list);

	if (foreign == NULL) {
		mutex_exit(&(dict_sys->mutex));

		return;
	}

	while (foreign != NULL) {
		if (create_table_format) {
			dict_print_info_on_foreign_key_in_create_format(
						file, trx, foreign, TRUE);
		} else {
			ulint	i;
			fputs("; (", file);

			for (i = 0; i < foreign->n_fields; i++) {
				if (i) {
					putc(' ', file);
				}

				ut_print_name(file, trx,
					foreign->foreign_col_names[i]);
			}

			fputs(") REFER ", file);
			ut_print_name(file, trx,
					foreign->referenced_table_name);
			putc('(', file);

			for (i = 0; i < foreign->n_fields; i++) {
				if (i) {
					putc(' ', file);
				}
				ut_print_name(file, trx,
					foreign->referenced_col_names[i]);
			}

			putc(')', file);

			if (foreign->type == DICT_FOREIGN_ON_DELETE_CASCADE) {
				fputs(" ON DELETE CASCADE", file);
			}
	
			if (foreign->type == DICT_FOREIGN_ON_DELETE_SET_NULL) {
				fputs(" ON DELETE SET NULL", file);
			}

			if (foreign->type & DICT_FOREIGN_ON_DELETE_NO_ACTION) {
				fputs(" ON DELETE NO ACTION", file);
			}

			if (foreign->type & DICT_FOREIGN_ON_UPDATE_CASCADE) {
				fputs(" ON UPDATE CASCADE", file);
			}
	
			if (foreign->type & DICT_FOREIGN_ON_UPDATE_SET_NULL) {
				fputs(" ON UPDATE SET NULL", file);
			}

			if (foreign->type & DICT_FOREIGN_ON_UPDATE_NO_ACTION) {
				fputs(" ON UPDATE NO ACTION", file);
			}
		}

		foreign = UT_LIST_GET_NEXT(foreign_list, foreign);
	}

	mutex_exit(&(dict_sys->mutex));
}

/************************************************************************
Displays the names of the index and the table. */
void
dict_index_name_print(
/*==================*/
	FILE*			file,	/* in: output stream */
	trx_t*			trx,	/* in: transaction */
	const dict_index_t*	index)	/* in: index to print */
{
	fputs("index ", file);
	ut_print_name(file, trx, index->name);
	fputs(" of table ", file);
	ut_print_name(file, trx, index->table_name);
}
