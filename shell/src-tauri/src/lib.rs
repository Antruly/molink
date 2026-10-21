use std::error::Error;
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::sync::Mutex;
use std::time::Duration;

use serde::Deserialize;
use tauri::{Manager, RunEvent, WebviewUrl, WebviewWindowBuilder};

/// Owns the child handle so the core can be stopped on application exit. The
/// core also watches the shell's pid (--parent-pid), which covers the case
/// where the shell is killed outright and never gets to run its exit handler.
struct CoreProcess(Mutex<Option<Child>>);

#[derive(Deserialize)]
struct ReadyLine {
    port: u16,
    token: String,
    #[serde(default)]
    service: Option<String>,
}

struct StartedCore {
    child: Child,
    ready: ReadyLine,
}

fn core_file_name() -> &'static str {
    if cfg!(windows) {
        "molink-core.exe"
    } else {
        "molink-core"
    }
}

/// Resolution order: explicit override, bundled resource, executable directory
/// (packagers may place it beside the shell), then the local CMake build tree
/// for development.
fn find_core(app: &tauri::AppHandle) -> Option<PathBuf> {
    if let Ok(explicit) = std::env::var("MOLINK_CORE") {
        let path = PathBuf::from(explicit);
        if path.is_file() {
            return Some(path);
        }
    }

    let mut candidates = Vec::new();
    if let Ok(dir) = app.path().resource_dir() {
        candidates.push(dir.join("binaries").join(core_file_name()));
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(dir.join(core_file_name()));
        }
    }
    candidates.push(
        PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../backend/build")
            .join(core_file_name()),
    );
    candidates.into_iter().find(|path| path.is_file())
}

fn start_core(app: &tauri::AppHandle) -> Result<StartedCore, String> {
    let path = find_core(app).ok_or_else(|| {
        "molink-core not found: build backend/ or set MOLINK_CORE to its path".to_string()
    })?;

    let mut child = Command::new(&path)
        .arg("--print-ready")
        .arg("--parent-pid")
        .arg(std::process::id().to_string())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .map_err(|error| format!("failed to start {}: {error}", path.display()))?;

    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| "molink-core stdout was not captured".to_string())?;

    // read_line blocks until the core prints its ready line. Keep it off the
    // main thread and bound the wait so a broken core cannot hang startup.
    let (sender, receiver) = mpsc::channel();
    std::thread::spawn(move || {
        let mut line = String::new();
        if BufReader::new(stdout).read_line(&mut line).is_ok() {
            let _ = sender.send(line);
        }
    });

    let line = match receiver.recv_timeout(Duration::from_secs(10)) {
        Ok(line) => line,
        Err(_) => {
            let _ = child.kill();
            let _ = child.wait();
            return Err("molink-core did not report ready within 10 seconds".to_string());
        }
    };

    let ready: ReadyLine = match serde_json::from_str(line.trim()) {
        Ok(ready) => ready,
        Err(error) => {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!("invalid ready line from molink-core: {error}"));
        }
    };

    if ready.port == 0 || ready.token.is_empty() {
        let _ = child.kill();
        let _ = child.wait();
        return Err("molink-core reported an empty port or token".to_string());
    }

    Ok(StartedCore { child, ready })
}

pub fn run() {
    let app = tauri::Builder::default()
        .setup(|app| {
            let started = start_core(app.handle())
                .map_err(|message| -> Box<dyn Error> { message.into() })?;

            if let Some(service) = &started.ready.service {
                println!("moLink shell: {service} ready on port {}", started.ready.port);
            }

            // The frontend reads host/port/token from the query string and
            // connects automatically when a token is present.
            let url = format!(
                "index.html?host=127.0.0.1&port={}&token={}",
                started.ready.port, started.ready.token
            );

            app.manage(CoreProcess(Mutex::new(Some(started.child))));

            WebviewWindowBuilder::new(app, "main", WebviewUrl::App(url.into()))
                .title("moLink")
                .inner_size(1280.0, 800.0)
                .min_inner_size(900.0, 600.0)
                .build()?;

            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("error while building the moLink shell");

    app.run(|app_handle, event| {
        if let RunEvent::Exit = event {
            if let Some(state) = app_handle.try_state::<CoreProcess>() {
                if let Ok(mut guard) = state.0.lock() {
                    if let Some(mut child) = guard.take() {
                        let _ = child.kill();
                        let _ = child.wait();
                    }
                }
            }
        }
    });
}
