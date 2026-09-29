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
    /*
     * 1 = a long-lived server that is NOT bound to this process: no watchdog,
     * no stop at exit, never reclaimed by a sweep ("durable": true in its
     * owner.json).  Only rpgl_stop / rpgl_stop_dir stop it.  Its auto dir is
     * <temp_root>/rpgldur_XXXXXX, outside anything that scans rpgl_*.
     * Appended last so the fields above keep their offsets.
     */
    int         durable;
    /* Where auto data dirs (rpgl_XXXXXX) go and what the start-up sweep scans.
     * NULL = $RUSTYPGLITE_TMPDIR, else /tmp. */
    const char *temp_root;
} rpgl_options;

/* ---- Sweep result ---- */
typedef struct rpgl_sweep_result {
    int examined;   /* rpgl_* dirs looked at */
    int reclaimed;  /* owner and watchdog dead: server stopped, dir removed */
    int live;       /* owner (or its watchdog) still running: left alone */
    int durable;    /* an rpgl_* dir whose owner.json says durable: left alone.  Normally
                       0 — durable auto dirs are rpgldur_*, which no sweep examines */
    int legacy;     /* no owner.json (older rustypglite, or mid-start): left alone */
    int skipped;    /* not ours to judge (other user, other PID namespace, unreadable) */
    int failed;     /* owner dead, but the server would not stop or the dir stay removed */
} rpgl_sweep_result;

/* ---- Lifecycle ---- */

/*
 * Start an embedded PostgreSQL instance.
 * - RPGL_ERR_ALREADY, touching nothing, if a server is already running in
 *   opts->data_dir
 * - Sweeps the temp root first (see rpgl_sweep)
 * - Runs initdb if the data directory doesn't exist
 * - Writes owner.json into the data dir: who started it (PID + start time)
 * - Unless durable: binds the server to this process with a watchdog, so it
 *   is stopped and its dir removed however this process ends (SIGKILL too)
 * - Starts postgres listening on a unix socket
 * - Waits until accepting connections
 * - opts can be NULL for all defaults
 * Returns RPGL_OK on success.
 */
int rpgl_start(const rpgl_options *opts, rpgl_instance **out);

/*
 * Connect to an already-running instance by data directory.
 * Reads postmaster.pid to get port/socket info. Does NOT start a server,
 * and rpgl_stop on the result frees the handle without stopping the server.
 * Use this for shared-server-across-workers patterns.
 */
int rpgl_connect_existing(const char *data_dir, rpgl_instance **out);

/*
 * Stop the PostgreSQL instance and free resources.
 * Unless keep_data was set (or data_dir was supplied), the data directory is
 * deleted.  Stops a durable instance too: stopping is always explicit.
 */
int rpgl_stop(rpgl_instance *inst);

/*
 * Free the handle WITHOUT stopping the server.  For a durable instance this
 * leaves it running after the process exits.  A non-durable server stays
 * bound to this process and is still stopped when it exits.
 */
int rpgl_detach(rpgl_instance *inst);

/*
 * Stop the server running in data_dir, by path — for scripts, and for a
 * process other than the owner (`rustypglite stop <dir>`).  Deletes the dir
 * only when its owner.json says rustypglite created it.  RPGL_OK when no
 * server is left running there.
 */
int rpgl_stop_dir(const char *data_dir);

/*
 * Reclaim servers whose owner is dead, under temp_root (NULL = the default,
 * $RUSTYPGLITE_TMPDIR else /tmp).  Only rpgl_* dirs whose owner.json proves
 * the owner (PID + start time) and its watchdog are both gone; never a live
 * owner's dir whatever its age, a durable one, or one without owner.json.
 * rpgl_start runs this itself.  RPGL_ERR_ALREADY if another process is
 * sweeping the same root right now.
 */
int rpgl_sweep(const char *temp_root, rpgl_sweep_result *out);

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
