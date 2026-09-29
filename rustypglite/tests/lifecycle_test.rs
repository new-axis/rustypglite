//! No leaked servers: a server dies with its owner however the owner ends,
//! the start-up sweep reclaims only provably dead owners' servers, and a
//! durable server is left alone until it is stopped explicitly.
//!
//! Every test works in its own temp root (never the real /tmp/rpgl_*), so
//! the sweeps here cannot touch anyone else's servers. The root's parent is
//! $RUSTYPGLITE_TEST_ROOT, else /tmp — kept short, because the unix socket
//! path inside it is limited to ~100 bytes (and macOS's temp_dir() is long).
//!
//! "Owner" processes that get killed are this test binary, re-run with
//! RPGL_CHILD_ROOT set so that only `owner_child` does anything.

use rustypglite::{EmbeddedPg, StartOptions};
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

// ---- helpers ----

struct Root(PathBuf);

impl Root {
    fn new(name: &str) -> Root {
        let base = std::env::var("RUSTYPGLITE_TEST_ROOT").unwrap_or_else(|_| "/tmp".into());
        let dir = PathBuf::from(base).join(format!("rpgltest-{}-{}", std::process::id(), name));
        std::fs::create_dir_all(&dir).unwrap();
        Root(dir)
    }
    fn path(&self) -> &str {
        self.0.to_str().unwrap()
    }
}

