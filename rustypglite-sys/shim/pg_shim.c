/*
 * pg_shim.c - Embedded PostgreSQL lifecycle manager
 *
 * Manages a real postgres server process: initdb → pg_ctl start → unix socket.
 * Any standard Postgres client connects via the returned connection string.
 *
 * A server started here never outlives the process that started it (its
 * "owner"), however that process ends — see "Ownership" below — unless the
 * caller asked for a durable server.
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
#include <sys/resource.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>
#include <ftw.h>
#include <sys/file.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

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
    int      borrowed;       /* from rpgl_connect_existing: never stop it */
    int      durable;        /* not bound to our lifetime, never swept */
    pid_t    pg_pid;         /* postmaster PID (0 if not running) */
    pid_t    watcher_pid;    /* the watchdog process (0 if none) */
    int      watcher_fd;     /* our end of the watchdog socket (-1 if none) */
    char     token[40];      /* identifies this start in owner.json */
};

/* ---- Instance tracking for cleanup on exit ---- */
/*
 * A normal exit stops every server we still hold, synchronously, so the
 * process does not end with work left for the watchdog.  There are
 * deliberately NO signal handlers: stopping a server from inside one calls
 * fork/malloc/exec, which is not async-signal-safe and can deadlock the very
 * process we are trying to clean up after — a hung owner holds its watchdog
 * socket open and so leaks the server.  A process killed by a signal is the
 * watchdog's job.
 */

#define MAX_INSTANCES 256
static rpgl_instance *g_instances[MAX_INSTANCES];
static int g_instance_count = 0;
static int g_atexit_registered = 0;
static pthread_mutex_t g_instances_lock = PTHREAD_MUTEX_INITIALIZER;

static void cleanup_all_instances(void) {
    for (;;) {
        pthread_mutex_lock(&g_instances_lock);
        rpgl_instance *inst = g_instance_count > 0 ? g_instances[g_instance_count - 1] : NULL;
        pthread_mutex_unlock(&g_instances_lock);
        if (!inst) return;
        rpgl_stop(inst);   /* untracks itself */
    }
}

static void track_instance(rpgl_instance *inst) {
    pthread_mutex_lock(&g_instances_lock);
    if (!g_atexit_registered) {
        atexit(cleanup_all_instances);
        g_atexit_registered = 1;
    }
    if (g_instance_count < MAX_INSTANCES) {
        g_instances[g_instance_count++] = inst;
    }
    pthread_mutex_unlock(&g_instances_lock);
}

