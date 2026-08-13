/*
 * pg_shim.c - Embedded PostgreSQL lifecycle manager
 *
 * Manages a real postgres server process: initdb → pg_ctl start → unix socket.
 * Any standard Postgres client connects via the returned connection string.
 */

#define _GNU_SOURCE  /* for dladdr */
#include "pg_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <time.h>
#include <dirent.h>
#include <sys/file.h>

/* ---- Instance ---- */

struct rpgl_instance {
    char    *data_dir;
    char    *socket_dir;
    char    *db_name;
    char    *pg_bin_dir;
    char    *conn_string;
    char     error_msg[2048];
    int      port;
    int      owns_data_dir;  /* should we delete it on stop? */
    pid_t    pg_pid;         /* postmaster PID (0 if not running) */
};

/* ---- Instance tracking for cleanup on exit ---- */

#define MAX_INSTANCES 256
static rpgl_instance *g_instances[MAX_INSTANCES];
static int g_instance_count = 0;
static int g_atexit_registered = 0;

static void cleanup_all_instances(void) {
    for (int i = 0; i < g_instance_count; i++) {
        if (g_instances[i]) {
            rpgl_stop(g_instances[i]);
            g_instances[i] = NULL;
        }
    }
    g_instance_count = 0;
}

static void signal_handler(int sig) {
    cleanup_all_instances();
    /* Re-raise to get default behavior (exit with signal status) */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Forward declaration — used by the stale-dir cleanup below. */
static int run_cmd(int silent, const char *argv[]);

/* ---- Startup serialization lock (cross-process) ---- */
/*
 * On fast machines, concurrent rpgl_start() calls race: cleanup_stale_tmpdirs()
 * can delete a freshly-created mkdtemp() directory that has no postmaster.pid
 * yet, causing initdb to fail.  A POSIX advisory lock on a well-known file
 * serialises the cleanup + mkdtemp window across processes.  The lock is held
 * only around cleanup and directory creation, then released before the slow
 * postgres boot — the age guard below (not the lock) is what protects our
 * young data dir from another process's cleanup once we let go.
 */
#define STARTUP_LOCK_PATH "/tmp/rpgl_startup.lock"

static int acquire_startup_lock(void) {
    int fd = open(STARTUP_LOCK_PATH, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return -1;
    /* Block until we hold an exclusive lock */
    if (flock(fd, LOCK_EX) < 0) { close(fd); return -1; }
    return fd;
}

static void release_startup_lock(int fd) {
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

/* ---- Stale-dir detection ---- */

/*
 * A crashed-run dir is only reclaimed once it is BOTH older than this AND has
 * no reachable postgres socket.  The age guard closes the initdb window: a dir
 * that was just mkdtemp()'d has no postmaster.pid yet, but it is seconds old,
 * so it is never mistaken for stale — even by a cleanup running outside our
 * startup-lock scope (a concurrent process, or a different PID namespace).
 */
#define STALE_AGE_SECONDS 600  /* 10 minutes */

/* Try to connect() to a unix socket path.  Returns 1 if a listener accepted. */
static int socket_accepts(const char *sockpath) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);
    return rc == 0;
}

/* Read the port from line 4 of a data dir's postmaster.pid, or -1. */
static int port_from_pidfile(const char *dir) {
    char pidfile[4096];
    snprintf(pidfile, sizeof(pidfile), "%s/postmaster.pid", dir);
    FILE *f = fopen(pidfile, "r");
    if (!f) return -1;
    char line[256];
    int lineno = 0, port = -1;
    while (fgets(line, sizeof(line), f)) {
        if (++lineno == 4) { port = atoi(line); break; }
    }
    fclose(f);
    return port;
}

/*
 * Is a postgres server accepting connections on this data dir's socket?
 *
 * We probe the unix socket rather than `kill -0` the pid: a socket connect()
 * works across PID namespaces and without signal permission, whereas kill -0
 * reports ESRCH/EPERM for a live process in another namespace — which is
 * exactly how a sandboxed sibling used to see every healthy instance as stale
 * and delete its live data dir.  A dir with no postmaster.pid is treated as
 * not-live (it may be mid-initdb; the age guard, not this, protects it).
 */
static int dir_has_live_socket(const char *dir) {
    char pidfile[4096];
    snprintf(pidfile, sizeof(pidfile), "%s/postmaster.pid", dir);
    if (access(pidfile, F_OK) != 0) return 0;

    char sockpath[4096];

    /* Primary: the port named in postmaster.pid (socket == data dir here). */
    int port = port_from_pidfile(dir);
    if (port > 0) {
        snprintf(sockpath, sizeof(sockpath), "%s/.s.PGSQL.%d", dir, port);
        if (access(sockpath, F_OK) == 0 && socket_accepts(sockpath))
            return 1;
    }

    /* Fallback: probe any .s.PGSQL.* socket present in the dir. */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, ".s.PGSQL.", 9) != 0) continue;
            snprintf(sockpath, sizeof(sockpath), "%s/%s", dir, de->d_name);
            if (socket_accepts(sockpath)) { closedir(d); return 1; }
        }
        closedir(d);
    }
    return 0;
}

