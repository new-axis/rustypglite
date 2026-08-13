/*
 * pg_shim.h - Embedded PostgreSQL lifecycle manager
 *
 * Manages a real PostgreSQL server process that listens on a unix socket
 * in a temporary directory. Any standard Postgres client (Npgsql, node-pg,
 * psycopg2) can connect using the returned connection string.
 *
 * Usage:
 *   rpgl_instance *pg;
 *   rpgl_start(NULL, &pg);              // initdb + pg_ctl start
 *   const char *cs = rpgl_connstr(pg);  // "host=/tmp/xxx port=5432 dbname=postgres"
 *   // ... use standard postgres client ...
 *   rpgl_stop(pg);                      // pg_ctl stop + cleanup
 */

#ifndef PG_SHIM_H
#define PG_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Error codes ---- */
#define RPGL_OK              0
#define RPGL_ERR_INIT       -1
#define RPGL_ERR_ALREADY    -2
#define RPGL_ERR_START      -3
#define RPGL_ERR_STOP       -4
#define RPGL_ERR_INTERNAL   -5
#define RPGL_ERR_OOM        -6
#define RPGL_ERR_TIMEOUT    -7

/* ---- Instance handle ---- */
typedef struct rpgl_instance rpgl_instance;

/* ---- Options ---- */
typedef struct rpgl_options {
    const char *data_dir;       /* NULL = auto temp directory */
    const char *db_name;        /* NULL = "postgres" */
    int         port;           /* 0 = auto-assign (find free port) */
    int         silent;         /* 1 = suppress postgres log output */
    int         keep_data;      /* 1 = don't delete data_dir on stop */
} rpgl_options;

/* ---- Lifecycle ---- */

/*
 * Start an embedded PostgreSQL instance.
 * - Runs initdb if the data directory doesn't exist
 * - Starts postgres listening on a unix socket
 * - Waits until accepting connections
 * - opts can be NULL for all defaults
 * Returns RPGL_OK on success.
 */
int rpgl_start(const rpgl_options *opts, rpgl_instance **out);

/*
 * Connect to an already-running instance by data directory.
 * Reads postmaster.pid to get port/socket info. Does NOT start a server.
 * Use this for shared-server-across-workers patterns.
 */
int rpgl_connect_existing(const char *data_dir, rpgl_instance **out);

/*
 * Stop the PostgreSQL instance and free resources.
 * Unless keep_data was set, the data directory is deleted.
 */
int rpgl_stop(rpgl_instance *inst);

/* ---- Connection info ---- */

/*
 * Get a libpq-style connection string.
 * e.g. "host=/tmp/rpgl_XXXXXX port=5432 dbname=postgres user=postgres"
 */
const char *rpgl_connection_string(rpgl_instance *inst);

/*
 * Get the unix socket directory path.
 */
const char *rpgl_socket_dir(rpgl_instance *inst);

/*
 * Get the port number.
 */
int rpgl_port(rpgl_instance *inst);

/*
 * Get the data directory path.
 */
const char *rpgl_data_dir(rpgl_instance *inst);

/* ---- Error info ---- */

/*
 * Get the last error message.
 */
const char *rpgl_last_error(rpgl_instance *inst);

/* ---- Utility ---- */

/*
 * Create a new database on this instance.
 * Runs: CREATE DATABASE <name>
 */
int rpgl_create_database(rpgl_instance *inst, const char *name);

/*
 * Execute SQL using psql on this instance.
 * Useful for running migrations/DDL before handing off to a client library.
 */
int rpgl_exec_sql(rpgl_instance *inst, const char *db_name, const char *sql);

/*
 * Execute a SQL file using psql.
 */
int rpgl_exec_file(rpgl_instance *inst, const char *db_name, const char *file_path);

#ifdef __cplusplus
}
#endif

#endif /* PG_SHIM_H */