impl Drop for Root {
    fn drop(&mut self) {
        // Stop anything a failed test left behind, then remove the root.
        if let Ok(entries) = std::fs::read_dir(&self.0) {
            for e in entries.flatten() {
                if e.path().is_dir() {
                    let _ = rustypglite::stop_dir(e.path().to_str().unwrap());
                }
            }
        }
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// An owner process; killed on drop if a test fails before killing it.
struct Owner {
    child: Child,
    dir: String,
}

impl Owner {
    fn start(root: &Root, durable: bool) -> Owner {
        let mut child = Command::new(std::env::current_exe().unwrap())
            .args(["owner_child", "--exact", "--nocapture", "--test-threads=1"])
            .env("RPGL_CHILD_ROOT", root.path())
            .env("RPGL_CHILD_DURABLE", if durable { "1" } else { "0" })
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let stdout = child.stdout.take().unwrap();
        for line in BufReader::new(stdout).lines() {
            let line = line.unwrap();
            // The harness prints "test owner_child ... " first, on the same line.
            if let Some((_, dir)) = line.split_once("RPGL_DIR=") {
                return Owner { child, dir: dir.to_string() };
            }
        }
        let _ = child.kill();
        panic!("owner child exited without starting a server");
    }

    fn kill9(&mut self) {
        signal("-KILL", self.child.id());
        let _ = self.child.wait();
    }
}

impl Drop for Owner {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn signal(sig: &str, pid: u32) {
    let ok = Command::new("kill").args([sig, &pid.to_string()]).status().unwrap().success();
    assert!(ok, "kill {} {} failed", sig, pid);
}

/// Running, and not a zombie.
fn alive(pid: u32) -> bool {
    if pid == 0 {
        return false;
    }
    #[cfg(target_os = "linux")]
    {
        match std::fs::read_to_string(format!("/proc/{}/stat", pid)) {
            Ok(stat) => {
                let state = stat.rsplit_once(") ").map(|(_, rest)| rest.chars().next());
                state != Some(Some('Z'))
            }
            Err(_) => false,
        }
    }
    #[cfg(not(target_os = "linux"))]
    {
        let out = Command::new("ps").args(["-o", "stat=", "-p", &pid.to_string()]).output().unwrap();
        let stat = String::from_utf8_lossy(&out.stdout);
        out.status.success() && !stat.trim().is_empty() && !stat.trim_start().starts_with('Z')
    }
}

fn postmaster_pid(dir: &str) -> u32 {
    std::fs::read_to_string(Path::new(dir).join("postmaster.pid"))
        .ok()
        .and_then(|s| s.lines().next().and_then(|l| l.trim().parse().ok()))
        .unwrap_or(0)
}

/// A field of owner.json, as its raw text (strings without quotes).
fn owner_field(dir: &str, key: &str) -> String {
    let json = std::fs::read_to_string(Path::new(dir).join("owner.json")).unwrap();
    let needle = format!("\"{}\":", key);
    let at = json.find(&needle).unwrap_or_else(|| panic!("no {} in owner.json", key));
    let rest = json[at + needle.len()..].trim_start();
    let end = rest.find([',', '\n']).unwrap();
    rest[..end].trim().trim_matches('"').to_string()
}

fn wait_until(timeout: Duration, mut cond: impl FnMut() -> bool) -> bool {
    let start = Instant::now();
    while start.elapsed() < timeout {
        if cond() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    cond()
}

fn start_in(root: &Root) -> EmbeddedPg {
    EmbeddedPg::start_with(StartOptions {
        temp_root: Some(root.path().to_string()),
        silent: true,
        ..Default::default()
    })
    .expect("start")
}

fn fake_dir(root: &Root, name: &str, owner_json: Option<String>) -> PathBuf {
    use std::os::unix::fs::PermissionsExt;
    let dir = root.0.join(name);
    std::fs::create_dir_all(&dir).unwrap();
    // 0700, as mkdtemp makes them: the sweep will not trust a dir others can write.
    std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700)).unwrap();
    std::fs::write(dir.join("PG_VERSION"), "17\n").unwrap();
    if let Some(json) = owner_json {
        std::fs::write(dir.join("owner.json"), json).unwrap();
    }
    dir
}

// ---- the owner process ----

#[test]
fn owner_child() {
    let Ok(root) = std::env::var("RPGL_CHILD_ROOT") else { return };
    let durable = std::env::var("RPGL_CHILD_DURABLE").as_deref() == Ok("1");
    let pg = EmbeddedPg::start_with(StartOptions {
        temp_root: Some(root),
        durable,
        silent: true,
        ..Default::default()
    })
    .expect("owner child: start");
    println!("RPGL_DIR={}", pg.data_dir());
    use std::io::Write;
    std::io::stdout().flush().unwrap();
    if std::env::var("RPGL_CHILD_FORK_EXIT").as_deref() == Ok("1") {
        // A child that forks WITHOUT exec and then exit()s runs the atexit
        // handlers it inherited. They must not touch the parent's server.
        extern "C" {
            fn fork() -> i32;
            fn exit(code: i32) -> !;
            fn waitpid(pid: i32, status: *mut i32, options: i32) -> i32;
        }
        let pid = unsafe { fork() };
        if pid == 0 {
            unsafe { exit(0) };
        }
        let mut status = 0;
        unsafe { waitpid(pid, &mut status, 0) };
        println!("RPGL_FORKED=1");
        std::io::stdout().flush().unwrap();
    }
    if std::env::var("RPGL_CHILD_FORK_HOLD").as_deref() == Ok("1") {
        // A child that forks WITHOUT exec and lives on must not keep the
        // server up once the real owner is gone.
        extern "C" {
            fn fork() -> i32;
            fn _exit(code: i32) -> !;
        }
        let pid = unsafe { fork() };
        if pid == 0 {
            for _ in 0..120 {
                std::thread::sleep(Duration::from_secs(1));
            }
            unsafe { _exit(0) };
        }
        println!("RPGL_FORKCHILD={}", pid);
        std::io::stdout().flush().unwrap();
    }
    loop {
        std::thread::sleep(Duration::from_secs(1)); // until killed
    }
}

// ---- tests ----

#[test]
fn kill9_of_the_owner_stops_the_server_and_removes_the_dir() {
    let root = Root::new("kill9");
    let mut owner = Owner::start(&root, false);
    let dir = owner.dir.clone();
    let pm = postmaster_pid(&dir);
    assert!(alive(pm), "server should be running");
    assert!(Path::new(&dir).exists());

    owner.kill9();

    assert!(
        wait_until(Duration::from_secs(15), || !alive(pm) && !Path::new(&dir).exists()),
        "after kill -9 of the owner: server alive={}, dir exists={}",
        alive(pm),
        Path::new(&dir).exists()
    );
}

#[test]
fn drop_stops_the_server_removes_the_dir_and_releases_the_watchdog() {
    let root = Root::new("drop");
    let pg = start_in(&root);
    let dir = pg.data_dir().to_string();
    let pm = postmaster_pid(&dir);
    let watcher: u32 = owner_field(&dir, "watcher_pid").parse().unwrap();
    assert!(alive(pm) && alive(watcher));

    drop(pg);

    assert!(!alive(pm), "server still running after drop");
    assert!(!Path::new(&dir).exists(), "dir still there after drop");
    assert!(wait_until(Duration::from_secs(3), || !alive(watcher)), "watchdog still running");
}

#[test]
fn owner_json_names_the_owner() {
    let root = Root::new("ownerjson");
    let pg = start_in(&root);
    let dir = pg.data_dir();
    assert!(dir.starts_with(root.path()), "auto dir should be under the temp root");
    assert_eq!(owner_field(dir, "owner_pid"), std::process::id().to_string());
    assert!(!owner_field(dir, "owner_start").is_empty());
    assert_eq!(owner_field(dir, "durable"), "false");
    assert_eq!(owner_field(dir, "owns_data_dir"), "true");
    assert!(owner_field(dir, "watcher_pid").parse::<u32>().unwrap() > 0);
    assert_eq!(owner_field(dir, "token").len(), 32);
}

#[test]
fn sweep_reclaims_only_dead_owners_servers() {
    let root = Root::new("sweep");

    // A: owner alive (this process).
    let live = start_in(&root);
    let live_dir = live.data_dir().to_string();
    let boot = owner_field(&live_dir, "boot_id");
    let ns = owner_field(&live_dir, "pid_ns");

    // B: owner AND its watchdog dead — the case only the sweep can clean up.
    // C: durable, owner dead — must survive, and the sweep must leave it.
    // Both start before either dies: every start sweeps, and C's must not
    // get to B first.
    let mut dead = Owner::start(&root, false);
    let mut durable = Owner::start(&root, true);

    let dead_dir = dead.dir.clone();
    let dead_pm = postmaster_pid(&dead_dir);
    let dead_watcher: u32 = owner_field(&dead_dir, "watcher_pid").parse().unwrap();
    // Freeze B's watchdog first so it cannot act, then kill both.
    signal("-STOP", dead_watcher);
    dead.kill9();
    signal("-KILL", dead_watcher);
    assert!(wait_until(Duration::from_secs(5), || !alive(dead_watcher)));
    assert!(alive(dead_pm), "B's server should be orphaned, not stopped");

    let durable_dir = durable.dir.clone();
    let durable_pm = postmaster_pid(&durable_dir);
    assert_eq!(owner_field(&durable_dir, "durable"), "true");
    durable.kill9();

    // D: legacy — no owner.json.
    let legacy = fake_dir(&root, "rpgl_legacy", None);

    // E: owner.json names this very process, but with the wrong start time:
    // the PID has been reused, so the real owner is dead.
    let reused = fake_dir(
        &root,
        "rpgl_reused",
        Some(format!(
            r#"{{"owner_pid": {}, "owner_start": "1", "boot_id": "{}", "pid_ns": "{}",
               "watcher_pid": 0, "watcher_start": "", "durable": false,
               "owns_data_dir": true, "token": "x"}}"#,
            std::process::id(),
            boot,
            ns
        )),
    );