/*
 * Clean up stale /tmp/rpgl_* directories from previous crashed runs.
 *
 * A directory is reclaimed only when it is BOTH:
 *   1. older than STALE_AGE_SECONDS (mtime guard — never touch a young dir,
 *      pidfile or not; closes the concurrent-initdb window), AND
 *   2. has no reachable postgres socket (liveness via connect(), which is
 *      namespace/permission agnostic — see dir_has_live_socket).
 *
 * Deletion forks /bin/rm directly (no shell) so a hostile directory name in
 * /tmp cannot inject a command.
 */
static void cleanup_stale_tmpdirs(void) {
    DIR *tmp = opendir("/tmp");
    if (!tmp) return;

    time_t now = time(NULL);
    struct dirent *de;
    while ((de = readdir(tmp)) != NULL) {
        if (strncmp(de->d_name, "rpgl_", 5) != 0) continue;

        char path[4096];
        snprintf(path, sizeof(path), "/tmp/%s", de->d_name);

        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;   /* skips rpgl_startup.lock */

        /* Age guard: never delete a dir younger than the threshold. */
        if (now - st.st_mtime < STALE_AGE_SECONDS) continue;

        /* Liveness guard: keep any dir with a reachable server. */
        if (dir_has_live_socket(path)) continue;

        /* Old AND no reachable server → genuinely stale. */
        const char *rm_argv[] = { "/bin/rm", "-rf", path, NULL };
        run_cmd(1, rm_argv);
    }
    closedir(tmp);
}

static void track_instance(rpgl_instance *inst) {
    if (!g_atexit_registered) {
        atexit(cleanup_all_instances);
        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);
        signal(SIGHUP, signal_handler);
        g_atexit_registered = 1;
    }
    if (g_instance_count < MAX_INSTANCES) {
        g_instances[g_instance_count++] = inst;
    }
}

static void untrack_instance(rpgl_instance *inst) {
    for (int i = 0; i < g_instance_count; i++) {
        if (g_instances[i] == inst) {
            g_instances[i] = g_instances[--g_instance_count];
            g_instances[g_instance_count] = NULL;
            return;
        }
    }
}

/* ---- Helpers ---- */

static void set_error(rpgl_instance *inst, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(inst->error_msg, sizeof(inst->error_msg), fmt, ap);
    va_end(ap);
}

/*
 * Find the directory containing this shared library (.so/.dylib).
 * Uses dladdr() to resolve the path from a symbol inside the library.
 */
static char *find_self_dir(void) {
    Dl_info info;
    /* Use a function defined in this file as the lookup symbol */
    if (dladdr((void *)find_self_dir, &info) && info.dli_fname) {
        char *path = realpath(info.dli_fname, NULL);
        if (path) {
            /* Strip the filename to get the directory */
            char *slash = strrchr(path, '/');
            if (slash) *slash = '\0';
            return path;
        }
    }
    return NULL;
}

