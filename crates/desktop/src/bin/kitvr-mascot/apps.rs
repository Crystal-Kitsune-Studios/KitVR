use std::io;
use std::path::PathBuf;

#[derive(Clone, Debug)]
pub struct Application {
    pub name: String,
    pub path: PathBuf,
}

#[cfg(target_os = "linux")]
fn parse_entry(text: &str, desktops: &[&str]) -> Option<String> {
    let mut in_entry = false;
    let mut name = None;
    let mut application = false;
    let mut hidden = false;
    let mut only_show = None;
    let mut not_show = None;
    for line in text.lines().map(str::trim) {
        if line.starts_with('[') && line.ends_with(']') {
            in_entry = line == "[Desktop Entry]";
            continue;
        }
        if !in_entry || line.starts_with('#') {
            continue;
        }
        let Some((key, value)) = line.split_once('=') else { continue };
        match key {
            "Name" if !value.trim().is_empty() => name = Some(value.trim().to_string()),
            "Type" => application = value.trim() == "Application",
            "Hidden" | "NoDisplay" if value.trim().eq_ignore_ascii_case("true") => hidden = true,
            "OnlyShowIn" => only_show = Some(value),
            "NotShowIn" => not_show = Some(value),
            _ => {}
        }
    }
    let matches = |value: &str| value.split(';').any(|entry| desktops.contains(&entry));
    if !application || hidden || only_show.is_some_and(|v| !matches(v)) ||
        not_show.is_some_and(matches) {
        return None;
    }
    name
}

#[cfg(target_os = "linux")]
pub fn list() -> io::Result<Vec<Application>> {
    use std::collections::HashSet;
    use std::path::{Path, PathBuf};

    fn scan(dir: &Path, prefix: &str, entries: &mut Vec<(String, PathBuf)>) -> io::Result<()> {
        let children = match std::fs::read_dir(dir) {
            Ok(children) => children,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
            Err(error) => return Err(error),
        };
        for child in children {
            let child = child?;
            let file_type = child.file_type()?;
            let name = child.file_name();
            let name = name.to_string_lossy();
            let id = format!("{prefix}{name}");
            if file_type.is_dir() {
                scan(&child.path(), &format!("{id}-"), entries)?;
            } else if (file_type.is_file() || file_type.is_symlink()) && name.ends_with(".desktop") {
                entries.push((id, child.path()));
            }
        }
        Ok(())
    }

    if std::path::Path::new("/.flatpak-info").exists() {
        return Err(io::Error::new(io::ErrorKind::Unsupported,
            "The application menu cannot launch host applications from Flatpak"));
    }
    let home = std::env::var_os("XDG_DATA_HOME").filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|v| PathBuf::from(v).join(".local/share")));
    let mut dirs: Vec<PathBuf> = home.into_iter().collect();
    let system_dirs = std::env::var_os("XDG_DATA_DIRS")
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| "/usr/local/share:/usr/share".into());
    dirs.extend(std::env::split_paths(&system_dirs));
    let current_desktop = std::env::var("XDG_CURRENT_DESKTOP").unwrap_or_default();
    let desktops: Vec<_> = current_desktop.split(':').filter(|v| !v.is_empty()).collect();
    let mut seen = HashSet::new();
    let mut result = Vec::new();
    for dir in dirs {
        let mut entries = Vec::new();
        scan(&dir.join("applications"), "", &mut entries)?;
        entries.sort_by(|a, b| a.0.cmp(&b.0));
        for (id, path) in entries {
            if !seen.insert(id) {
                continue;
            }
            let text = match std::fs::read_to_string(&path) {
                Ok(text) => text,
                Err(error) => {
                    eprintln!("kitvr-mascot: could not read {}: {error}", path.display());
                    continue;
                }
            };
            if let Some(name) = parse_entry(&text, &desktops) {
                result.push(Application { name, path });
            }
        }
    }
    result.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
    Ok(result)
}

#[cfg(not(target_os = "linux"))]
pub fn list() -> io::Result<Vec<Application>> {
    Err(io::Error::new(io::ErrorKind::Unsupported, "Application menu is currently available on Linux only"))
}

#[cfg(target_os = "linux")]
pub fn launch(app: &Application) -> io::Result<()> {
    let output = std::process::Command::new("gio").arg("launch").arg(&app.path).output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "Could not launch {}: {}", app.name, String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    Ok(())
}

#[cfg(not(target_os = "linux"))]
pub fn launch(_app: &Application) -> io::Result<()> {
    Err(io::Error::new(io::ErrorKind::Unsupported, "Application launch is currently available on Linux only"))
}

#[cfg(all(test, target_os = "linux"))]
mod tests {
    use super::*;

    #[test]
    fn respects_desktop_entry_visibility() {
        assert_eq!(parse_entry("[Desktop Entry]\nType=Application\nName=Editor\n", &["GNOME"]),
                   Some("Editor".into()));
        assert!(parse_entry("[Desktop Entry]\nType=Application\nName=Hidden\nNoDisplay=true", &["GNOME"]).is_none());
        assert!(parse_entry("[Desktop Entry]\nType=Application\nName=Other\nOnlyShowIn=KDE;", &["GNOME"]).is_none());
        assert!(parse_entry("[Desktop Entry]\nType=Application\nName=Other\nNotShowIn=GNOME;", &["GNOME"]).is_none());
        assert!(parse_entry("[Other]\nType=Application\nName=Other", &["GNOME"]).is_none());
    }
}