static void untrack_instance(rpgl_instance *inst) {
    pthread_mutex_lock(&g_instances_lock);
    for (int i = 0; i < g_instance_count; i++) {
        if (g_instances[i] == inst) {
            g_instances[i] = g_instances[--g_instance_count];
            g_instances[g_instance_count] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_instances_lock);
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

/* ---- Ownership ---- */
/*
 * Every server started here is bound to its owner — the process that called
 * rpgl_start() — in two ways:
 *
 *  1. A WATCHDOG.  Before initdb we fork a small /bin/sh process holding one
 *     end of a socketpair; the owner holds the other end, close-on-exec, so no
 *     other process (postgres included) inherits it.  When the owner stops the
 *     server it writes "released" and the watchdog exits.  When the owner dies
 *     any other way — SIGKILL, a crash, an abort — the kernel closes its end,
 *     the watchdog reads EOF, fast-stops the server and removes the data dir.
 *     The watchdog is in its own session, ignores the terminal's signals and
 *     does not share the owner's fate, which is the point: a thread in the
 *     owner, or a signal handler, dies with it before it can act.
 *
 *     Why not PR_SET_PDEATHSIG?  pg_ctl daemonises the postmaster (it forks,
 *     setsid()s, execs /bin/sh which execs postgres, and exits), so the
 *     postmaster's parent is init, not the owner, and PDEATHSIG would never
 *     fire.  Even if we stopped using pg_ctl: PDEATHSIG fires when the forking
 *     THREAD exits, not the process — in .NET or Node that is a thread-pool
 *     thread, which would kill a healthy server mid-suite; it is Linux-only;
 *     and it kills the server but cannot remove the data dir.
 *
 *  2. owner.json in the data dir, naming the owner by PID *and* start time
 *     (so a reused PID cannot pass for it), the boot, and the PID namespace.
 *     Every rpgl_start() first sweeps the temp root and reclaims a server
 *     only when owner.json proves its owner is dead and its watchdog is gone
 *     too.  This is the backstop for a watchdog that was itself killed.
 *
 * A durable server (opts->durable) gets neither a watchdog nor atexit
 * handling, and the sweep never touches it; only rpgl_stop / rpgl_stop_dir
 * stop it.
 */

#define OWNER_FILE "owner.json"
#define OWNER_JSON_MAX 65536

/* What we know about a process's identity, as opaque strings. */
static void read_small_file(const char *path, char *out, size_t outsz) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(out, 1, outsz - 1, f);
    fclose(f);
    out[n] = '\0';
    out[strcspn(out, "\n")] = '\0';
}

/* Identifies this boot: a PID + start time is only meaningful within one. */
static void current_boot_id(char *out, size_t outsz) {
#if defined(__linux__)
    read_small_file("/proc/sys/kernel/random/boot_id", out, outsz);
#elif defined(__APPLE__)
    struct timeval tv;
    size_t len = sizeof(tv);
    int mib[2] = { CTL_KERN, KERN_BOOTTIME };
    if (sysctl(mib, 2, &tv, &len, NULL, 0) == 0)
        snprintf(out, outsz, "%ld.%06ld", (long)tv.tv_sec, (long)tv.tv_usec);
    else
        out[0] = '\0';
#else
    out[0] = '\0';
#endif
}

/* PIDs are only comparable within one PID namespace. */
static void current_pid_ns(char *out, size_t outsz) {
    out[0] = '\0';
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/ns/pid", out, outsz - 1);
    out[n > 0 ? n : 0] = '\0';
#endif
}

/*
 * The start time of a process as an opaque string.
 * Returns 0 on success, -1 if there is no such process, -2 if it exists (or
 * may) but its start time cannot be read.
 */
static int proc_start_time(pid_t pid, char *out, size_t outsz) {
    if (pid <= 0) return -2;
    if (kill(pid, 0) != 0 && errno == ESRCH) return -1;
#if defined(__linux__)
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? -1 : -2;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* comm (field 2) may contain spaces and parens: parse after the last ')'. */
    char *p = strrchr(buf, ')');
    if (!p || p[1] != ' ') return -2;
    p += 2;                                  /* field 3 */
    for (int field = 3; field < 22; field++) {
        p = strchr(p, ' ');
        if (!p) return -2;
        p++;
    }
    size_t len = strcspn(p, " ");            /* field 22: starttime */
    if (len == 0 || len >= outsz) return -2;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
#elif defined(__APPLE__)
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
    memset(&kp, 0, sizeof(kp));
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0) return -2;
    if (len == 0) return -1;
    snprintf(out, outsz, "%ld.%06ld",
             (long)kp.kp_proc.p_starttime.tv_sec,
             (long)kp.kp_proc.p_starttime.tv_usec);
    return 0;
#else
    (void)out; (void)outsz;
    return -2;
#endif
}

static void random_token(char *out, size_t outsz) {
    unsigned char bytes[16];
    int ok = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ok = read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes);
        close(fd);
    }
    if (!ok) {
        static unsigned long counter = 0;
        unsigned long long seed = ((unsigned long long)getpid() << 32)
                                ^ (unsigned long long)time(NULL)
                                ^ (unsigned long long)(++counter * 2654435761u);
        for (size_t i = 0; i < sizeof(bytes); i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            bytes[i] = (unsigned char)(seed >> 56);
        }
    }
    for (size_t i = 0; i < sizeof(bytes) && 2 * i + 2 < outsz; i++)
        snprintf(out + 2 * i, 3, "%02x", bytes[i]);
}

/* ---- owner.json ---- */

static void json_put_string(FILE *f, const char *s) {
    if (!s) { fputs("null", f); return; }
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

/* Where the value of "key" starts in a flat JSON object we wrote, or NULL. */
static const char *json_value(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, needle)) != NULL) {
        const char *q = p + strlen(needle);
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            return q;
        }
        p = q;
    }
    return NULL;
}

/* A string value; 0 on success.  Escapes other than \" and \\ are kept raw. */
static int json_get_string(const char *json, const char *key, char *out, size_t outsz) {
    const char *v = json_value(json, key);
    if (!v || *v != '"') return -1;
    size_t n = 0;
    for (v++; *v && *v != '"'; v++) {
        if (*v == '\\' && v[1]) v++;
        if (n + 1 >= outsz) return -1;
        out[n++] = *v;
    }
    if (*v != '"') return -1;
    out[n] = '\0';
    return 0;
}

static int json_get_long(const char *json, const char *key, long *out) {
    const char *v = json_value(json, key);
    if (!v) return -1;
    char *end;
    errno = 0;
    long x = strtol(v, &end, 10);
    if (end == v || errno) return -1;
    *out = x;
    return 0;
}