static char *find_pg_bin_dir(void) {
    char buf[4096];

    /* 1. Explicit env var (always wins) */
    const char *env = getenv("RUSTYPGLITE_PG_DIR");
    if (env) {
        char *bindir = malloc(strlen(env) + 5);
        if (bindir) sprintf(bindir, "%s/bin", env);
        return bindir;
    }

    /* 2. Look for pg/ directory next to the .so file itself.
     *    This is the primary discovery mechanism for packaged installs:
     *      node_modules/rustypglite/native/librustypglite.so
     *      node_modules/rustypglite/native/pg/bin/postgres
     *    or:
     *      runtimes/linux-x64/native/librustypglite.so
     *      runtimes/linux-x64/native/pg/bin/postgres
     */
    char *self_dir = find_self_dir();
    if (self_dir) {
        snprintf(buf, sizeof(buf), "%s/pg/bin", self_dir);
        if (access(buf, F_OK) == 0) {
            free(self_dir);
            return strdup(buf);
        }
        /* Also check one level up (if .so is in lib/ and pg/ is sibling) */
        snprintf(buf, sizeof(buf), "%s/../pg/bin", self_dir);
        if (access(buf, F_OK) == 0) {
            free(self_dir);
            char *resolved = realpath(buf, NULL);
            return resolved ? resolved : strdup(buf);
        }
        free(self_dir);
    }

    /* 3. CARGO_MANIFEST_DIR (Rust development builds) */
    const char *manifest = getenv("CARGO_MANIFEST_DIR");
    if (manifest) {
        snprintf(buf, sizeof(buf), "%s/../rustypglite-sys/pg-install/bin", manifest);
        if (access(buf, F_OK) == 0) return realpath(buf, NULL);
        snprintf(buf, sizeof(buf), "%s/pg-install/bin", manifest);
        if (access(buf, F_OK) == 0) return realpath(buf, NULL);
    }

    /* 4. Common relative paths from cwd (development) */
    const char *candidates[] = {
        "rustypglite-sys/pg-install/bin",
        "../rustypglite-sys/pg-install/bin",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], F_OK) == 0)
            return realpath(candidates[i], NULL);
    }

    /* 5. Try PATH as last resort */
    const char *path = getenv("PATH");
    if (path) {
        char *p = strdup(path);
        char *tok = strtok(p, ":");
        while (tok) {
            snprintf(buf, sizeof(buf), "%s/pg_ctl", tok);
            if (access(buf, X_OK) == 0) {
                char *result = strdup(tok);
                free(p);
                return result;
            }
            tok = strtok(NULL, ":");
        }
        free(p);
    }

    return NULL;
}

static char *find_share_dir(const char *bin_dir) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s/../share/postgresql", bin_dir);
    if (access(buf, F_OK) == 0) return strdup(buf);
    snprintf(buf, sizeof(buf), "%s/../share", bin_dir);
    if (access(buf, F_OK) == 0) return strdup(buf);
    return NULL;
}

/* Run a command, capture exit code. Optionally suppress output. */
static int run_cmd(int silent, const char *argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (silent) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Find a free TCP port by binding to 0 and checking what we got */
static int find_free_port(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 5432;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return 5432;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(sock, (struct sockaddr *)&addr, &len) < 0) {
        close(sock);
        return 5432;
    }

    int port = ntohs(addr.sin_port);
    close(sock);
    return port;
}

/* Wait for postgres to accept connections, up to timeout_ms */
static int wait_for_ready(rpgl_instance *inst, int timeout_ms) {
    char pg_isready[4096];
    snprintf(pg_isready, sizeof(pg_isready), "%s/pg_isready", inst->pg_bin_dir);

    /* If pg_isready exists, use it */
    if (access(pg_isready, X_OK) == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", inst->port);

        const char *argv[] = {
            pg_isready,
            "-h", inst->socket_dir,
            "-p", port_str,
            "-U", "postgres",
            "-q",
            NULL
        };

        int elapsed = 0;
        while (elapsed < timeout_ms) {
            if (run_cmd(1, argv) == 0)
                return RPGL_OK;
            usleep(50000); /* 50ms */
            elapsed += 50;
        }
        set_error(inst, "Postgres did not become ready within %dms", timeout_ms);
        return RPGL_ERR_TIMEOUT;
    }

    /* Fallback: try connecting to the unix socket */
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        char sockpath[4096];
        snprintf(sockpath, sizeof(sockpath), "%s/.s.PGSQL.%d",
                 inst->socket_dir, inst->port);

        struct stat st;
        if (stat(sockpath, &st) == 0) {
            /* Socket file exists, try connecting */
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock >= 0) {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);

                if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    close(sock);
                    /* Give it a tiny bit more time to finish startup */
                    usleep(50000);
                    return RPGL_OK;
                }
                close(sock);
            }
        }
        usleep(50000);
        elapsed += 50;
    }
    set_error(inst, "Postgres socket did not appear within %dms", timeout_ms);
    return RPGL_ERR_TIMEOUT;
}