    // F: an owner in another PID namespace — cannot be judged from here.
    let foreign = fake_dir(
        &root,
        "rpgl_foreign",
        Some(format!(
            r#"{{"owner_pid": 999999, "owner_start": "1", "boot_id": "{}", "pid_ns": "pid:[1]",
               "watcher_pid": 0, "watcher_start": "", "durable": false,
               "owns_data_dir": true, "token": "x"}}"#,
            boot
        )),
    );

    std::thread::sleep(Duration::from_millis(500));
    assert!(alive(durable_pm), "a durable server must outlive its owner");

    let report = rustypglite::sweep(Some(root.path())).unwrap().expect("not busy");
    eprintln!("{:?}", report);

    assert_eq!(report.reclaimed, 2, "B and E");
    assert_eq!(report.live, 1, "A");
    assert_eq!(report.durable, 0, "C is rpgldur_*: outside the sweep altogether");
    assert!(Path::new(&durable_dir).file_name().unwrap().to_str().unwrap().starts_with("rpgldur_"));
    assert_eq!(report.legacy, 1, "D");
    assert_eq!(report.skipped, 1, "F");
    assert_eq!(report.failed, 0);

    assert!(!alive(dead_pm) && !Path::new(&dead_dir).exists(), "B reclaimed");
    assert!(!reused.exists(), "E reclaimed");
    assert!(alive(postmaster_pid(&live_dir)) && Path::new(&live_dir).exists(), "A untouched");
    assert!(alive(durable_pm) && Path::new(&durable_dir).exists(), "C untouched");
    assert!(legacy.exists(), "D untouched");
    assert!(foreign.exists(), "F untouched");

    // The explicit stop is the way to end a durable server.
    rustypglite::stop_dir(&durable_dir).unwrap();
    assert!(!alive(durable_pm), "durable server stopped explicitly");
    assert!(!Path::new(&durable_dir).exists(), "its auto dir removed");

    drop(live);
}

