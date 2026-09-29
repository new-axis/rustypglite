//! # RustyPGlite — Embedded PostgreSQL Server
//!
//! Starts a real PostgreSQL server that listens on a unix socket.
//! Any standard Postgres client (Npgsql, node-pg, psycopg2) connects normally.
//!
//! ## Quick Start
//!
//! ```no_run
//! use rustypglite::EmbeddedPg;
//!
//! let pg = EmbeddedPg::start().unwrap();
//! println!("Connect with: {}", pg.connection_string());
//! // Use any Postgres client library with this connection string.
//! // Server stops automatically when `pg` is dropped.
//! ```
//!
//! ## No leaked servers
//!
//! A server never outlives the process that started it. Dropping the handle
//! stops it; so does a normal exit; and if the process is killed outright
//! (SIGKILL, a crash, a cancelled test run) a small watchdog process stops the
//! server and removes its data dir. Each start also sweeps the temp root for
//! servers whose owner is provably dead (see [`sweep`]). For a server that
//! must outlive its starter, ask for one: [`StartOptions::durable`].

mod error;

pub use error::{Error, Result};

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};

/// An embedded PostgreSQL server instance.
///
/// Manages the full lifecycle: initdb → start → accept connections → stop → cleanup.
/// The server listens on a unix socket (no TCP) and is configured for maximum
/// testing speed (fsync=off, synchronous_commit=off, etc.).
pub struct EmbeddedPg {
    handle: *mut rustypglite_sys::rpgl_instance,
    durable: bool,
}

unsafe impl Send for EmbeddedPg {}

impl EmbeddedPg {
    /// Start an embedded PostgreSQL server with default settings.
    /// Creates a temp directory, runs initdb, starts postgres.
    pub fn start() -> Result<Self> {
        let mut handle: *mut rustypglite_sys::rpgl_instance = std::ptr::null_mut();
        let rc = unsafe { rustypglite_sys::rpgl_start(std::ptr::null(), &mut handle) };
        if rc != rustypglite_sys::RPGL_OK {
            return Err(Error::from_code(rc, handle));
        }
        Ok(EmbeddedPg { handle, durable: false })
    }

    /// Start with custom options.
    pub fn start_with(opts: StartOptions) -> Result<Self> {
        let c_data_dir = opts.data_dir.as_ref().map(|s| CString::new(s.as_str()).unwrap());
        let c_db_name = opts.db_name.as_ref().map(|s| CString::new(s.as_str()).unwrap());
        let c_temp_root = opts.temp_root.as_ref().map(|s| CString::new(s.as_str()).unwrap());

        let c_opts = rustypglite_sys::rpgl_options {
            data_dir: c_data_dir.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            db_name: c_db_name.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            port: opts.port.unwrap_or(0) as c_int,
            silent: if opts.silent { 1 } else { 0 },
            keep_data: if opts.keep_data { 1 } else { 0 },
            durable: if opts.durable { 1 } else { 0 },
            temp_root: c_temp_root.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        };

        let mut handle: *mut rustypglite_sys::rpgl_instance = std::ptr::null_mut();
        let rc = unsafe { rustypglite_sys::rpgl_start(&c_opts, &mut handle) };
        if rc != rustypglite_sys::RPGL_OK {
            return Err(Error::from_code(rc, handle));
        }
        Ok(EmbeddedPg { handle, durable: opts.durable })
    }

    /// Connect to an already-running postgres instance by its data directory.
    /// Reads postmaster.pid to discover port and socket. Does not start a server,
    /// and dropping the handle does not stop it.
    /// Use for shared-server-across-workers patterns.
    pub fn connect_existing(data_dir: &str) -> Result<Self> {
        let c_dir = CString::new(data_dir).map_err(|_| Error::Init("invalid path".into()))?;
        let mut handle: *mut rustypglite_sys::rpgl_instance = std::ptr::null_mut();
        let rc = unsafe { rustypglite_sys::rpgl_connect_existing(c_dir.as_ptr(), &mut handle) };
        if rc != rustypglite_sys::RPGL_OK {
            return Err(Error::from_code(rc, handle));
        }
        Ok(EmbeddedPg { handle, durable: false })
    }

    /// Get the connection string for this instance.
    /// Format: "host=/tmp/rpgl_xxx;port=NNNNN;database=postgres;username=postgres"
    pub fn connection_string(&self) -> &str {
        unsafe {
            let ptr = rustypglite_sys::rpgl_connection_string(self.handle);
            if ptr.is_null() {
                return "";
            }
            CStr::from_ptr(ptr).to_str().unwrap_or("")
        }
    }