/* ---- Public API ---- */

int rpgl_start(const rpgl_options *opts, rpgl_instance **out) {
    if (!out) return RPGL_ERR_INIT;

    /*
     * Serialise the cleanup + directory-creation window across processes, then
     * release before the slow boot.  Cleaning up and mkdtemp()'ing under the
     * lock keeps a concurrent rpgl_start() from scanning /tmp while our new
     * dir exists but is not yet populated; once we hold a data dir, the age
     * guard in cleanup_stale_tmpdirs() protects it and we no longer need the
     * lock (so parallel boots proceed independently).
     */
    int lock_fd = acquire_startup_lock();

    /* Clean up any stale tmpdirs from previous crashed runs */
    cleanup_stale_tmpdirs();

    char *bin_dir = find_pg_bin_dir();
    if (!bin_dir) { release_startup_lock(lock_fd); return RPGL_ERR_INIT; }

    rpgl_instance *inst = calloc(1, sizeof(rpgl_instance));
    if (!inst) { free(bin_dir); release_startup_lock(lock_fd); return RPGL_ERR_OOM; }

    inst->pg_bin_dir = bin_dir;
    inst->db_name = strdup((opts && opts->db_name) ? opts->db_name : "postgres");
    inst->port = (opts && opts->port > 0) ? opts->port : find_free_port();

    int silent = opts ? opts->silent : 1;
    int keep_data = opts ? opts->keep_data : 0;

    /* ── Data directory ── */
    if (opts && opts->data_dir) {
        inst->data_dir = strdup(opts->data_dir);
        inst->owns_data_dir = 0;
    } else {
        char tmpl[] = "/tmp/rpgl_XXXXXX";
        char *tmpdir = mkdtemp(tmpl);
        if (!tmpdir) {
            set_error(inst, "mkdtemp failed: %s", strerror(errno));
            free(inst->pg_bin_dir);
            free(inst->db_name);
            free(inst);
            release_startup_lock(lock_fd);
            return RPGL_ERR_INIT;
        }
        inst->data_dir = strdup(tmpdir);
        inst->owns_data_dir = !keep_data;
    }

    /* Socket dir = data dir (postgres creates .s.PGSQL.PORT here) */
    inst->socket_dir = strdup(inst->data_dir);

    /*
     * Our data dir now exists and is seconds old, so the age guard protects it
     * from any other process's cleanup.  Release the startup lock before the
     * slow initdb/boot so concurrent starts are not serialised behind us.
     */
    release_startup_lock(lock_fd);
    lock_fd = -1;

    /* ── Set LD_LIBRARY_PATH for child processes ── */
    char lib_path[4096];
    snprintf(lib_path, sizeof(lib_path), "%s/../lib", bin_dir);
    setenv("LD_LIBRARY_PATH", lib_path, 1);

    /* ── initdb (if needed) ── */
    char version_file[4096];
    snprintf(version_file, sizeof(version_file), "%s/PG_VERSION", inst->data_dir);

    if (access(version_file, F_OK) != 0) {
        char initdb_path[4096];
        snprintf(initdb_path, sizeof(initdb_path), "%s/initdb", bin_dir);
        char *share_dir = find_share_dir(bin_dir);

        const char *argv[20];
        int argc = 0;
        argv[argc++] = initdb_path;
        argv[argc++] = "-D";
        argv[argc++] = inst->data_dir;
        argv[argc++] = "-U";
        argv[argc++] = "postgres";
        argv[argc++] = "--no-sync";
        argv[argc++] = "--no-instructions";
        argv[argc++] = "-A";
        argv[argc++] = "trust";
        if (share_dir) {
            argv[argc++] = "-L";
            argv[argc++] = share_dir;
        }
        argv[argc] = NULL;

        int rc = run_cmd(silent, (const char **)argv);
        free(share_dir);

        if (rc != 0) {
            set_error(inst, "initdb failed with exit code %d", rc);
            rpgl_stop(inst);
            return RPGL_ERR_INIT;
        }
    }

    /* ── Tweak postgresql.conf for speed ── */
    char conf_path[4096];
    snprintf(conf_path, sizeof(conf_path), "%s/postgresql.conf", inst->data_dir);
    FILE *conf = fopen(conf_path, "a");
    if (conf) {
        fprintf(conf, "\n# RustyPGlite: tuned for testing speed\n");
        fprintf(conf, "fsync = off\n");
        fprintf(conf, "synchronous_commit = off\n");
        fprintf(conf, "full_page_writes = off\n");
        fprintf(conf, "random_page_cost = 1.1\n");
        fprintf(conf, "shared_buffers = 64MB\n");
        fprintf(conf, "work_mem = 16MB\n");
        fprintf(conf, "max_connections = 500\n");
        fprintf(conf, "log_min_messages = warning\n");
        fprintf(conf, "log_statement = 'none'\n");
        fprintf(conf, "unix_socket_directories = '%s'\n", inst->socket_dir);
        fprintf(conf, "listen_addresses = ''\n");  /* unix socket only, no TCP */
        fprintf(conf, "port = %d\n", inst->port);
        fclose(conf);
    }

    /* ── Start postgres ── */
    char pg_ctl_path[4096];
    snprintf(pg_ctl_path, sizeof(pg_ctl_path), "%s/pg_ctl", bin_dir);

    /* Check if pg_ctl exists; if not, start postgres directly */
    if (access(pg_ctl_path, X_OK) == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", inst->port);

        const char *argv[] = {
            pg_ctl_path,
            "start",
            "-D", inst->data_dir,
            "-w",       /* wait for startup */
            "-t", "10", /* timeout 10 seconds */
            "-l", "/dev/null",
            "-o", "-F",  /* no fsync */
            NULL
        };

        int rc = run_cmd(silent, argv);
        if (rc != 0) {
            set_error(inst, "pg_ctl start failed with exit code %d", rc);
            rpgl_stop(inst);
            return RPGL_ERR_START;
        }

        /* Read the PID from postmaster.pid */
        char pidfile[4096];
        snprintf(pidfile, sizeof(pidfile), "%s/postmaster.pid", inst->data_dir);
        FILE *f = fopen(pidfile, "r");
        if (f) {
            if (fscanf(f, "%d", &inst->pg_pid) != 1)
                inst->pg_pid = 0;
            fclose(f);
        }
    } else {
        /* No pg_ctl — start postgres directly */
        char postgres_path[4096];
        snprintf(postgres_path, sizeof(postgres_path), "%s/postgres", bin_dir);
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", inst->port);

        pid_t pid = fork();
        if (pid < 0) {
            set_error(inst, "fork failed: %s", strerror(errno));
            rpgl_stop(inst);
            return RPGL_ERR_START;
        }
        if (pid == 0) {
            /* Child: start postgres */
            if (silent) {
                int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                    close(devnull);
                }
            }
            setsid(); /* new session so signals don't propagate */
            execl(postgres_path, "postgres",
                  "-D", inst->data_dir,
                  "-F",
                  "-k", inst->socket_dir,
                  "-p", port_str,
                  "-h", "",  /* no TCP */
                  NULL);
            _exit(127);
        }
        inst->pg_pid = pid;
    }

    /* ── Wait for ready ── */
    int rc = wait_for_ready(inst, 10000); /* 10 second timeout */
    if (rc != RPGL_OK) {
        rpgl_stop(inst);
        return rc;
    }

    /* ── Build connection string ── */
    char cs[4096];
    snprintf(cs, sizeof(cs),
             "host=%s;port=%d;database=%s;username=postgres",
             inst->socket_dir, inst->port, inst->db_name);
    inst->conn_string = strdup(cs);

    track_instance(inst);
    *out = inst;
    return RPGL_OK;
}