/* 1 for true, 0 for false, -1 when absent or not a boolean. */
static int json_get_bool(const char *json, const char *key) {
    const char *v = json_value(json, key);
    if (!v) return -1;
    if (strncmp(v, "true", 4) == 0) return 1;
    if (strncmp(v, "false", 5) == 0) return 0;
    return -1;
}

/*
 * Write owner.json atomically (temp file + rename), so a reader never sees
 * half of one.  Returns 0 on success.
 */
static int write_owner_file(rpgl_instance *inst) {
    char path[4096], tmp[4096];
    snprintf(path, sizeof(path), "%s/%s", inst->data_dir, OWNER_FILE);
    snprintf(tmp, sizeof(tmp), "%s/%s.tmp", inst->data_dir, OWNER_FILE);

    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }

    char owner_start[64] = "", watcher_start[64] = "", boot[128], ns[128], cwd[4096];
    proc_start_time(getpid(), owner_start, sizeof(owner_start));
    if (inst->watcher_pid > 0)
        proc_start_time(inst->watcher_pid, watcher_start, sizeof(watcher_start));
    current_boot_id(boot, sizeof(boot));
    current_pid_ns(ns, sizeof(ns));
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';

    /* Informational: which session (agent, CI job, shell) started it. */
    static const char *const session_vars[] = {
        "RUSTYPGLITE_SESSION_ID",
        "CLAUDE_CODE_BRIDGE_SESSION_ID",
        "CLAUDE_CODE_SESSION_ID",
        NULL
    };
    const char *session_var = NULL, *session_id = NULL;
    for (int i = 0; session_vars[i]; i++) {
        const char *v = getenv(session_vars[i]);
        if (v && *v) { session_var = session_vars[i]; session_id = v; break; }
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": 1,\n");
    fprintf(f, "  \"owner_pid\": %d,\n", (int)getpid());
    fprintf(f, "  \"owner_start\": "); json_put_string(f, owner_start); fprintf(f, ",\n");
    fprintf(f, "  \"owner_cwd\": "); json_put_string(f, cwd); fprintf(f, ",\n");
    fprintf(f, "  \"unix_session\": %d,\n", (int)getsid(0));
    fprintf(f, "  \"session_id\": "); json_put_string(f, session_id); fprintf(f, ",\n");
    fprintf(f, "  \"session_var\": "); json_put_string(f, session_var); fprintf(f, ",\n");
    fprintf(f, "  \"boot_id\": "); json_put_string(f, boot); fprintf(f, ",\n");
    fprintf(f, "  \"pid_ns\": "); json_put_string(f, ns); fprintf(f, ",\n");
    fprintf(f, "  \"watcher_pid\": %d,\n", (int)inst->watcher_pid);
    fprintf(f, "  \"watcher_start\": "); json_put_string(f, watcher_start); fprintf(f, ",\n");
    fprintf(f, "  \"durable\": %s,\n", inst->durable ? "true" : "false");
    fprintf(f, "  \"owns_data_dir\": %s,\n", inst->owns_data_dir ? "true" : "false");
    fprintf(f, "  \"port\": %d,\n", inst->port);
    fprintf(f, "  \"created_at\": %ld,\n", (long)time(NULL));
    fprintf(f, "  \"token\": "); json_put_string(f, inst->token); fprintf(f, "\n");
    fprintf(f, "}\n");

    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/*
 * Read a dir's owner.json into buf.  Returns 0 on success, -1 if there is
 * none, -2 if it exists but cannot be trusted (not ours, not a regular file,
 * unreadable, too large).
 */
static int read_owner_file(const char *dir, char *buf, size_t bufsz) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, OWNER_FILE);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? -1 : -2;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()
        || st.st_size <= 0 || (size_t)st.st_size >= bufsz) {
        close(fd);
        return -2;
    }
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0) return -2;
    buf[n] = '\0';
    return 0;
}

/* Does this dir's owner.json still carry our token?  (Absent counts as no.) */
static int owner_token_is(const char *dir, const char *token) {
    char *buf = malloc(OWNER_JSON_MAX);
    char have[64];
    int same = buf && read_owner_file(dir, buf, OWNER_JSON_MAX) == 0
            && json_get_string(buf, "token", have, sizeof(have)) == 0
            && strcmp(have, token) == 0;
    free(buf);
    return same;
}

enum { PROC_DEAD = 0, PROC_ALIVE = 1, PROC_UNKNOWN = 2 };

/*
 * Is the process named by (pid, start) — recorded in owner.json under the
 * given boot and PID namespace — still running?  UNKNOWN whenever we cannot
 * be sure, and callers must treat UNKNOWN as alive.
 */
