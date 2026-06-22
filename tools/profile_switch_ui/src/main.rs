use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;

mod runtime;

#[derive(Clone, Debug)]
struct AppConfig {
    repo: PathBuf,
    exe: PathBuf,
    config: Option<PathBuf>,
    host: String,
    port: u16,
    open: bool,
}

#[derive(Clone, Debug)]
struct Profile {
    name: String,
    path: PathBuf,
    comment: Option<String>,
}

#[derive(Debug)]
struct ControlOutput {
    ok: bool,
    code: Option<i32>,
    stdout: String,
    stderr: String,
}

#[derive(Debug)]
struct RuntimeState {
    health_summary: String,
    active_profile: Option<String>,
    active_source: Option<String>,
    status_text: String,
    fans: Vec<runtime::FanRow>,
}

#[derive(Debug)]
struct HttpRequest {
    method: String,
    path: String,
    body: Vec<u8>,
    host: Option<String>,
    origin: Option<String>,
    sec_fetch_site: Option<String>,
}

// Upper bound on a request body. The only legitimate POST is the tiny /switch
// form, so a few KiB is ample; this mirrors the existing 16 KiB header cap and
// bounds memory regardless of a client-supplied Content-Length.
const MAX_BODY: usize = 64 * 1024;

fn main() {
    if let Err(err) = real_main() {
        eprintln!("Error: {err}");
        std::process::exit(1);
    }
}

fn real_main() -> Result<(), String> {
    let cfg = parse_args(env::args().skip(1))?;
    if !cfg.exe.exists() {
        return Err(format!(
            "control executable was not found: {}",
            cfg.exe.display()
        ));
    }

    let listener = TcpListener::bind((cfg.host.as_str(), cfg.port))
        .map_err(|err| format!("failed to bind {}:{}: {err}", cfg.host, cfg.port))?;
    let url = format!("http://{}:{}/", cfg.host, cfg.port);
    println!("SVG-MB profile UI listening on {url}");
    println!("Control executable: {}", cfg.exe.display());
    println!("Repository: {}", cfg.repo.display());

    if cfg.open {
        open_browser(&url);
    }

    for incoming in listener.incoming() {
        match incoming {
            Ok(mut stream) => {
                if let Err(err) = handle_connection(&cfg, &mut stream) {
                    let body = render_page(
                        &cfg,
                        &[],
                        &RuntimeState {
                            health_summary: "Request failed".to_string(),
                            active_profile: None,
                            active_source: None,
                            status_text: err.to_string(),
                            fans: Vec::new(),
                        },
                        Some(&format!("Request failed: {err}")),
                    );
                    let _ = respond_html(&mut stream, "500 Internal Server Error", &body);
                }
            }
            Err(err) => eprintln!("accept failed: {err}"),
        }
    }

    Ok(())
}

fn parse_args<I>(args: I) -> Result<AppConfig, String>
where
    I: IntoIterator<Item = String>,
{
    let mut repo = env::current_dir().map_err(|err| format!("cannot read cwd: {err}"))?;
    let mut exe: Option<PathBuf> = None;
    let mut config: Option<PathBuf> = None;
    let mut host = "127.0.0.1".to_string();
    let mut port = 8766;
    let mut open = false;

    let mut iter = args.into_iter();
    while let Some(arg) = iter.next() {
        match arg.as_str() {
            "--repo" => repo = take_path("--repo", &mut iter)?,
            "--exe" => exe = Some(take_path("--exe", &mut iter)?),
            "--config" => config = Some(take_path("--config", &mut iter)?),
            "--host" => host = take_value("--host", &mut iter)?,
            "--port" => {
                let raw = take_value("--port", &mut iter)?;
                port = raw
                    .parse::<u16>()
                    .map_err(|_| format!("--port must be a TCP port, got {raw}"))?;
            }
            "--open" => open = true,
            "--help" | "-h" => {
                print_help();
                std::process::exit(0);
            }
            other => return Err(format!("unknown argument: {other}")),
        }
    }

    let repo = normalize_path(repo);
    let exe = exe
        .map(normalize_path)
        .unwrap_or_else(|| repo.join("release").join("svg-mb-control.exe"));
    let config = config.map(normalize_path);

    Ok(AppConfig {
        repo,
        exe,
        config,
        host,
        port,
        open,
    })
}