int rpgl_stop(rpgl_instance *inst) {
    if (!inst) return RPGL_ERR_INIT;

    untrack_instance(inst);

    /* Try pg_ctl stop first */
    if (inst->pg_bin_dir && inst->data_dir) {
        char pg_ctl_path[4096];
        snprintf(pg_ctl_path, sizeof(pg_ctl_path), "%s/pg_ctl", inst->pg_bin_dir);

        if (access(pg_ctl_path, X_OK) == 0) {
            const char *argv[] = {
                pg_ctl_path, "stop",
                "-D", inst->data_dir,
                "-m", "fast",
                "-w",
                "-t", "5",
                NULL
            };
            run_cmd(1, argv);
        } else if (inst->pg_pid > 0) {
            /* No pg_ctl — signal directly */
            kill(inst->pg_pid, SIGTERM);
            int status;
            for (int i = 0; i < 50; i++) {
                if (waitpid(inst->pg_pid, &status, WNOHANG) > 0) break;
                usleep(100000);
            }
            /* Force kill if still alive */
            if (kill(inst->pg_pid, 0) == 0) {
                kill(inst->pg_pid, SIGKILL);
                waitpid(inst->pg_pid, &status, 0);
            }
        }
    }

    /* Clean up data directory */
    if (inst->owns_data_dir && inst->data_dir) {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", inst->data_dir);
        system(cmd);
    }

    free(inst->data_dir);
    free(inst->socket_dir);
    free(inst->db_name);
    free(inst->pg_bin_dir);
    free(inst->conn_string);
    free(inst);
    return RPGL_OK;
}

