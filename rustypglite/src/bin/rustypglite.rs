//! `rustypglite` — stop a server by its data dir, or sweep for dead owners' servers.
//!
//! ```text
//! rustypglite stop <data-dir>...     stop exactly these servers (e.g. from `run.sh down`)
//! rustypglite sweep [--root <dir>]   reclaim servers whose owner is dead, and report
//! ```
//!
//! Needs no Postgres binaries: stopping is by signal to the postmaster named in
//! the dir's postmaster.pid.

use std::process::ExitCode;

const USAGE: &str = "usage:
  rustypglite stop <data-dir>...     stop the server in each dir; remove the dir
                                     only if rustypglite created it
  rustypglite sweep [--root <dir>]   stop and remove servers whose owner is dead
                                     (default root: $RUSTYPGLITE_TMPDIR, else /tmp)";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("stop") if args.len() > 1 => {
            let mut ok = true;
            for dir in &args[1..] {
                match rustypglite::stop_dir(dir) {
                    Ok(()) => println!("stopped {}", dir),
                    Err(e) => {
                        eprintln!("rustypglite: {}", e);
                        ok = false;
                    }
                }
            }
            if ok { ExitCode::SUCCESS } else { ExitCode::FAILURE }
        }
        Some("sweep") => {
            let root = match &args[1..] {
                [] => None,
                [flag, dir] if flag == "--root" => Some(dir.as_str()),
                _ => {
                    eprintln!("{}", USAGE);
                    return ExitCode::from(2);
                }
            };
            match rustypglite::sweep(root) {
                Ok(Some(r)) => {
                    println!(
                        "examined {}: reclaimed {}, live {}, durable {}, \
                         legacy (no owner.json) {}, skipped {}, failed {}",
                        r.examined, r.reclaimed, r.live, r.durable, r.legacy, r.skipped, r.failed
                    );
                    if r.failed > 0 { ExitCode::FAILURE } else { ExitCode::SUCCESS }
                }
                Ok(None) => {
                    println!("another process is sweeping this root right now");
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    eprintln!("rustypglite: {}", e);
                    ExitCode::FAILURE
                }
            }
        }
        _ => {
            eprintln!("{}", USAGE);
            ExitCode::from(2)
        }
    }
}