fn take_value<I>(flag: &str, iter: &mut I) -> Result<String, String>
where
    I: Iterator<Item = String>,
{
    iter.next()
        .ok_or_else(|| format!("{flag} requires a value"))
}

fn take_path<I>(flag: &str, iter: &mut I) -> Result<PathBuf, String>
where
    I: Iterator<Item = String>,
{
    Ok(PathBuf::from(take_value(flag, iter)?))
}

fn normalize_path(path: PathBuf) -> PathBuf {
    path.canonicalize().unwrap_or(path)
}

fn print_help() {
    println!(
        "Usage: svg-mb-profile-ui [--repo PATH] [--exe PATH] [--config PATH] [--host HOST] [--port PORT] [--open]\n\
         \n\
         Serves a minimal local browser UI for listing known profiles and calling\n\
         the existing svg-mb-control.exe --set-profile operator path."
    );
}

fn handle_connection(cfg: &AppConfig, stream: &mut TcpStream) -> Result<(), String> {
    stream
        .set_read_timeout(Some(Duration::from_secs(2)))
        .map_err(|err| format!("failed to set read timeout: {err}"))?;
    stream
        .set_write_timeout(Some(Duration::from_secs(5)))
        .map_err(|err| format!("failed to set write timeout: {err}"))?;

    let request =
        read_http_request(stream).map_err(|err| format!("failed to read request: {err}"))?;
    match (request.method.as_str(), request.path.as_str()) {
        ("GET", "/") => render_and_respond(cfg, stream, None),
        ("POST", "/switch") => {
            // The only state-changing route. Reject cross-origin / DNS-rebound
            // requests so a page the operator merely visits cannot force a
            // profile switch (worker restart) at 127.0.0.1.
            if !request_is_local_origin(&request, cfg) {
                return respond_bytes(
                    stream,
                    "403 Forbidden",
                    "text/plain",
                    b"cross-origin request rejected",
                )
                .map_err(|err| format!("failed to write response: {err}"));
            }
            let profiles = list_profiles(cfg);
            let requested = parse_form_profile(&request.body)
                .ok_or_else(|| "profile form field is missing".to_string())?;
            let profile = profiles
                .iter()
                .find(|profile| profile.name == requested)
                .ok_or_else(|| format!("unknown profile: {requested}"))?;

            if !profile_name_allowed(&profile.name) {
                return Err(format!("unsafe profile name: {}", profile.name));
            }

            let output = run_control(cfg, &["--set-profile", &profile.name]);
            let message = if output.ok {
                format!("Profile switch requested: {}", profile.name)
            } else {
                format!(
                    "Profile switch failed for {} (exit {:?}). {}{}",
                    profile.name,
                    output.code,
                    output.stdout.trim(),
                    output.stderr.trim()
                )
            };
            render_and_respond(cfg, stream, Some(&message))
        }
        ("GET", "/favicon.ico") => respond_bytes(stream, "404 Not Found", "text/plain", b"")
            .map_err(|err| format!("failed to write response: {err}")),
        _ => respond_bytes(stream, "404 Not Found", "text/plain", b"not found")
            .map_err(|err| format!("failed to write response: {err}")),
    }
}

fn render_and_respond(
    cfg: &AppConfig,
    stream: &mut TcpStream,
    message: Option<&str>,
) -> Result<(), String> {
    let profiles = list_profiles(cfg);
    let state = read_runtime_state(cfg);
    let body = render_page(cfg, &profiles, &state, message);
    respond_html(stream, "200 OK", &body).map_err(|err| format!("failed to write response: {err}"))
}