int rpgl_connect_existing(const char *data_dir, rpgl_instance **out) {
    if (!data_dir || !out) return RPGL_ERR_INIT;

    /* Read postmaster.pid to get port and socket dir */
    char pidfile[4096];
    snprintf(pidfile, sizeof(pidfile), "%s/postmaster.pid", data_dir);

    FILE *f = fopen(pidfile, "r");
    if (!f) return RPGL_ERR_INIT;  /* not running */

    char lines[8][256];
    int nlines = 0;
    while (nlines < 8 && fgets(lines[nlines], sizeof(lines[nlines]), f))
        nlines++;
    fclose(f);

    if (nlines < 5) return RPGL_ERR_INIT;

    /* postmaster.pid format:
     * line 1: PID
     * line 2: data directory
     * line 3: start timestamp
     * line 4: port
     * line 5: socket directory
     */
    int port = atoi(lines[3]);
    char *socket_dir = lines[4];
    /* Strip newline */
    socket_dir[strcspn(socket_dir, "\n")] = '\0';

    if (port <= 0) return RPGL_ERR_INIT;

    rpgl_instance *inst = calloc(1, sizeof(rpgl_instance));
    if (!inst) return RPGL_ERR_OOM;

    inst->data_dir = strdup(data_dir);
    inst->socket_dir = strdup(socket_dir);
    inst->db_name = strdup("postgres");
    inst->pg_bin_dir = find_pg_bin_dir();
    inst->port = port;
    inst->owns_data_dir = 0;  /* don't delete — we didn't create it */
    inst->pg_pid = 0;         /* don't manage — we didn't start it */

    char cs[4096];
    snprintf(cs, sizeof(cs),
             "host=%s;port=%d;database=postgres;username=postgres",
             inst->socket_dir, inst->port);
    inst->conn_string = strdup(cs);

    *out = inst;
    return RPGL_OK;
}

const char *rpgl_connection_string(rpgl_instance *inst) {
    return inst ? inst->conn_string : NULL;
}

const char *rpgl_socket_dir(rpgl_instance *inst) {
    return inst ? inst->socket_dir : NULL;
}

int rpgl_port(rpgl_instance *inst) {
    return inst ? inst->port : 0;
}

const char *rpgl_data_dir(rpgl_instance *inst) {
    return inst ? inst->data_dir : NULL;
}

const char *rpgl_last_error(rpgl_instance *inst) {
    if (!inst) return "null instance";
    return inst->error_msg;
}

/*
 * Execute SQL by connecting to the running server via libpq (dynamically loaded).
 * This avoids needing psql/createdb binaries — only postgres, initdb, pg_ctl required.
 */

