use std::ffi::CStr;

pub type Result<T> = std::result::Result<T, Error>;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("initialization failed: {0}")]
    Init(String),

    #[error("server start failed: {0}")]
    Start(String),

    #[error("server stop failed: {0}")]
    Stop(String),

    #[error("internal error: {0}")]
    Internal(String),

    #[error("out of memory")]
    OutOfMemory,

    #[error("timeout waiting for postgres: {0}")]
    Timeout(String),
}

impl Error {
    pub(crate) fn from_code(code: i32, inst: *mut rustypglite_sys::rpgl_instance) -> Error {
        let msg = if !inst.is_null() {
            let ptr = unsafe { rustypglite_sys::rpgl_last_error(inst) };
            if ptr.is_null() {
                String::new()
            } else {
                unsafe { CStr::from_ptr(ptr) }
                    .to_string_lossy()
                    .into_owned()
            }
        } else {
            String::new()
        };

        match code {
            rustypglite_sys::RPGL_ERR_INIT => Error::Init(msg),
            rustypglite_sys::RPGL_ERR_START => Error::Start(msg),
            rustypglite_sys::RPGL_ERR_STOP => Error::Stop(msg),
            rustypglite_sys::RPGL_ERR_INTERNAL => Error::Internal(msg),
            rustypglite_sys::RPGL_ERR_OOM => Error::OutOfMemory,
            rustypglite_sys::RPGL_ERR_TIMEOUT => Error::Timeout(msg),
            _ => Error::Internal(format!("unknown error code: {}", code)),
        }
    }
}