fn list_profiles(cfg: &AppConfig) -> Vec<Profile> {
    let mut found = BTreeMap::<String, Profile>::new();
    for dir in profile_dirs(cfg) {
        let entries = match fs::read_dir(&dir) {
            Ok(entries) => entries,
            Err(_) => continue,
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.extension().and_then(|ext| ext.to_str()) != Some("json") {
                continue;
            }
            let Some(stem) = path.file_stem().and_then(|stem| stem.to_str()) else {
                continue;
            };
            if !profile_name_allowed(stem) {
                continue;
            }
            let name = stem.to_string();
            if found.contains_key(&name) {
                continue;
            }
            let comment = read_profile_comment(&path);
            found.insert(
                name.clone(),
                Profile {
                    name,
                    comment,
                    path,
                },
            );
        }
    }
    found.into_values().collect()
}

fn profile_dirs(cfg: &AppConfig) -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    let exe_parent = cfg.exe.parent().map(Path::to_path_buf);
    if let Some(parent) = &exe_parent {
        dirs.push(parent.join("profiles"));
        dirs.push(parent.join("config").join("profiles"));
        if let Some(repo_like) = parent.parent() {
            dirs.push(repo_like.join("profiles"));
            dirs.push(repo_like.join("config").join("profiles"));
        }
    }

    dirs.push(cfg.repo.join("profiles"));
    dirs.push(cfg.repo.join("config").join("profiles"));
    dirs.push(
        env::current_dir()
            .unwrap_or_else(|_| cfg.repo.clone())
            .join("profiles"),
    );
    dirs.push(
        env::current_dir()
            .unwrap_or_else(|_| cfg.repo.clone())
            .join("config")
            .join("profiles"),
    );

    dedup_paths(dirs)
}

fn dedup_paths(paths: Vec<PathBuf>) -> Vec<PathBuf> {
    let mut seen = BTreeMap::<String, PathBuf>::new();
    for path in paths {
        let key = normalize_path(path.clone())
            .to_string_lossy()
            .to_lowercase();
        seen.entry(key).or_insert(path);
    }
    seen.into_values().collect()
}

fn read_profile_comment(path: &Path) -> Option<String> {
    fs::read_to_string(path)
        .ok()
        .and_then(|text| runtime::parse_profile_comment(&text))
        .map(|comment| compact_whitespace(&comment))
}

fn read_runtime_state(cfg: &AppConfig) -> RuntimeState {
    let health = run_control(cfg, &["--health", "--json"]);
    let status = run_control(cfg, &["--status"]);

    let health_info = runtime::parse_health(&health.stdout);
    let health_summary = if health.ok {
        let state = health_info
            .health_state
            .clone()
            .or_else(|| health_info.status.clone());
        match (state, health_info.process_id) {
            (Some(state), Some(pid)) => format!("{state}; worker PID {pid}"),
            (Some(state), None) => state,
            _ => "Health command succeeded".to_string(),
        }
    } else {
        format!(
            "Health command failed (exit {:?}): {}{}",
            health.code,
            health.stdout.trim(),
            health.stderr.trim()
        )
    };

    let (active_profile, active_source) =
        read_active_profile_from_runtime(health_info.runtime_home.as_deref());
    let status_text = if status.ok {
        status.stdout
    } else {
        format!(
            "Status command failed (exit {:?})\n{}\n{}",
            status.code, status.stdout, status.stderr
        )
    };

    let fans = if let Some(home) = health_info.runtime_home.as_deref() {
        let home = PathBuf::from(home);
        let snapshot = fs::read_to_string(home.join("current_state.json")).ok();
        let control = fs::read_to_string(home.join("control_runtime.json")).ok();
        runtime::build_fan_rows(snapshot.as_deref(), control.as_deref())
    } else {
        Vec::new()
    };

    RuntimeState {
        health_summary,
        active_profile,
        active_source,
        status_text,
        fans,
    }
}

fn read_active_profile_from_runtime(runtime_home: Option<&str>) -> (Option<String>, Option<String>) {
    let Some(runtime_home) = runtime_home else {
        return (None, None);
    };
    let runtime_home = PathBuf::from(runtime_home);
    let candidates = [
        runtime_home.join("control_runtime.json"),
        runtime_home.join("control_supervisor.json"),
    ];

    for candidate in candidates {
        let Ok(text) = fs::read_to_string(candidate) else {
            continue;
        };
        let rt = runtime::parse_runtime(&text);
        let name = rt.active_profile_name.or(rt.requested_profile_name);
        let source = rt.active_profile_source;
        if name.is_some() || source.is_some() {
            return (name, source);
        }
    }

    (None, None)
}