#[test]
fn stop_dir_stops_a_live_owners_server_and_the_owner_copes() {
    let root = Root::new("stopdir");
    let pg = start_in(&root);
    let dir = pg.data_dir().to_string();
    let pm = postmaster_pid(&dir);

    rustypglite::stop_dir(&dir).unwrap();
    assert!(!alive(pm));
    assert!(!Path::new(&dir).exists());

    drop(pg); // the owner's own stop finds nothing to do, and must not fail
    assert!(rustypglite::stop_dir(&dir).is_err(), "no such dir any more");
}

#[test]
fn stop_dir_never_removes_a_dir_it_did_not_create() {
    let root = Root::new("keepuser");
    let user_dir = root.0.join("mydata");
    let pg = EmbeddedPg::start_with(StartOptions {
        data_dir: Some(user_dir.to_str().unwrap().to_string()),
        temp_root: Some(root.path().to_string()),
        silent: true,
        ..Default::default()
    })
    .expect("start");
    let pm = postmaster_pid(user_dir.to_str().unwrap());

    rustypglite::stop_dir(user_dir.to_str().unwrap()).unwrap();
    assert!(!alive(pm));
    assert!(user_dir.join("PG_VERSION").exists(), "the caller's data dir is kept");
    drop(pg);
}

#[test]
fn dropping_a_connect_existing_handle_leaves_the_server_running() {
    let root = Root::new("borrow");
    let pg = start_in(&root);
    let dir = pg.data_dir().to_string();
    let pm = postmaster_pid(&dir);

    let other = EmbeddedPg::connect_existing(&dir).unwrap();
    drop(other);
    assert!(alive(pm), "a borrowed handle must not stop the owner's server");
    pg.exec_sql("SELECT 1").expect("still serving");
}

#[test]
fn a_durable_server_survives_its_handle() {
    let root = Root::new("durable");
    let pg = EmbeddedPg::start_with(StartOptions {
        temp_root: Some(root.path().to_string()),
        durable: true,
        silent: true,
        ..Default::default()
    })
    .expect("start");
    assert!(pg.is_durable());
    let dir = pg.data_dir().to_string();
    let pm = postmaster_pid(&dir);
    assert_eq!(owner_field(&dir, "watcher_pid"), "0", "no watchdog");

    drop(pg);
    assert!(alive(pm), "dropping a durable handle detaches");

    rustypglite::stop_dir(&dir).unwrap();
    assert!(!alive(pm));
}