static int process_state(long pid, const char *start, const char *boot, const char *ns) {
    char cur_boot[128], cur_ns[128], cur_start[64];
    current_boot_id(cur_boot, sizeof(cur_boot));
    current_pid_ns(cur_ns, sizeof(cur_ns));

    /* A different boot: nothing recorded then can still be running. */
    if (boot[0] && cur_boot[0] && strcmp(boot, cur_boot) != 0) return PROC_DEAD;
    /*
     * A different PID namespace (a container, a sandbox): the PID means
     * something else here, and "no such process" proves nothing.  Hands off.
     */
    if (strcmp(ns, cur_ns) != 0) return PROC_UNKNOWN;
    if (pid <= 0 || !start[0]) return PROC_UNKNOWN;

    int rc = proc_start_time((pid_t)pid, cur_start, sizeof(cur_start));
    if (rc == -1) return PROC_DEAD;
    if (rc != 0) return PROC_UNKNOWN;
    /* Same PID, different start time: the PID was reused; the owner is gone. */
    return strcmp(start, cur_start) == 0 ? PROC_ALIVE : PROC_DEAD;
}

/* ---- Stopping a server by its data dir ---- */

/* The postmaster PID from line 1 of postmaster.pid, or 0. */
static pid_t postmaster_pid(const char *dir) {
    char pidfile[4096];
    snprintf(pidfile, sizeof(pidfile), "%s/postmaster.pid", dir);
    FILE *f = fopen(pidfile, "r");
    if (!f) return 0;
    long pid = 0;
    if (fscanf(f, "%ld", &pid) != 1) pid = 0;
    fclose(f);
    return pid > 0 ? (pid_t)pid : 0;
}

/*
 * Is pid a running postmaster for this data dir?  1 yes (or cannot rule it
 * out), 0 no.  A postmaster chdir()s into its data dir, so on Linux its cwd
 * tells a live server apart from a stale postmaster.pid whose PID has since
 * been reused by an unrelated process.
 */
static int is_postmaster_of(pid_t pid, const char *dir) {
    if (pid <= 0) return 0;
    waitpid(pid, NULL, WNOHANG);        /* reap it if it is our own child */
    if (kill(pid, 0) != 0 && errno == ESRCH) return 0;
#if defined(__linux__)
    char link[64], cwd[PATH_MAX], real[PATH_MAX];
    snprintf(link, sizeof(link), "/proc/%d/cwd", (int)pid);
    ssize_t n = readlink(link, cwd, sizeof(cwd) - 1);
    if (n < 0) return errno == ENOENT ? 0 : 1;   /* gone (or a zombie) vs. can't see */
    cwd[n] = '\0';
    if (!realpath(dir, real)) return 1;
    return strcmp(cwd, real) == 0;
#else
    (void)dir;
    return 1;
#endif
}

static int wait_gone(pid_t pid, const char *dir, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (!is_postmaster_of(pid, dir)) return 1;
        usleep(50000);
    }
    return !is_postmaster_of(pid, dir);
}

/*
 * Stop whatever server is running in dir: fast shutdown (SIGINT), then
 * immediate (SIGQUIT), then SIGKILL.  Signals rather than pg_ctl, so it needs
 * no binaries and trusts nothing but postmaster.pid and the process table.
 * Returns 0 when no server is running there any more, -1 otherwise.
 */
static int stop_server_in_dir(const char *dir) {
    pid_t pid = postmaster_pid(dir);
    if (!is_postmaster_of(pid, dir)) return 0;

    static const int sigs[] = { SIGINT, SIGQUIT, SIGKILL };
    static const int waits_ms[] = { 10000, 5000, 5000 };
    for (int i = 0; i < 3; i++) {
        if (kill(pid, sigs[i]) != 0 && errno == ESRCH) return 0;
        if (wait_gone(pid, dir, waits_ms[i])) return 0;
    }
    return -1;
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)st; (void)type; (void)ftw;
    remove(path);
    return 0;
}

/* rm -rf, without a shell.  Returns 0 when the dir is gone. */
static int remove_tree(const char *dir) {
    nftw(dir, remove_entry, 32, FTW_DEPTH | FTW_PHYS);
    struct stat st;
    return lstat(dir, &st) != 0 && errno == ENOENT ? 0 : -1;
}