fn run_control(cfg: &AppConfig, args: &[&str]) -> ControlOutput {
    let mut command = Command::new(&cfg.exe);
    command.args(args);
    if let Some(config) = &cfg.config {
        command.arg("--config").arg(config);
    }
    if let Some(dir) = cfg.exe.parent() {
        command.current_dir(dir);
    }
    command.stdout(Stdio::piped()).stderr(Stdio::piped());

    match command.output() {
        Ok(output) => ControlOutput {
            ok: output.status.success(),
            code: output.status.code(),
            stdout: String::from_utf8_lossy(&output.stdout).to_string(),
            stderr: String::from_utf8_lossy(&output.stderr).to_string(),
        },
        Err(err) => ControlOutput {
            ok: false,
            code: None,
            stdout: String::new(),
            stderr: err.to_string(),
        },
    }
}

fn read_http_request(stream: &mut TcpStream) -> io::Result<HttpRequest> {
    let mut buffer = Vec::new();
    let mut chunk = [0u8; 1024];
    let header_end;

    loop {
        let read = stream.read(&mut chunk)?;
        if read == 0 {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "client closed connection",
            ));
        }
        buffer.extend_from_slice(&chunk[..read]);
        if let Some(pos) = find_header_end(&buffer) {
            header_end = pos;
            break;
        }
        if buffer.len() > 16 * 1024 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "request headers too large",
            ));
        }
    }

    let headers = String::from_utf8_lossy(&buffer[..header_end]).to_string();
    let mut lines = headers.lines();
    let request_line = lines
        .next()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing request line"))?;
    let mut parts = request_line.split_whitespace();
    let method = parts
        .next()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing method"))?
        .to_string();
    let path = parts
        .next()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing path"))?
        .split('?')
        .next()
        .unwrap_or("/")
        .to_string();

    let mut content_length = 0usize;
    let mut host = None;
    let mut origin = None;
    let mut sec_fetch_site = None;
    for line in headers.lines().skip(1) {
        let Some((name, value)) = line.split_once(':') else {
            continue;
        };
        let value = value.trim();
        if name.eq_ignore_ascii_case("content-length") {
            content_length = value.parse::<usize>().unwrap_or(0);
        } else if name.eq_ignore_ascii_case("host") {
            host = Some(value.to_string());
        } else if name.eq_ignore_ascii_case("origin") {
            origin = Some(value.to_string());
        } else if name.eq_ignore_ascii_case("sec-fetch-site") {
            sec_fetch_site = Some(value.to_string());
        }
    }

    if content_length > MAX_BODY {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "request body too large",
        ));
    }

    let body_start = header_end + 4;
    let mut body = buffer.get(body_start..).unwrap_or_default().to_vec();
    while body.len() < content_length {
        let read = stream.read(&mut chunk)?;
        if read == 0 {
            break;
        }
        body.extend_from_slice(&chunk[..read]);
        if body.len() > MAX_BODY {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "request body too large",
            ));
        }
    }
    body.truncate(content_length);

    Ok(HttpRequest {
        method,
        path,
        body,
        host,
        origin,
        sec_fetch_site,
    })
}

fn find_header_end(buffer: &[u8]) -> Option<usize> {
    buffer.windows(4).position(|window| window == b"\r\n\r\n")
}

fn respond_html(stream: &mut TcpStream, status: &str, body: &str) -> io::Result<()> {
    respond_bytes(stream, status, "text/html; charset=utf-8", body.as_bytes())
}