    /// Get the unix socket directory path.
    pub fn socket_dir(&self) -> &str {
        unsafe {
            let ptr = rustypglite_sys::rpgl_socket_dir(self.handle);
            if ptr.is_null() { "" } else { CStr::from_ptr(ptr).to_str().unwrap_or("") }
        }
    }

    /// Get the port number.
    pub fn port(&self) -> i32 {
        unsafe { rustypglite_sys::rpgl_port(self.handle) as i32 }
    }

    /// Get the data directory path.
    pub fn data_dir(&self) -> &str {
        unsafe {
            let ptr = rustypglite_sys::rpgl_data_dir(self.handle);
            if ptr.is_null() { "" } else { CStr::from_ptr(ptr).to_str().unwrap_or("") }
        }
    }

    /// Create a new database on this instance.
    pub fn create_database(&self, name: &str) -> Result<()> {
        let c_name = CString::new(name).map_err(|_| Error::Init("invalid db name".into()))?;
        let rc = unsafe { rustypglite_sys::rpgl_create_database(self.handle, c_name.as_ptr()) };
        if rc == rustypglite_sys::RPGL_OK {
            Ok(())
        } else {
            Err(Error::from_code(rc, self.handle))
        }
    }

    /// Execute SQL directly (via psql). Useful for migrations/DDL setup.
    pub fn exec_sql(&self, sql: &str) -> Result<()> {
        self.exec_sql_on(None, sql)
    }

    /// Execute SQL on a specific database.
    pub fn exec_sql_on(&self, db_name: Option<&str>, sql: &str) -> Result<()> {
        let c_sql = CString::new(sql).map_err(|_| Error::Init("invalid SQL".into()))?;
        let c_db = db_name.map(|s| CString::new(s).unwrap());
        let db_ptr = c_db.as_ref().map_or(std::ptr::null(), |s| s.as_ptr());

        let rc = unsafe { rustypglite_sys::rpgl_exec_sql(self.handle, db_ptr, c_sql.as_ptr()) };
        if rc == rustypglite_sys::RPGL_OK {
            Ok(())
        } else {
            Err(Error::from_code(rc, self.handle))
        }
    }

    /// Execute a SQL file.
    pub fn exec_file(&self, db_name: Option<&str>, file_path: &str) -> Result<()> {
        let c_path = CString::new(file_path).map_err(|_| Error::Init("invalid path".into()))?;
        let c_db = db_name.map(|s| CString::new(s).unwrap());
        let db_ptr = c_db.as_ref().map_or(std::ptr::null(), |s| s.as_ptr());

        let rc =
            unsafe { rustypglite_sys::rpgl_exec_file(self.handle, db_ptr, c_path.as_ptr()) };
        if rc == rustypglite_sys::RPGL_OK {
            Ok(())
        } else {
            Err(Error::from_code(rc, self.handle))
        }
    }

    /// Whether this server was started durable (see [`StartOptions::durable`]).
    pub fn is_durable(&self) -> bool {
        self.durable
    }

    /// Stop the server and clean up. Stops a durable server too: this is the
    /// explicit stop.
    pub fn stop(mut self) {
        let handle = std::mem::replace(&mut self.handle, std::ptr::null_mut());
        if !handle.is_null() {
            unsafe { rustypglite_sys::rpgl_stop(handle) };
        }
    }

    /// Let go of the handle without stopping the server. A durable server
    /// keeps running after this process exits; a non-durable one is still
    /// stopped when this process exits.
    pub fn detach(mut self) {
        let handle = std::mem::replace(&mut self.handle, std::ptr::null_mut());
        if !handle.is_null() {
            unsafe { rustypglite_sys::rpgl_detach(handle) };
        }
    }
}

impl Drop for EmbeddedPg {
    /// Stops the server — unless it is durable, which by definition is not
    /// tied to the lifetime of this handle or process: then it detaches.
    fn drop(&mut self) {
        let handle = std::mem::replace(&mut self.handle, std::ptr::null_mut());
        if !handle.is_null() {
            unsafe {
                if self.durable {
                    rustypglite_sys::rpgl_detach(handle);
                } else {
                    rustypglite_sys::rpgl_stop(handle);
                }
            }
        }
    }
}