/* ---- The watchdog ---- */
/*
 * Runs as: /bin/sh -c WATCHDOG_SCRIPT rustypglite-watchdog DIR TOKEN PG_CTL OWNS BEAT
 * with its stdin one end of a socketpair whose other end only the owner holds.
 *
 * While the owner lives, a background loop touches the dir every BEAT
 * seconds.  Nothing in this version judges a dir by age, but rustypglite
 * 0.1.x did (older than ten minutes with no answering socket = stale), and
 * a machine runs mixed versions for a while: an idle live server's dir must
 * never look old to them.  The loop ends with the watchdog (it checks $$),
 * or when the dir is gone.  (The loop inherits the ignored TERM, hence
 * kill -9; its current `sleep` may outlive it by up to BEAT seconds, holding
 * nothing.)
 *
 * "released" means the owner stopped the server itself: exit untouched.  EOF
 * without it means the owner is gone: stop the server and, when the dir is
 * ours to delete, delete it.  It acts only on a dir whose owner.json still
 * carries this start's token (or has none yet — the owner died during
 * initdb), so it can never act on a dir that has since been reused.  The loop
 * covers an initdb or pg_ctl the dead owner left running, which can keep
 * writing into the dir for a moment after the owner is gone.
 */
static const char WATCHDOG_SCRIPT[] =
    "trap '' HUP INT QUIT TERM PIPE\n"
    "PATH=/usr/bin:/bin:$PATH\n"
    "dir=$1 token=$2 pgctl=$3 owns=$4 beat=$5\n"
    "( while kill -0 $$ 2>/dev/null && [ -d \"$dir\" ]; do\n"
    "    touch -c \"$dir\"; sleep \"$beat\"\n"
    "  done ) </dev/null >/dev/null 2>&1 &\n"
    "hb=$!\n"
    "IFS= read -r msg\n"
    "kill -9 $hb 2>/dev/null\n"
    "[ \"$msg\" = released ] && exit 0\n"
    "if [ -f \"$dir/owner.json\" ]; then\n"
    "  grep -qF \"\\\"token\\\": \\\"$token\\\"\" \"$dir/owner.json\" || exit 0\n"
    "fi\n"
    "n=0\n"
    "while [ $n -lt 5 ]; do\n"
    "  n=$((n + 1))\n"
    "  \"$pgctl\" stop -D \"$dir\" -m fast -w -t 10 >/dev/null 2>&1 ||\n"
    "    \"$pgctl\" stop -D \"$dir\" -m immediate -w -t 5 >/dev/null 2>&1\n"
    "  [ \"$owns\" = 1 ] || exit 0\n"
    "  [ -d \"$dir\" ] || exit 0\n"
    "  if \"$pgctl\" status -D \"$dir\" >/dev/null 2>&1; then sleep 1; continue; fi\n"
    "  rm -rf -- \"$dir\"\n"
    "  sleep 1\n"
    "done\n";

#if !defined(SOCK_CLOEXEC)
static void set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}
#endif

/*
 * Fork the watchdog for inst.  Returns 0 on success.  Everything the child
 * needs is prepared before fork(): the owner may be multithreaded (.NET,
 * Node), so between fork and exec the child makes only async-signal-safe
 * calls.
 */
static int spawn_watchdog(rpgl_instance *inst) {
    char pg_ctl[4096];
    snprintf(pg_ctl, sizeof(pg_ctl), "%s/pg_ctl", inst->pg_bin_dir);
    /* Heartbeat, seconds.  Overridable for tests only. */
    char beat[16] = "60";
    const char *beat_env = getenv("RUSTYPGLITE_HEARTBEAT_SECONDS");
    if (beat_env && atoi(beat_env) > 0) snprintf(beat, sizeof(beat), "%d", atoi(beat_env));
    const char *argv[] = {
        "/bin/sh", "-c", WATCHDOG_SCRIPT, "rustypglite-watchdog",
        inst->data_dir, inst->token, pg_ctl, inst->owns_data_dir ? "1" : "0", beat,
        NULL
    };

    int sv[2];
#if defined(SOCK_CLOEXEC)
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return -1;
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    set_cloexec(sv[0]);
    set_cloexec(sv[1]);
#endif
#if defined(SO_NOSIGPIPE)
    { int one = 1; setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif

    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    struct rlimit rl;
    int max_fd = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
                  && rl.rlim_cur < 65536) ? (int)rl.rlim_cur : 65536;
    sigset_t none;
    sigemptyset(&none);

    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]); close(sv[1]);
        if (devnull >= 0) close(devnull);
        return -1;
    }
    if (pid == 0) {
        setsid();                       /* out of the owner's terminal and group */
        sigprocmask(SIG_SETMASK, &none, NULL);
        if (dup2(sv[1], STDIN_FILENO) < 0) _exit(127);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        /* Hold nothing of the owner's open: not its pipes, not its sockets. */
        for (int fd = 3; fd < max_fd; fd++) close(fd);
        if (chdir("/") != 0) { /* not fatal */ }
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(sv[1]);
    if (devnull >= 0) close(devnull);
    inst->watcher_fd = sv[0];
    inst->watcher_pid = pid;
    return 0;
}

