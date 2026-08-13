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
        Ok(EmbeddedPg { handle })
    }

    /// Start with custom options.
    pub fn start_with(opts: StartOptions) -> Result<Self> {
        let c_data_dir = opts.data_dir.as_ref().map(|s| CString::new(s.as_str()).unwrap());
        let c_db_name = opts.db_name.as_ref().map(|s| CString::new(s.as_str()).unwrap());

        let c_opts = rustypglite_sys::rpgl_options {
            data_dir: c_data_dir.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            db_name: c_db_name.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            port: opts.port.unwrap_or(0) as c_int,
            silent: if opts.silent { 1 } else { 0 },
            keep_data: if opts.keep_data { 1 } else { 0 },
        };

        let mut handle: *mut rustypglite_sys::rpgl_instance = std::ptr::null_mut();
        let rc = unsafe { rustypglite_sys::rpgl_start(&c_opts, &mut handle) };
        if rc != rustypglite_sys::RPGL_OK {
            return Err(Error::from_code(rc, handle));
        }
        Ok(EmbeddedPg { handle })
    }

    /// Connect to an already-running postgres instance by its data directory.
    /// Reads postmaster.pid to discover port and socket. Does not start a server.
    /// Use for shared-server-across-workers patterns.
    pub fn connect_existing(data_dir: &str) -> Result<Self> {
        let c_dir = CString::new(data_dir).map_err(|_| Error::Init("invalid path".into()))?;
        let mut handle: *mut rustypglite_sys::rpgl_instance = std::ptr::null_mut();
        let rc = unsafe { rustypglite_sys::rpgl_connect_existing(c_dir.as_ptr(), &mut handle) };
        if rc != rustypglite_sys::RPGL_OK {
            return Err(Error::from_code(rc, handle));
        }
        Ok(EmbeddedPg { handle })
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

    /// Stop the server and clean up.
    pub fn stop(self) {
        // Drop handles it
        drop(self);
    }
}

impl Drop for EmbeddedPg {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { rustypglite_sys::rpgl_stop(self.handle) };
            self.handle = std::ptr::null_mut();
        }
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
}

// ---- C ABI exports for FFI consumers (.NET, Node.js, etc.) ----

#[no_mangle]
pub extern "C" fn rpglite_start() -> *mut EmbeddedPg {
    match EmbeddedPg::start() {
        Ok(pg) => Box::into_raw(Box::new(pg)),
        Err(_) => std::ptr::null_mut(),
    }
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