/* libpq function pointers (loaded once via dlopen) */
static void *libpq_handle = NULL;
typedef void *(*pq_connectdb_fn)(const char *conninfo);
typedef int (*pq_status_fn)(const void *conn);
typedef void *(*pq_exec_fn)(void *conn, const char *sql);
typedef int (*pq_result_status_fn)(const void *res);
typedef const char *(*pq_result_error_fn)(const void *res);
typedef void (*pq_clear_fn)(void *res);
typedef void (*pq_finish_fn)(void *conn);

static pq_connectdb_fn  pq_connectdb  = NULL;
static pq_status_fn     pq_status     = NULL;
static pq_exec_fn       pq_exec       = NULL;
static pq_result_status_fn pq_result_status = NULL;
static pq_result_error_fn  pq_result_error  = NULL;
static pq_clear_fn      pq_clear      = NULL;
static pq_finish_fn     pq_finish     = NULL;

static int load_libpq(const char *bin_dir) {
    if (libpq_handle) return 0;

    char libpath[4096];

    /*
     * The shared-library SUFFIX is platform-specific, and so is where the
     * version goes: Linux ships libpq.so.5, macOS ships libpq.5.dylib. Getting
     * this wrong does not look like a portability bug from the outside — dlopen
     * simply returns NULL for a path that is otherwise correct, so it reads as
     * a missing or corrupt Postgres install rather than a wrong suffix.
     */
#if defined(__APPLE__)
    static const char *const LIBPQ_VERSIONED   = "libpq.5.dylib";
    static const char *const LIBPQ_UNVERSIONED = "libpq.dylib";
#else
    static const char *const LIBPQ_VERSIONED   = "libpq.so.5";
    static const char *const LIBPQ_UNVERSIONED = "libpq.so";
#endif

    /* Try pg/lib/ next to bin/ */
    snprintf(libpath, sizeof(libpath), "%s/../lib/%s", bin_dir, LIBPQ_VERSIONED);
    libpq_handle = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    if (!libpq_handle) {
        snprintf(libpath, sizeof(libpath), "%s/../lib/%s", bin_dir, LIBPQ_UNVERSIONED);
        libpq_handle = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    }
    if (!libpq_handle) {
        /* Try system libpq */
        libpq_handle = dlopen(LIBPQ_VERSIONED, RTLD_NOW | RTLD_LOCAL);
    }
    if (!libpq_handle) return -1;

    pq_connectdb     = (pq_connectdb_fn)dlsym(libpq_handle, "PQconnectdb");
    pq_status        = (pq_status_fn)dlsym(libpq_handle, "PQstatus");
    pq_exec          = (pq_exec_fn)dlsym(libpq_handle, "PQexec");
    pq_result_status = (pq_result_status_fn)dlsym(libpq_handle, "PQresultStatus");
    pq_result_error  = (pq_result_error_fn)dlsym(libpq_handle, "PQresultErrorMessage");
    pq_clear         = (pq_clear_fn)dlsym(libpq_handle, "PQclear");
    pq_finish        = (pq_finish_fn)dlsym(libpq_handle, "PQfinish");

    if (!pq_connectdb || !pq_status || !pq_exec || !pq_result_status ||
        !pq_result_error || !pq_clear || !pq_finish) {
        dlclose(libpq_handle);
        libpq_handle = NULL;
        return -1;
    }
    return 0;
}

/* Connect to the running server and execute SQL */
static int exec_via_libpq(rpgl_instance *inst, const char *db_name, const char *sql) {
    if (load_libpq(inst->pg_bin_dir) < 0) {
        set_error(inst, "Could not load libpq");
        return RPGL_ERR_INTERNAL;
    }

    char conninfo[4096];
    snprintf(conninfo, sizeof(conninfo),
             "host=%s port=%d dbname=%s user=postgres",
             inst->socket_dir, inst->port, db_name);

    void *conn = pq_connectdb(conninfo);
    if (!conn || pq_status(conn) != 0 /* CONNECTION_OK */) {
        set_error(inst, "libpq connect failed to %s", db_name);
        if (conn) pq_finish(conn);
        return RPGL_ERR_INTERNAL;
    }

    void *res = pq_exec(conn, sql);
    int status = pq_result_status(res);
    /* PGRES_COMMAND_OK=1, PGRES_TUPLES_OK=2 */
    int ok = (status == 1 || status == 2);

    if (!ok) {
        const char *err = pq_result_error(res);
        set_error(inst, "SQL error: %s", err ? err : "unknown");
    }

    pq_clear(res);
    pq_finish(conn);
    return ok ? RPGL_OK : RPGL_ERR_INTERNAL;
}