/// Stop the server running in `data_dir`, from any process — the explicit
/// stop for scripts (`rustypglite stop <dir>`). Deletes the dir only when its
/// `owner.json` says rustypglite created it; never a data dir you supplied.
pub fn stop_dir(data_dir: &str) -> Result<()> {
    let c_dir = CString::new(data_dir).map_err(|_| Error::Init("invalid path".into()))?;
    let rc = unsafe { rustypglite_sys::rpgl_stop_dir(c_dir.as_ptr()) };
    match rc {
        rustypglite_sys::RPGL_OK => Ok(()),
        rustypglite_sys::RPGL_ERR_STOP => {
            Err(Error::Stop(format!("the server in {} did not stop", data_dir)))
        }
        _ => Err(Error::Init(format!("{} is not a data dir of yours", data_dir))),
    }
}

/// What a [`sweep`] found. See `rpgl_sweep_result` in `pg_shim.h`.
pub type SweepReport = rustypglite_sys::rpgl_sweep_result;

/// Reclaim servers whose owner is provably dead, under `temp_root` (None =
/// `$RUSTYPGLITE_TMPDIR`, else `/tmp`). Touches only `rpgl_*` dirs whose
/// `owner.json` shows the owner (PID + start time) and its watchdog are both
/// gone — never a live owner's dir however old, a durable one, or one without
/// `owner.json`. Every start already does this; call it to report or to
/// sweep without starting. `Ok(None)` if another process is sweeping now.
pub fn sweep(temp_root: Option<&str>) -> Result<Option<SweepReport>> {
    let c_root = temp_root
        .map(CString::new)
        .transpose()
        .map_err(|_| Error::Init("invalid path".into()))?;
    let mut report = SweepReport::default();
    let rc = unsafe {
        rustypglite_sys::rpgl_sweep(
            c_root.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            &mut report,
        )
    };
    match rc {
        rustypglite_sys::RPGL_OK => Ok(Some(report)),
        rustypglite_sys::RPGL_ERR_ALREADY => Ok(None),
        _ => Err(Error::Init("could not sweep (temp root missing or unwritable?)".into())),
    }
}

/// Options for starting an embedded PostgreSQL instance.
#[derive(Default)]
pub struct StartOptions {
    /// Data directory. None = auto temp directory.
    pub data_dir: Option<String>,
    /// Database name. None = "postgres".
    pub db_name: Option<String>,
    /// Port number. None = auto-assign free port.
    pub port: Option<u16>,
    /// Suppress postgres log output. Default: true.
    pub silent: bool,
    /// Keep data directory after stop. Default: false.
    pub keep_data: bool,
    /// A long-lived server NOT bound to this process: no watchdog, not stopped
    /// on exit or drop, never reclaimed by a sweep (`"durable": true` in its
    /// `owner.json`). Stop it with [`EmbeddedPg::stop`] or [`stop_dir`].
    /// Default: false.
    pub durable: bool,
    /// Where auto data dirs go and what the start-up sweep scans.
    /// None = `$RUSTYPGLITE_TMPDIR`, else `/tmp`.
    pub temp_root: Option<String>,
}

// ---- C ABI exports for FFI consumers (.NET, Node.js, etc.) ----

#[no_mangle]
pub extern "C" fn rpglite_start() -> *mut EmbeddedPg {
    match EmbeddedPg::start() {
        Ok(pg) => Box::into_raw(Box::new(pg)),
        Err(_) => std::ptr::null_mut(),
    }
}