fn respond_bytes(
    stream: &mut TcpStream,
    status: &str,
    content_type: &str,
    body: &[u8],
) -> io::Result<()> {
    write!(
        stream,
        "HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    )?;
    stream.write_all(body)
}

fn render_page(
    cfg: &AppConfig,
    profiles: &[Profile],
    state: &RuntimeState,
    message: Option<&str>,
) -> String {
    let active = state
        .active_profile
        .as_deref()
        .unwrap_or("not reported by runtime");
    let source = state.active_source.as_deref().unwrap_or("not reported");
    let mut profile_rows = String::new();

    if profiles.is_empty() {
        profile_rows.push_str(
            "<tr><td colspan=\"4\" class=\"empty\">No profile JSON files were found.</td></tr>",
        );
    } else {
        for profile in profiles {
            let is_active = state.active_profile.as_deref() == Some(profile.name.as_str());
            let mut row_class = "profile-row";
            if is_active {
                row_class = "profile-row active";
            }
            let comment = profile
                .comment
                .as_deref()
                .unwrap_or("No profile comment provided.");
            let button = if is_active {
                "<button type=\"submit\" disabled>Active</button>".to_string()
            } else {
                format!(
                    "<button type=\"submit\" name=\"profile\" value=\"{}\">Apply</button>",
                    html_escape(&profile.name)
                )
            };
            profile_rows.push_str(&format!(
                "<tr class=\"{row_class}\">\
                 <td class=\"name\">{}</td>\
                 <td>{}</td>\
                 <td class=\"path\">{}</td>\
                 <td><form method=\"post\" action=\"/switch\">{button}</form></td>\
                 </tr>",
                html_escape(&profile.name),
                html_escape(comment),
                html_escape(&profile.path.display().to_string())
            ));
        }
    }

    let fan_rows = if state.fans.is_empty() {
        "<tr><td colspan=\"5\" class=\"empty\">Fan data unavailable.</td></tr>".to_string()
    } else {
        let mut out = String::new();
        for fan in &state.fans {
            let duty = match fan.duty_percent {
                Some(v) => format!("{v:.1}%"),
                None => "&mdash;".to_string(),
            };
            let rpm = match (fan.tach_valid, fan.rpm) {
                (true, Some(v)) => v.to_string(),
                _ => "&mdash;".to_string(),
            };
            let temp = match (fan.observed_temp_c, fan.temp_source.as_deref()) {
                (Some(t), Some(src)) => format!("{t:.0} &deg;C ({})", html_escape(src)),
                (Some(t), None) => format!("{t:.0} &deg;C"),
                _ => "&mdash;".to_string(),
            };
            let controller = match fan.controller_kind.as_deref() {
                Some(k) => html_escape(k),
                None => "&mdash;".to_string(),
            };
            out.push_str(&format!(
                "<tr><td class=\"name\">ch{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
                fan.channel, duty, rpm, temp, controller
            ));
        }
        out
    };

    let message_html = message
        .map(|msg| format!("<section class=\"notice\">{}</section>", html_escape(msg)))
        .unwrap_or_default();

    format!(
        "<!doctype html>\
         <html lang=\"en\">\
         <head>\
         <meta charset=\"utf-8\">\
         <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\
         <meta http-equiv=\"refresh\" content=\"3\">\
         <title>SVG-MB Profile Switch</title>\
         <style>{}</style>\
         </head>\
         <body>\
         <main>\
         <header>\
         <h1>Profile Switch</h1>\
         <form method=\"get\" action=\"/\"><button type=\"submit\">Refresh</button></form>\
         </header>\
         {message_html}\
         <section class=\"state-grid\">\
         <div><span>Health</span><strong>{}</strong></div>\
         <div><span>Active profile</span><strong>{}</strong></div>\
         <div><span>Source</span><strong>{}</strong></div>\
         </section>\
         <section class=\"table-wrap fans\">\
         <table>\
         <thead><tr><th>Fan</th><th>Duty %</th><th>RPM</th><th>Temp</th><th>Controller</th></tr></thead>\
         <tbody>{fan_rows}</tbody>\
         </table>\
         </section>\
         <p class=\"consequence\">Switching profiles uses the existing <code>--set-profile</code> path. The supervisor restarts the worker, so fans can briefly return to firmware control during the handoff.</p>\
         <section class=\"table-wrap\">\
         <table>\
         <thead><tr><th>Profile</th><th>Notes</th><th>File</th><th>Action</th></tr></thead>\
         <tbody>{profile_rows}</tbody>\
         </table>\
         </section>\
         <section class=\"details\">\
         <h2>Runtime Status</h2>\
         <pre>{}</pre>\
         </section>\
         <footer>Repo: {} | Control: {}</footer>\
         </main>\
         </body>\
         </html>",
        page_css(),
        html_escape(&state.health_summary),
        html_escape(active),
        html_escape(source),
        html_escape(&state.status_text),
        html_escape(&cfg.repo.display().to_string()),
        html_escape(&cfg.exe.display().to_string())
    )
}

fn page_css() -> &'static str {
    r#"
:root {
  color-scheme: light;
  --bg: #f5f6f3;
  --panel: #ffffff;
  --ink: #1f2428;
  --muted: #5f6b73;
  --line: #d9ded8;
  --accent: #0f766e;
  --accent-strong: #0b5f59;
  --warn: #9a5a00;
  --ok-bg: #e7f3ef;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--bg);
  color: var(--ink);
  font: 14px/1.45 "Segoe UI", system-ui, sans-serif;
}
main {
  width: min(1180px, calc(100vw - 32px));
  margin: 24px auto 32px;
}
header {
  align-items: center;
  border-bottom: 1px solid var(--line);
  display: flex;
  gap: 16px;
  justify-content: space-between;
  padding-bottom: 12px;
}
h1, h2 { margin: 0; font-weight: 650; letter-spacing: 0; }
h1 { font-size: 24px; }
h2 { font-size: 16px; }
button {
  background: var(--accent);
  border: 1px solid var(--accent-strong);
  border-radius: 6px;
  color: #fff;
  cursor: pointer;
  font: inherit;
  min-height: 34px;
  min-width: 78px;
  padding: 6px 12px;
}
button:hover { background: var(--accent-strong); }
button:disabled {
  background: #e9ece8;
  border-color: #c8cec8;
  color: #69736b;
  cursor: default;
}
.notice {
  background: #fff8e8;
  border: 1px solid #e8cf9a;
  border-radius: 6px;
  color: #513800;
  margin-top: 16px;
  padding: 10px 12px;
}
.state-grid {
  display: grid;
  gap: 12px;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  margin: 18px 0 12px;
}
.state-grid div {
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: 6px;
  min-height: 74px;
  padding: 12px;
}
.state-grid span {
  color: var(--muted);
  display: block;
  font-size: 12px;
  margin-bottom: 6px;
  text-transform: uppercase;
}
.state-grid strong {
  display: block;
  font-size: 15px;
  overflow-wrap: anywhere;
}
.consequence {
  color: var(--warn);
  margin: 0 0 16px;
}
code {
  background: #e8ece8;
  border-radius: 4px;
  padding: 1px 4px;
}
.table-wrap {
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: 6px;
  overflow-x: auto;
}
table {
  border-collapse: collapse;
  min-width: 860px;
  width: 100%;
}
th, td {
  border-bottom: 1px solid var(--line);
  padding: 10px 12px;
  text-align: left;
  vertical-align: top;
}
th {
  background: #eef1ed;
  color: #394147;
  font-size: 12px;
  text-transform: uppercase;
}
tr:last-child td { border-bottom: 0; }
.profile-row.active td { background: var(--ok-bg); }
.name { font-weight: 650; width: 180px; }
.path {
  color: var(--muted);
  font-family: Consolas, "Cascadia Mono", monospace;
  font-size: 12px;
  overflow-wrap: anywhere;
  width: 420px;
}
.empty { color: var(--muted); text-align: center; }
.details {
  margin-top: 18px;
}
pre {
  background: #202521;
  border-radius: 6px;
  color: #edf4ed;
  margin: 8px 0 0;
  max-height: 380px;
  overflow: auto;
  padding: 12px;
  white-space: pre-wrap;
}
footer {
  color: var(--muted);
  font-size: 12px;
  margin-top: 14px;
  overflow-wrap: anywhere;
}
@media (max-width: 760px) {
  main { width: min(100vw - 20px, 1180px); margin-top: 12px; }
  header { align-items: flex-start; flex-direction: column; }
  .state-grid { grid-template-columns: 1fr; }
}
.fans { margin: 12px 0 16px; }
"#
}