/* Tell the watchdog the owner has dealt with the server, and wait for it. */
static void release_watchdog(rpgl_instance *inst) {
    if (inst->watcher_fd >= 0) {
        static const char msg[] = "released\n";
#if defined(MSG_NOSIGNAL)
        send(inst->watcher_fd, msg, sizeof(msg) - 1, MSG_NOSIGNAL);
#else
        send(inst->watcher_fd, msg, sizeof(msg) - 1, 0);   /* SO_NOSIGPIPE is set */
#endif
        close(inst->watcher_fd);
        inst->watcher_fd = -1;
    }
    if (inst->watcher_pid > 0) {
        /* It exits at once; bounded anyway, in case it was stopped or killed. */
        for (int waited = 0; waited < 2000; waited += 10) {
            pid_t r = waitpid(inst->watcher_pid, NULL, WNOHANG);
            if (r == inst->watcher_pid || (r < 0 && errno == ECHILD)) break;
            usleep(10000);
        }
        inst->watcher_pid = 0;
    }
}

/* ---- Temp root and sweep ---- */

static const char *temp_root_for(const rpgl_options *opts) {
    if (opts && opts->temp_root && opts->temp_root[0]) return opts->temp_root;
    const char *env = getenv("RUSTYPGLITE_TMPDIR");
    if (env && env[0]) return env;
    return "/tmp";
}

enum { SWEEP_LIVE, SWEEP_DURABLE, SWEEP_LEGACY, SWEEP_STARTING, SWEEP_SKIPPED,
       SWEEP_RECLAIMED, SWEEP_FAILED };

/*
 * A dir with no owner.json is left alone whatever its age.  Age only decides
 * whether it is worth MENTIONING: a young one is almost certainly a start in
 * progress (owner.json is written after initdb), not an old rustypglite's.
 */
#define LEGACY_NOTICE_AGE_SECONDS 120

/*
 * Judge one rpgl_* dir, and reclaim it only if owner.json proves both its
 * owner and its watchdog are dead.  Nothing else is ever touched: not a live
 * owner's dir however old, not a dir without owner.json, not a durable one,
 * not one whose owner we cannot see (another PID namespace).
 */
static int sweep_one_with(const char *path, char *buf);

static int sweep_one(const char *path) {
    char *buf = malloc(OWNER_JSON_MAX);
    if (!buf) return SWEEP_SKIPPED;
    int verdict = sweep_one_with(path, buf);
    free(buf);
    return verdict;
}

static int sweep_one_with(const char *path, char *buf) {
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;   /* not a dir: ignore */
    if (st.st_uid != geteuid() || (st.st_mode & 022)) return SWEEP_SKIPPED;

    int rc = read_owner_file(path, buf, OWNER_JSON_MAX);
    if (rc == -1)
        return time(NULL) - st.st_mtime < LEGACY_NOTICE_AGE_SECONDS ? SWEEP_STARTING : SWEEP_LEGACY;
    if (rc != 0) return SWEEP_SKIPPED;

    if (json_get_bool(buf, "durable") != 0) {
        /* true, or unreadable: either way not ours to judge */
        return json_get_bool(buf, "durable") == 1 ? SWEEP_DURABLE : SWEEP_SKIPPED;
    }

    long owner_pid = 0, watcher_pid = 0;
    char owner_start[64] = "", watcher_start[64] = "", boot[128] = "", ns[128] = "";
    if (json_get_long(buf, "owner_pid", &owner_pid) != 0
        || json_get_string(buf, "owner_start", owner_start, sizeof(owner_start)) != 0
        || json_get_string(buf, "boot_id", boot, sizeof(boot)) != 0
        || json_get_string(buf, "pid_ns", ns, sizeof(ns)) != 0)
        return SWEEP_SKIPPED;
    json_get_long(buf, "watcher_pid", &watcher_pid);
    json_get_string(buf, "watcher_start", watcher_start, sizeof(watcher_start));

    int owner = process_state(owner_pid, owner_start, boot, ns);
    if (owner == PROC_ALIVE) return SWEEP_LIVE;
    if (owner == PROC_UNKNOWN) return SWEEP_SKIPPED;

    /* Owner dead.  A live watchdog is already cleaning up: leave it to it. */
    if (watcher_pid > 0 && process_state(watcher_pid, watcher_start, boot, ns) != PROC_DEAD)
        return SWEEP_LIVE;

    if (stop_server_in_dir(path) != 0) return SWEEP_FAILED;
    if (json_get_bool(buf, "owns_data_dir") == 1 && remove_tree(path) != 0)
        return SWEEP_FAILED;
    return SWEEP_RECLAIMED;
}