int rpgl_create_database(rpgl_instance *inst, const char *name) {
    if (!inst || !name) return RPGL_ERR_INIT;

    /* Always use libpq (in-process, no fork) — ~1ms vs ~60ms for createdb fork */
    char sql[512];
    snprintf(sql, sizeof(sql), "CREATE DATABASE \"%s\"", name);
    int rc = exec_via_libpq(inst, "postgres", sql);
    if (rc == RPGL_OK) return rc;

    /* Fallback to createdb if libpq failed to load */
    char createdb_path[4096];
    snprintf(createdb_path, sizeof(createdb_path), "%s/createdb", inst->pg_bin_dir);
    if (access(createdb_path, X_OK) == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", inst->port);
        const char *argv[] = {
            createdb_path,
            "-h", inst->socket_dir,
            "-p", port_str,
            "-U", "postgres",
            name,
            NULL
        };
        return (run_cmd(1, argv) == 0) ? RPGL_OK : RPGL_ERR_INTERNAL;
    }

    return rc;
}

int rpgl_exec_sql(rpgl_instance *inst, const char *db_name, const char *sql) {
    if (!inst || !sql) return RPGL_ERR_INIT;
    if (!db_name) db_name = inst->db_name;

    /* Always use libpq (in-process, no fork) — ~1ms vs ~159ms for psql fork */
    int rc = exec_via_libpq(inst, db_name, sql);
    if (rc == RPGL_OK) return rc;

    /* Fallback to psql if libpq failed to load */
    char psql_path[4096];
    snprintf(psql_path, sizeof(psql_path), "%s/psql", inst->pg_bin_dir);
    if (access(psql_path, X_OK) == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", inst->port);
        const char *argv[] = {
            psql_path,
            "-h", inst->socket_dir,
            "-p", port_str,
            "-U", "postgres",
            "-d", db_name,
            "-c", sql,
            "-q",
            NULL
        };
        int fork_rc = run_cmd(1, argv);
        if (fork_rc != 0) {
            set_error(inst, "psql exec failed (exit %d) for: %.100s", fork_rc, sql);
            return RPGL_ERR_INTERNAL;
        }
        return RPGL_OK;
    }

    return rc;
}

int rpgl_exec_file(rpgl_instance *inst, const char *db_name, const char *file_path) {
    if (!inst || !file_path) return RPGL_ERR_INIT;
    if (!db_name) db_name = inst->db_name;

    /* Use psql -f if available */
    char psql_path[4096];
    snprintf(psql_path, sizeof(psql_path), "%s/psql", inst->pg_bin_dir);

    if (access(psql_path, X_OK) == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", inst->port);
        const char *argv[] = {
            psql_path,
            "-h", inst->socket_dir,
            "-p", port_str,
            "-U", "postgres",
            "-d", db_name,
            "-f", file_path,
            "-q",
            NULL
        };
        int rc = run_cmd(1, argv);
        if (rc != 0) {
            set_error(inst, "psql file exec failed (exit %d) for: %s", rc, file_path);
            return RPGL_ERR_INTERNAL;
        }
        return RPGL_OK;
    }

    /* Fallback: read file and execute via libpq */
    FILE *f = fopen(file_path, "r");
    if (!f) {
        set_error(inst, "Cannot open file: %s", file_path);
        return RPGL_ERR_INTERNAL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *sql = malloc(len + 1);
    if (!sql) { fclose(f); return RPGL_ERR_OOM; }
    fread(sql, 1, len, f);
    sql[len] = '\0';
    fclose(f);

    int rc = exec_via_libpq(inst, db_name, sql);
    free(sql);
    return rc;
}