// Accept a state-changing request only when it originates from the local UI
// itself. This closes two gaps for a cookieless localhost helper:
//   * CSRF  — a cross-origin <form> auto-POST carries an Origin we don't allow.
//   * DNS rebinding — a rebound request carries a foreign Host we don't allow.
// Non-browser clients (curl, scripts) send neither Origin nor Sec-Fetch-Site
// and a conforming Host, so the operator's own tooling is unaffected.
fn request_is_local_origin(request: &HttpRequest, cfg: &AppConfig) -> bool {
    let port = cfg.port;
    let allowed = [
        format!("127.0.0.1:{port}"),
        format!("localhost:{port}"),
        format!("[::1]:{port}"),
        format!("{}:{}", cfg.host, port),
    ];
    let is_allowed = |authority: &str| {
        let authority = authority.trim();
        allowed.iter().any(|a| a.eq_ignore_ascii_case(authority))
    };

    if let Some(host) = &request.host {
        if !is_allowed(host) {
            return false;
        }
    }
    if let Some(origin) = &request.origin {
        let authority = origin.trim();
        if authority.eq_ignore_ascii_case("null") || !is_allowed(header_authority(authority)) {
            return false;
        }
    }
    if let Some(site) = &request.sec_fetch_site {
        let site = site.trim().to_ascii_lowercase();
        if site != "same-origin" && site != "none" {
            return false;
        }
    }
    true
}