static int sweep_root(const char *temp_root, rpgl_sweep_result *out, int *old_legacy) {
    rpgl_sweep_result res;
    memset(&res, 0, sizeof(res));
    if (out) *out = res;
    if (old_legacy) *old_legacy = 0;
    if (!temp_root || !temp_root[0]) temp_root = temp_root_for(NULL);

    /*
     * One sweeper at a time, per user and root.  Non-blocking: if someone
     * else is sweeping, their sweep covers ours and we do not wait for it.
     */
    char lock_path[4096];
    snprintf(lock_path, sizeof(lock_path), "%s/rpgl_sweep.%d.lock", temp_root, (int)geteuid());
    int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock_fd < 0) return RPGL_ERR_INIT;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(lock_fd);
        return RPGL_ERR_ALREADY;
    }

    DIR *d = opendir(temp_root);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "rpgl_", 5) != 0) continue;
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", temp_root, de->d_name);
            int verdict = sweep_one(path);
            if (verdict < 0) continue;
            res.examined++;
            switch (verdict) {
                case SWEEP_LIVE:      res.live++; break;
                case SWEEP_DURABLE:   res.durable++; break;
                case SWEEP_LEGACY:    res.legacy++; if (old_legacy) (*old_legacy)++; break;
                case SWEEP_STARTING:  res.legacy++; break;
                case SWEEP_SKIPPED:   res.skipped++; break;
                case SWEEP_RECLAIMED: res.reclaimed++; break;
                case SWEEP_FAILED:    res.failed++; break;
            }
        }
        closedir(d);
    }

    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    if (out) *out = res;
    return RPGL_OK;
}

int rpgl_sweep(const char *temp_root, rpgl_sweep_result *out) {
    return sweep_root(temp_root, out, NULL);
}

/* The sweep every start runs.  Says once per process if it left legacy dirs. */
static void sweep_on_start(const char *temp_root) {
    static int legacy_reported = 0;
    rpgl_sweep_result res;
    int old_legacy = 0;
    if (sweep_root(temp_root, &res, &old_legacy) != RPGL_OK) return;
    if (old_legacy > 0 && !legacy_reported) {
        legacy_reported = 1;
        fprintf(stderr,
                "rustypglite: left %d old data dir(s) in %s alone: they have no owner.json, "
                "so an older rustypglite made them and nothing can say whether their "
                "owner is alive. Stop one with `rustypglite stop <dir>`.\n",
                old_legacy, temp_root);
    }
}