/// Start with options. Null pointers and 0 mean "default"; nonzero flags mean yes.
#[no_mangle]
pub extern "C" fn rpglite_start_with(
    data_dir: *const c_char,
    db_name: *const c_char,
    port: c_int,
    keep_data: c_int,
    durable: c_int,
    temp_root: *const c_char,
) -> *mut EmbeddedPg {
    let opt = |p: *const c_char| {
        if p.is_null() {
            None
        } else {
            Some(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
        }
    };
    let opts = StartOptions {
        data_dir: opt(data_dir),
        db_name: opt(db_name),
        port: if port > 0 { Some(port as u16) } else { None },
        silent: true,
        keep_data: keep_data != 0,
        durable: durable != 0,
        temp_root: opt(temp_root),
    };
    match EmbeddedPg::start_with(opts) {
        Ok(pg) => Box::into_raw(Box::new(pg)),
        Err(_) => std::ptr::null_mut(),
    }
}

/// Stop the server, durable or not, and free the handle.
#[no_mangle]
pub extern "C" fn rpglite_stop_server(pg: *mut EmbeddedPg) {
    if !pg.is_null() {
        unsafe { Box::from_raw(pg) }.stop();
    }
}

/// Free the handle without stopping the server (see [`EmbeddedPg::detach`]).
#[no_mangle]
pub extern "C" fn rpglite_detach(pg: *mut EmbeddedPg) {
    if !pg.is_null() {
        unsafe { Box::from_raw(pg) }.detach();
    }
}

/// 0 when no server is left running in `data_dir`; -4 if it would not stop;
/// -1 if `data_dir` is not a data dir of this user's.
#[no_mangle]
pub extern "C" fn rpglite_stop_dir(data_dir: *const c_char) -> c_int {
    if data_dir.is_null() {
        return rustypglite_sys::RPGL_ERR_INIT;
    }
    unsafe { rustypglite_sys::rpgl_stop_dir(data_dir) }
}

/// Fills `out` (may be null); 0 on success, -2 if another process is sweeping.
#[no_mangle]
pub extern "C" fn rpglite_sweep(
    temp_root: *const c_char,
    out: *mut rustypglite_sys::rpgl_sweep_result,
) -> c_int {
    unsafe { rustypglite_sys::rpgl_sweep(temp_root, out) }
}

#[no_mangle]
pub extern "C" fn rpglite_connect_existing(data_dir: *const c_char) -> *mut EmbeddedPg {
    if data_dir.is_null() {
        return std::ptr::null_mut();
    }
    let path = unsafe { CStr::from_ptr(data_dir) }.to_str().unwrap_or("");
    match EmbeddedPg::connect_existing(path) {
        Ok(pg) => Box::into_raw(Box::new(pg)),
        Err(_) => std::ptr::null_mut(),
    }
}

/// Drop the handle: stops a non-durable server, detaches from a durable one.
#[no_mangle]
pub extern "C" fn rpglite_stop(pg: *mut EmbeddedPg) {
    if !pg.is_null() {
        unsafe { drop(Box::from_raw(pg)) };
    }
}

#[no_mangle]
pub extern "C" fn rpglite_connection_string(pg: *mut EmbeddedPg) -> *const c_char {
    if pg.is_null() {
        return std::ptr::null();
    }
    let pg = unsafe { &*pg };
    unsafe { rustypglite_sys::rpgl_connection_string(pg.handle) }
}

#[no_mangle]
pub extern "C" fn rpglite_socket_dir(pg: *mut EmbeddedPg) -> *const c_char {
    if pg.is_null() {
        return std::ptr::null();
    }
    let pg = unsafe { &*pg };
    unsafe { rustypglite_sys::rpgl_socket_dir(pg.handle) }
}

#[no_mangle]
pub extern "C" fn rpglite_port(pg: *mut EmbeddedPg) -> c_int {
    if pg.is_null() {
        return 0;
    }
    let pg = unsafe { &*pg };
    unsafe { rustypglite_sys::rpgl_port(pg.handle) }
}

#[no_mangle]
pub extern "C" fn rpglite_data_dir(pg: *mut EmbeddedPg) -> *const c_char {
    if pg.is_null() {
        return std::ptr::null();
    }
    let pg = unsafe { &*pg };
    unsafe { rustypglite_sys::rpgl_data_dir(pg.handle) }
}

#[no_mangle]
pub extern "C" fn rpglite_create_database(pg: *mut EmbeddedPg, name: *const c_char) -> c_int {
    if pg.is_null() || name.is_null() {
        return -1;
    }
    let pg = unsafe { &*pg };
    let name = unsafe { CStr::from_ptr(name) }.to_str().unwrap_or("");
    match pg.create_database(name) {
        Ok(_) => 0,
        Err(_) => -1,
    }
}

#[no_mangle]
pub extern "C" fn rpglite_exec_sql(
    pg: *mut EmbeddedPg,
    db_name: *const c_char,
    sql: *const c_char,
) -> c_int {
    if pg.is_null() || sql.is_null() {
        return -1;
    }
    let pg = unsafe { &*pg };
    let sql = unsafe { CStr::from_ptr(sql) }.to_str().unwrap_or("");
    let db = if db_name.is_null() {
        None
    } else {
        Some(unsafe { CStr::from_ptr(db_name) }.to_str().unwrap_or("postgres"))
    };
    match pg.exec_sql_on(db, sql) {
        Ok(_) => 0,
        Err(_) => -1,
    }
}