// Reduce an Origin value (e.g. "http://127.0.0.1:8766") to its host:port
// authority for comparison.
fn header_authority(value: &str) -> &str {
    let value = value
        .strip_prefix("http://")
        .or_else(|| value.strip_prefix("https://"))
        .unwrap_or(value);
    value.split('/').next().unwrap_or(value)
}

fn parse_form_profile(body: &[u8]) -> Option<String> {
    let form = String::from_utf8_lossy(body);
    for pair in form.split('&') {
        let (name, value) = pair.split_once('=').unwrap_or((pair, ""));
        if url_decode(name) == "profile" {
            return Some(url_decode(value));
        }
    }
    None
}

fn url_decode(value: &str) -> String {
    let bytes = value.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        match bytes[index] {
            b'+' => {
                out.push(b' ');
                index += 1;
            }
            b'%' if index + 2 < bytes.len() => {
                if let Ok(hex) = std::str::from_utf8(&bytes[index + 1..index + 3]) {
                    if let Ok(byte) = u8::from_str_radix(hex, 16) {
                        out.push(byte);
                        index += 3;
                        continue;
                    }
                }
                out.push(bytes[index]);
                index += 1;
            }
            byte => {
                out.push(byte);
                index += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).to_string()
}

fn profile_name_allowed(name: &str) -> bool {
    if name.is_empty()
        || name.starts_with('-')
        || name.contains("..")
        || name.contains('/')
        || name.contains('\\')
    {
        return false;
    }
    if !name
        .chars()
        .all(|ch| ch.is_ascii_alphanumeric() || matches!(ch, '-' | '_' | '.'))
    {
        return false;
    }
    if name.chars().all(|ch| ch == '.') {
        return false;
    }
    let stem = name.split('.').next().unwrap_or(name);
    !is_windows_reserved_stem(stem)
}

// Windows treats CON/PRN/AUX/NUL and COM1-9/LPT1-9 as device names regardless
// of extension; reject them so a `<stem>.json` lookup can never resolve to a
// device handle on the downstream `--set-profile` path.
fn is_windows_reserved_stem(stem: &str) -> bool {
    let upper = stem.to_ascii_uppercase();
    if matches!(upper.as_str(), "CON" | "PRN" | "AUX" | "NUL") {
        return true;
    }
    for prefix in ["COM", "LPT"] {
        if let Some(rest) = upper.strip_prefix(prefix) {
            if rest.len() == 1 && matches!(rest.as_bytes()[0], b'1'..=b'9') {
                return true;
            }
        }
    }
    false
}

fn compact_whitespace(value: &str) -> String {
    value.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn html_escape(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for ch in value.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&#39;"),
            other => out.push(other),
        }
    }
    out
}

