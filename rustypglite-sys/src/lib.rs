//! Low-level FFI bindings to the embedded PostgreSQL lifecycle manager.
//!
//! This crate provides raw C FFI declarations that match the `pg_shim.h` API.
//! Users should prefer the safe `rustypglite` crate instead.

#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int};

// Error codes
pub const RPGL_OK: c_int = 0;
pub const RPGL_ERR_INIT: c_int = -1;
pub const RPGL_ERR_ALREADY: c_int = -2;
pub const RPGL_ERR_START: c_int = -3;
pub const RPGL_ERR_STOP: c_int = -4;
pub const RPGL_ERR_INTERNAL: c_int = -5;
pub const RPGL_ERR_OOM: c_int = -6;
pub const RPGL_ERR_TIMEOUT: c_int = -7;

/// Opaque instance handle.
#[repr(C)]
pub struct rpgl_instance {
    _opaque: [u8; 0],
}

/// Options for starting an embedded postgres.
#[repr(C)]
pub struct rpgl_options {
    pub data_dir: *const c_char,
    pub db_name: *const c_char,
    pub port: c_int,
    pub silent: c_int,
    pub keep_data: c_int,
}

extern "C" {
    pub fn rpgl_start(opts: *const rpgl_options, out: *mut *mut rpgl_instance) -> c_int;
    pub fn rpgl_connect_existing(data_dir: *const c_char, out: *mut *mut rpgl_instance) -> c_int;
    pub fn rpgl_stop(inst: *mut rpgl_instance) -> c_int;

    pub fn rpgl_connection_string(inst: *mut rpgl_instance) -> *const c_char;
    pub fn rpgl_socket_dir(inst: *mut rpgl_instance) -> *const c_char;
    pub fn rpgl_port(inst: *mut rpgl_instance) -> c_int;
    pub fn rpgl_data_dir(inst: *mut rpgl_instance) -> *const c_char;

    pub fn rpgl_last_error(inst: *mut rpgl_instance) -> *const c_char;

    pub fn rpgl_create_database(inst: *mut rpgl_instance, name: *const c_char) -> c_int;
    pub fn rpgl_exec_sql(
        inst: *mut rpgl_instance,
        db_name: *const c_char,
        sql: *const c_char,
    ) -> c_int;
    pub fn rpgl_exec_file(
        inst: *mut rpgl_instance,
        db_name: *const c_char,
        file_path: *const c_char,
    ) -> c_int;
}