#[test]
fn the_watchdog_keeps_a_live_owners_dir_looking_fresh() {
    // For rustypglite 0.1.x processes on the same machine, which still take
    // a dir older than ten minutes (with no answering socket) to be stale.
    let root = Root::new("heartbeat");
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["owner_child", "--exact", "--nocapture", "--test-threads=1"])
        .env("RPGL_CHILD_ROOT", root.path())
        .env("RPGL_CHILD_DURABLE", "0")
        .env("RUSTYPGLITE_HEARTBEAT_SECONDS", "1")
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let stdout = child.stdout.take().unwrap();
    let dir = BufReader::new(stdout)
        .lines()
        .map(|l| l.unwrap())
        .find_map(|l| l.split_once("RPGL_DIR=").map(|(_, d)| d.to_string()))
        .expect("owner child started a server");
    let mut owner = Owner { child, dir };

    let old = Command::new("touch").args(["-t", "202001010000", &owner.dir]).status().unwrap();
    assert!(old.success());
    let age = || {
        std::fs::metadata(&owner.dir).unwrap().modified().unwrap().elapsed().unwrap_or_default()
    };
    assert!(age() > Duration::from_secs(3600));
    assert!(
        wait_until(Duration::from_secs(5), || age() < Duration::from_secs(60)),
        "the dir was not touched"
    );
    owner.kill9();
}

#[test]
fn a_forked_child_exiting_leaves_the_parents_server_alone() {
    let root = Root::new("forkexit");
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["owner_child", "--exact", "--nocapture", "--test-threads=1"])
        .env("RPGL_CHILD_ROOT", root.path())
        .env("RPGL_CHILD_DURABLE", "0")
        .env("RPGL_CHILD_FORK_EXIT", "1")
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines().map(|l| l.unwrap());
    let dir = lines
        .find_map(|l| l.split_once("RPGL_DIR=").map(|(_, d)| d.to_string()))
        .expect("owner child started a server");
    let mut owner = Owner { child, dir: dir.clone() };
    let pm = postmaster_pid(&dir);
    assert!(lines.any(|l| l.contains("RPGL_FORKED=1")), "the fork probe did not finish");

    std::thread::sleep(Duration::from_millis(500));
    assert!(alive(pm), "the forked child's exit stopped the parent's server");
    assert!(Path::new(&dir).join("owner.json").exists(), "the forked child's exit removed the dir");
    let watcher: u32 = owner_field(&dir, "watcher_pid").parse().unwrap();
    assert!(alive(watcher), "the forked child's exit released the parent's watchdog");

    // And the parent's server still dies with the parent.
    owner.kill9();
    assert!(wait_until(Duration::from_secs(15), || !alive(pm) && !Path::new(&dir).exists()));
}

#[test]
fn starting_on_a_dir_that_already_has_a_server_changes_nothing() {
    let root = Root::new("occupied");
    let user_dir = root.0.join("shared");
    let first = EmbeddedPg::start_with(StartOptions {
        data_dir: Some(user_dir.to_str().unwrap().to_string()),
        temp_root: Some(root.path().to_string()),
        durable: true,
        silent: true,
        ..Default::default()
    })
    .expect("first start");
    let dir = first.data_dir().to_string();
    let pm = postmaster_pid(&dir);
    let token = owner_field(&dir, "token");

    let second = EmbeddedPg::start_with(StartOptions {
        data_dir: Some(dir.clone()),
        temp_root: Some(root.path().to_string()),
        silent: true,
        ..Default::default()
    });
    assert!(second.is_err(), "a second server cannot start on an occupied dir");
    assert!(alive(pm), "the failed start stopped the server that was already there");
    assert_eq!(owner_field(&dir, "token"), token, "the failed start rewrote owner.json");
    first.exec_sql("SELECT 1").expect("the first server still serves");

    first.stop();
    assert!(!alive(pm));
}

