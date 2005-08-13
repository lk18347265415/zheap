/*-------------------------------------------------------------------------
 *
 * genfile.c
 *		Functions for direct access to files
 *
 *
 * Copyright (c) 2004-2005, PostgreSQL Global Development Group
 * 
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/genfile.c,v 1.4 2005/08/13 19:02:34 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/memutils.h"


typedef struct 
{
	char	*location;
	DIR		*dirdesc;
} directory_fctx;


/*
 * Validate a path and convert to absolute form.
 *
 * Argument may be absolute or relative to the DataDir (but we only allow
 * absolute paths that match Log_directory).
 */
static char *
check_and_make_absolute(text *arg)
{
	int input_len = VARSIZE(arg) - VARHDRSZ;
	char *filename = palloc(input_len + 1);
	
	memcpy(filename, VARDATA(arg), input_len);
	filename[input_len] = '\0';

	canonicalize_path(filename);	/* filename can change length here */

	/* Disallow ".." in the path */
	if (path_contains_parent_reference(filename))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("reference to parent directory (\"..\") not allowed"))));

	if (is_absolute_path(filename))
	{
		/* The log directory might be outside our datadir, but allow it */
	    if (is_absolute_path(Log_directory) &&
			strncmp(filename, Log_directory, strlen(Log_directory)) == 0 &&
			(filename[strlen(Log_directory)] == '/' ||
			 filename[strlen(Log_directory)] == '\0'))
			return filename;

	    ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("absolute path not allowed"))));
		return NULL;			/* keep compiler quiet */
	}
	else
	{
	    char *absname = palloc(strlen(DataDir) + strlen(filename) + 2);
		sprintf(absname, "%s/%s", DataDir, filename);
		pfree(filename);
		return absname;
	}
}


/*
 * Read a section of a file, returning it as text
 */
Datum
pg_read_file(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_P(0);
	int64		seek_offset = PG_GETARG_INT64(1);
	int64		bytes_to_read = PG_GETARG_INT64(2);
	char 		*buf;
	size_t		nbytes;
	FILE		*file;
	char		*filename;

	if (!superuser())
	    ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to read files"))));

	filename = check_and_make_absolute(filename_t);

	if ((file = AllocateFile(filename, PG_BINARY_R)) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m",
						filename)));

	if (fseeko(file, (off_t) seek_offset,
			   (seek_offset >= 0) ? SEEK_SET : SEEK_END) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in file \"%s\": %m", filename)));

	if (bytes_to_read < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("requested length cannot be negative")));

	/* not sure why anyone thought that int64 length was a good idea */
	if (bytes_to_read > (MaxAllocSize - VARHDRSZ))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("requested length too large")));
	
	buf = palloc((Size) bytes_to_read + VARHDRSZ);

	nbytes = fread(VARDATA(buf), 1, (size_t) bytes_to_read, file);

	if (nbytes < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", filename)));

	VARATT_SIZEP(buf) = nbytes + VARHDRSZ;

	FreeFile(file);
	pfree(filename);

	PG_RETURN_TEXT_P(buf);
}

/*
 * stat a file
 */
Datum
pg_stat_file(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_P(0);
	char		*filename;
	struct stat fst;
	Datum		values[5];
	bool		isnull[5];
	HeapTuple	tuple;
	TupleDesc	tupdesc;

	if (!superuser())
	    ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to get file information"))));

	filename = check_and_make_absolute(filename_t);

	if (stat(filename, &fst) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", filename)));

	/*
	 * This record type had better match the output parameters declared
	 * for me in pg_proc.h (actually, in system_views.sql at the moment).
	 */
	tupdesc = CreateTemplateTupleDesc(5, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1,
					   "length", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2,
					   "atime", TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3,
					   "mtime", TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4,
					   "ctime", TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5,
					   "isdir", BOOLOID, -1, 0);
	BlessTupleDesc(tupdesc);

	values[0] = Int64GetDatum((int64) fst.st_size);
	values[1] = TimestampTzGetDatum(time_t_to_timestamptz(fst.st_atime));
	values[2] = TimestampTzGetDatum(time_t_to_timestamptz(fst.st_mtime));
	values[3] = TimestampTzGetDatum(time_t_to_timestamptz(fst.st_ctime));
	values[4] = BoolGetDatum(fst.st_mode & S_IFDIR);

	memset(isnull, false, sizeof(isnull));

	tuple = heap_form_tuple(tupdesc, values, isnull);

	pfree(filename);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}


/*
 * List a directory (returns the filenames only)
 */
Datum
pg_ls_dir(PG_FUNCTION_ARGS)
{
	FuncCallContext	*funcctx;
	struct dirent	*de;
	directory_fctx	*fctx;

	if (!superuser())
	    ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to get directory listings"))));

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		fctx = palloc(sizeof(directory_fctx));
		fctx->location = check_and_make_absolute(PG_GETARG_TEXT_P(0));

		fctx->dirdesc = AllocateDir(fctx->location);

		if (!fctx->dirdesc)
		    ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open directory \"%s\": %m",
							fctx->location)));

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	fctx = (directory_fctx*) funcctx->user_fctx;

	while ((de = ReadDir(fctx->dirdesc, fctx->location)) != NULL)
	{
		int			len = strlen(de->d_name);
		text		*result;

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
		    continue;

		result = palloc(len + VARHDRSZ);
		VARATT_SIZEP(result) = len + VARHDRSZ;
		memcpy(VARDATA(result), de->d_name, len);

		SRF_RETURN_NEXT(funcctx, PointerGetDatum(result));
	}

	FreeDir(fctx->dirdesc);

	SRF_RETURN_DONE(funcctx);
}