int rpgl_stop_dir(const char *data_dir) {
    if (!data_dir || !data_dir[0]) return RPGL_ERR_INIT;

    struct stat st;
    if (lstat(data_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return RPGL_ERR_INIT;
    if (st.st_uid != geteuid()) return RPGL_ERR_INIT;

    char *buf = malloc(OWNER_JSON_MAX);
    if (!buf) return RPGL_ERR_OOM;
    int owns = read_owner_file(data_dir, buf, OWNER_JSON_MAX) == 0
            && json_get_bool(buf, "owns_data_dir") == 1;
    free(buf);

    if (stop_server_in_dir(data_dir) != 0) return RPGL_ERR_STOP;

    /* Delete only what owner.json says we created; never someone's own data dir. */
    if (owns && remove_tree(data_dir) != 0) return RPGL_ERR_STOP;
    return RPGL_OK;
}

/* ---- Public API ---- */

int rpgl_start(const rpgl_options *opts, rpgl_instance **out) {
    if (!out) return RPGL_ERR_INIT;

    const char *temp_root = temp_root_for(opts);
    int durable = opts ? opts->durable : 0;

    /* Reclaim servers whose owners died (and nothing else — see sweep_one). */
    sweep_on_start(temp_root);

    char *bin_dir = find_pg_bin_dir();
    if (!bin_dir) return RPGL_ERR_INIT;

    rpgl_instance *inst = calloc(1, sizeof(rpgl_instance));
    if (!inst) { free(bin_dir); return RPGL_ERR_OOM; }

    inst->pg_bin_dir = bin_dir;
    inst->watcher_fd = -1;
    inst->durable = durable;
    random_token(inst->token, sizeof(inst->token));
    inst->db_name = strdup((opts && opts->db_name) ? opts->db_name : "postgres");
    inst->port = (opts && opts->port > 0) ? opts->port : find_free_port();

    int silent = opts ? opts->silent : 1;
    int keep_data = opts ? opts->keep_data : 0;

    /* ── Data directory ── */
    if (opts && opts->data_dir) {
        inst->data_dir = strdup(opts->data_dir);
        inst->owns_data_dir = 0;
    } else {
        char tmpl[4096];
        snprintf(tmpl, sizeof(tmpl), "%s/rpgl_XXXXXX", temp_root);
        mkdir(temp_root, 0700);   /* a custom root may not exist yet */
        char *tmpdir = mkdtemp(tmpl);
        if (!tmpdir) {
            set_error(inst, "mkdtemp failed: %s", strerror(errno));
            rpgl_stop(inst);
            return RPGL_ERR_INIT;
        }
        inst->data_dir = strdup(tmpdir);
        inst->owns_data_dir = !keep_data;
    }

    /* Socket dir = data dir (postgres creates .s.PGSQL.PORT here) */
    inst->socket_dir = strdup(inst->data_dir);

    /* ── Set LD_LIBRARY_PATH for child processes ── */
    char lib_path[4096];
    snprintf(lib_path, sizeof(lib_path), "%s/../lib", bin_dir);
    setenv("LD_LIBRARY_PATH", lib_path, 1);

    /*
     * ── Bind the server to us, before anything can outlive us ──
     * From here on, however this process ends, the watchdog stops the server
     * and removes the dir.
     */
    if (!durable && spawn_watchdog(inst) != 0) {
        set_error(inst, "could not start the watchdog: %s", strerror(errno));
        rpgl_stop(inst);
        return RPGL_ERR_INIT;
    }

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

    /*
     * ── owner.json ── (after initdb, which refuses a non-empty directory)
     * The sweep reads this to decide whether our server may be reclaimed.
     */
    if (write_owner_file(inst) != 0) {
        set_error(inst, "could not write %s: %s", OWNER_FILE, strerror(errno));
        rpgl_stop(inst);
        return RPGL_ERR_INIT;
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
        /*
         * pg_ctl DAEMONISES the postmaster: it forks, setsid()s, execs
         * /bin/sh -c "exec postgres ...", and exits.  So the postmaster's
         * parent is init (PPID 1) even while we are alive — a PPID of 1 is
         * NOT evidence of an orphan.  owner.json is.
         */
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

        inst->pg_pid = postmaster_pid(inst->data_dir);
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

    /* A durable server outlives us on purpose: no stop at exit either. */
    if (!durable) track_instance(inst);
    *out = inst;
    return RPGL_OK;
}

static void free_instance(rpgl_instance *inst) {
    free(inst->data_dir);
    free(inst->socket_dir);
    free(inst->db_name);
    free(inst->pg_bin_dir);
    free(inst->conn_string);
    free(inst);
}

int rpgl_stop(rpgl_instance *inst) {
    if (!inst) return RPGL_ERR_INIT;

    untrack_instance(inst);

    /* Attached with rpgl_connect_existing: someone else's server. */
    if (inst->borrowed) {
        free_instance(inst);
        return RPGL_OK;
    }

    int rc = RPGL_OK;
    if (inst->data_dir) {
        if (stop_server_in_dir(inst->data_dir) != 0) {
            rc = RPGL_ERR_STOP;
        } else if (inst->owns_data_dir) {
            /*
             * Only a dir that is still this start's (owner.json carries our
             * token), or one we never got as far as claiming.  Someone may
             * have stopped it with rpgl_stop_dir and the name been reused.
             */
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", inst->data_dir, OWNER_FILE);
            struct stat st;
            int unclaimed = lstat(path, &st) != 0 && errno == ENOENT;
            if (unclaimed || owner_token_is(inst->data_dir, inst->token))
                remove_tree(inst->data_dir);
        } else if (owner_token_is(inst->data_dir, inst->token)) {
            /* A dir we keep: leave no stale claim on it behind. */
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", inst->data_dir, OWNER_FILE);
            unlink(path);
        }
    }

    if (rc == RPGL_OK) {
        /* Only now: the watchdog must not stand down before the server is down. */
        release_watchdog(inst);
    } else if (inst->watcher_fd >= 0) {
        /* We could not stop it: hang up without "released", so the watchdog tries. */
        close(inst->watcher_fd);
        inst->watcher_fd = -1;
    }

    free_instance(inst);
    return rc;
}

int rpgl_detach(rpgl_instance *inst) {
    if (!inst) return RPGL_ERR_INIT;
    untrack_instance(inst);
    /*
     * A non-durable server stays bound to this process: its watchdog socket
     * is deliberately left open, so the server still goes when we do.
     */
    free_instance(inst);
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
    inst->borrowed = 1;       /* don't stop — we didn't start it */
    inst->pg_pid = 0;
    inst->watcher_fd = -1;

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