#[test]
fn a_changed_boot_id_alone_does_not_make_a_live_owner_dead() {
    // macOS derived the boot id from kern.boottime, which moves when the
    // clock is stepped. Whatever the boot id says, a PID whose start time
    // still matches is alive.
    let root = Root::new("bootid");
    let live = start_in(&root);
    let live_dir = live.data_dir().to_string();
    let ns = owner_field(&live_dir, "pid_ns");
    let start = owner_field(&live_dir, "owner_start");

    let other_boot = fake_dir(
        &root,
        "rpgl_otherboot",
        Some(format!(
            r#"{{"owner_pid": {}, "owner_start": "{}", "boot_id": "not-this-boot", "pid_ns": "{}",
               "watcher_pid": 0, "watcher_start": "", "durable": false,
               "owns_data_dir": true, "token": "x"}}"#,
            std::process::id(),
            start,
            ns
        )),
    );

    let report = rustypglite::sweep(Some(root.path())).unwrap().expect("not busy");
    if cfg!(target_os = "linux") {
        // Linux start times are ticks since boot, so across boots they are not
        // comparable: a matching one under a different boot_id (which never
        // moves) is a coincidence, and the owner is dead.
        assert_eq!(report.reclaimed, 1, "{:?}", report);
        assert!(!other_boot.exists(), "a pre-reboot dir must not leak");
    } else {
        // macOS start times are absolute, and the boot id must never decide alone.
        assert_eq!(report.reclaimed, 0, "{:?}", report);
        assert!(other_boot.exists(), "a live owner's dir was reclaimed over a boot id");
    }
    drop(live);
}

#[test]
fn a_stale_postmaster_pid_naming_someone_elses_process_does_not_block_a_start() {
    // After a reboot or crash, postmaster.pid can name a PID that now belongs
    // to another user (here: init). That is not our server.
    let root = Root::new("stalepid");
    let dir = root.0.join("data");
    let dir_s = dir.to_str().unwrap().to_string();
    let opts = || StartOptions {
        data_dir: Some(dir_s.clone()),
        temp_root: Some(root.path().to_string()),
        silent: true,
        ..Default::default()
    };
    EmbeddedPg::start_with(opts()).expect("first start").stop();
    std::fs::write(dir.join("postmaster.pid"), format!("1
{}
0
5432
{}
", dir_s, dir_s)).unwrap();

    let pg = EmbeddedPg::start_with(opts()).expect("a stale postmaster.pid must not block the start");
    pg.exec_sql("SELECT 1").expect("serving");
    pg.stop();
}

#[test]
fn two_starts_racing_on_one_data_dir_leave_exactly_one_server_running() {
    let root = Root::new("race");
    let dir = root.0.join("data").to_str().unwrap().to_string();
    let rootp = root.path().to_string();
    let barrier = std::sync::Arc::new(std::sync::Barrier::new(2));
    let handles: Vec<_> = (0..2)
        .map(|_| {
            let (dir, rootp, barrier) = (dir.clone(), rootp.clone(), barrier.clone());
            std::thread::spawn(move || {
                barrier.wait();
                EmbeddedPg::start_with(StartOptions {
                    data_dir: Some(dir),
                    temp_root: Some(rootp),
                    silent: true,
                    ..Default::default()
                })
            })
        })
        .collect();
    let results: Vec<_> = handles.into_iter().map(|h| h.join().unwrap()).collect();
    let winners: Vec<_> = results.into_iter().filter_map(|r| r.ok()).collect();
    assert_eq!(winners.len(), 1, "exactly one start wins");
    winners[0].exec_sql("SELECT 1").expect("the winner's server survived the loser's failure");
}

#[test]
fn a_forked_child_that_lives_on_does_not_keep_the_server_up() {
    let root = Root::new("forkhold");
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["owner_child", "--exact", "--nocapture", "--test-threads=1"])
        .env("RPGL_CHILD_ROOT", root.path())
        .env("RPGL_CHILD_DURABLE", "0")
        .env("RPGL_CHILD_FORK_HOLD", "1")
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines().map(|l| l.unwrap());
    let dir = lines
        .find_map(|l| l.split_once("RPGL_DIR=").map(|(_, d)| d.to_string()))
        .expect("owner child started a server");
    let forked: u32 = lines
        .find_map(|l| l.split_once("RPGL_FORKCHILD=").map(|(_, p)| p.trim().parse().unwrap()))
        .expect("fork probe reported its child");
    let mut owner = Owner { child, dir: dir.clone() };
    let pm = postmaster_pid(&dir);

    owner.kill9();
    let gone = wait_until(Duration::from_secs(15), || !alive(pm) && !Path::new(&dir).exists());
    let forked_alive = alive(forked);
    signal("-KILL", forked);
    assert!(forked_alive, "the forked child should still have been running");
    assert!(gone, "a forked child's copy of the watchdog socket kept the server up");
}