#[cfg(target_os = "windows")]
fn open_browser(url: &str) {
    let _ = Command::new("cmd")
        .args(["/C", "start", "", url])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn();
}

#[cfg(not(target_os = "windows"))]
fn open_browser(url: &str) {
    let _ = Command::new("xdg-open")
        .arg(url)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_form_profile_value() {
        assert_eq!(
            parse_form_profile(b"profile=snd-desk-composed&x=1").as_deref(),
            Some("snd-desk-composed")
        );
        assert_eq!(
            parse_form_profile(b"profile=quiet+bench%2Eone").as_deref(),
            Some("quiet bench.one")
        );
    }

    #[test]
    fn rejects_path_like_profile_names() {
        assert!(profile_name_allowed("snd-desk-composed"));
        assert!(profile_name_allowed("quiet.1"));
        assert!(!profile_name_allowed("../quiet"));
        assert!(!profile_name_allowed("sub\\quiet"));
        assert!(!profile_name_allowed("quiet bench"));
    }

    #[test]
    fn rejects_reserved_and_dash_profile_names() {
        // Leading dash (argv-flag confusion) and dot-only stems.
        assert!(!profile_name_allowed("-foo"));
        assert!(!profile_name_allowed("."));
        assert!(!profile_name_allowed(".."));
        // Windows reserved device stems, case-insensitive, with/without ext.
        assert!(!profile_name_allowed("NUL"));
        assert!(!profile_name_allowed("nul"));
        assert!(!profile_name_allowed("CON.json"));
        assert!(!profile_name_allowed("COM1"));
        assert!(!profile_name_allowed("lpt9"));
        // Look-alikes that are NOT reserved stay allowed.
        assert!(profile_name_allowed("console"));
        assert!(profile_name_allowed("com10"));
        assert!(profile_name_allowed("nul-backup"));
    }

    #[test]
    fn same_origin_guard_blocks_cross_origin_switch() {
        let cfg = AppConfig {
            repo: PathBuf::from("."),
            exe: PathBuf::from("svg-mb-control.exe"),
            config: None,
            host: "127.0.0.1".to_string(),
            port: 8766,
            open: false,
        };
        let req = |host: Option<&str>, origin: Option<&str>, sfs: Option<&str>| HttpRequest {
            method: "POST".to_string(),
            path: "/switch".to_string(),
            body: Vec::new(),
            host: host.map(str::to_string),
            origin: origin.map(str::to_string),
            sec_fetch_site: sfs.map(str::to_string),
        };

        // Same-origin form POST from the UI page.
        assert!(request_is_local_origin(
            &req(Some("127.0.0.1:8766"), Some("http://127.0.0.1:8766"), Some("same-origin")),
            &cfg
        ));
        // Non-browser client (no Origin / Sec-Fetch-Site), conforming Host.
        assert!(request_is_local_origin(&req(Some("127.0.0.1:8766"), None, None), &cfg));
        // Cross-origin CSRF attempt.
        assert!(!request_is_local_origin(
            &req(Some("127.0.0.1:8766"), Some("http://evil.example"), None),
            &cfg
        ));
        // DNS-rebound request carries a foreign Host.
        assert!(!request_is_local_origin(&req(Some("attacker.example:8766"), None, None), &cfg));
        // Browser-labelled cross-site navigation.
        assert!(!request_is_local_origin(
            &req(Some("127.0.0.1:8766"), None, Some("cross-site")),
            &cfg
        ));
        // Opaque origin.
        assert!(!request_is_local_origin(&req(Some("127.0.0.1:8766"), Some("null"), None), &cfg));
    }

    #[test]
    fn escapes_html_content() {
        assert_eq!(
            html_escape("<profile name=\"x\">"),
            "&lt;profile name=&quot;x&quot;&gt;"
        );
    }
}
