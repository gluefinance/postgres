/*-------------------------------------------------------------------------
 *
 * dbcommands.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_H
#define DBCOMMANDS_H

#include <nodes/parsenodes.h>

extern void createdb(const CreatedbStmt *stmt);
extern void dropdb(const char *dbname);
extern void AlterDatabaseSet(AlterDatabaseSetStmt *stmt);

#endif   /* DBCOMMANDS_H */
