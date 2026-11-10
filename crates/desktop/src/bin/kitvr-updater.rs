use std::env;
use std::fs;
#[cfg(windows)]
use std::fs::File;
use std::io;
#[cfg(windows)]
use std::io::copy;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    if let Err(error) = run() {
        eprintln!("KitVR update failed: {error}");
        std::process::exit(1);
    }
}
#[cfg(not(windows))]
use std::thread;
#[cfg(not(windows))]
use std::time::Duration;

fn run() -> io::Result<()> {
    let mut args = env::args().skip(1);
    let mode = args.next().ok_or_else(|| invalid("missing update mode"))?;
    let parent_pid = args
        .next()
        .ok_or_else(|| invalid("missing parent process id"))?
        .parse::<u32>()
        .map_err(|_| invalid("invalid parent process id"))?;
    let package = PathBuf::from(
        args.next()
            .ok_or_else(|| invalid("missing update package"))?,
    );
    let target = PathBuf::from(args.next().ok_or_else(|| invalid("missing install path"))?);
    wait_for_parent(parent_pid)?;

    #[cfg(windows)]
    {
        if mode == "windows" {
            update_windows(&package, &target)
        } else {
            Err(invalid("unknown update mode"))
        }
    }
    #[cfg(not(windows))]
    {
        if mode == "appimage" {
            update_appimage(&package, &target)
        } else {
            Err(invalid("unknown update mode"))
        }
    }
}

#[cfg(windows)]
fn update_windows(package: &Path, install_dir: &Path) -> io::Result<()> {
    let staging = install_dir.join(format!(".kitvr-update-{}", std::process::id()));
    fs::create_dir_all(&staging)?;
    let archive_file = File::open(package)?;
    let mut archive = zip::ZipArchive::new(archive_file).map_err(invalid_data)?;
    for name in ["kitvr-desktop.exe", "kitvr-server.exe"] {
        let mut entry = archive.by_name(name).map_err(invalid_data)?;
        let output = staging.join(name);
        let mut file = File::create(output)?;
        copy(&mut entry, &mut file)?;
    }

    replace_file(
        &staging.join("kitvr-server.exe"),
        &install_dir.join("kitvr-server.exe"),
    )?;
    let desktop = install_dir.join("kitvr-desktop.exe");
    replace_file(&staging.join("kitvr-desktop.exe"), &desktop)?;
    let _ = fs::remove_dir_all(staging);
    let _ = fs::remove_file(package);
    Command::new(desktop).spawn()?;
    Ok(())
}

#[cfg(not(windows))]
fn update_appimage(package: &Path, target: &Path) -> io::Result<()> {
    let staging = target.with_extension("AppImage.new");
    fs::copy(package, &staging)?;
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(&staging, fs::Permissions::from_mode(0o755))?;
    replace_file(&staging, target)?;
    let _ = fs::remove_file(package);
    Command::new(target).spawn()?;
    Ok(())
}

fn replace_file(source: &Path, target: &Path) -> io::Result<()> {
    let backup = target.with_extension("kitvr-backup");
    if backup.exists() {
        fs::remove_file(&backup)?;
    }
    let had_target = target.exists();
    if had_target {
        fs::rename(target, &backup)?;
    }
    if let Err(error) = fs::rename(source, target) {
        if had_target {
            let _ = fs::rename(&backup, target);
        }
        return Err(error);
    }
    if had_target {
        let _ = fs::remove_file(backup);
    }
    Ok(())
}

#[cfg(windows)]
fn wait_for_parent(pid: u32) -> io::Result<()> {
    use std::ffi::c_void;
    use std::ptr;

    const SYNCHRONIZE: u32 = 0x0010_0000;
    const INFINITE: u32 = 0xffff_ffff;
    const ERROR_INVALID_PARAMETER: u32 = 87;
    #[link(name = "kernel32")]
    extern "system" {
        fn OpenProcess(access: u32, inherit: i32, process_id: u32) -> *mut c_void;
        fn WaitForSingleObject(handle: *mut c_void, milliseconds: u32) -> u32;
        fn CloseHandle(handle: *mut c_void) -> i32;
        fn GetLastError() -> u32;
    }

    let handle = unsafe { OpenProcess(SYNCHRONIZE, 0, pid) };
    if handle == ptr::null_mut() {
        return if unsafe { GetLastError() } == ERROR_INVALID_PARAMETER {
            Ok(())
        } else {
            Err(io::Error::last_os_error())
        };
    }
    let result = unsafe { WaitForSingleObject(handle, INFINITE) };
    unsafe { CloseHandle(handle) };
    if result == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

#[cfg(not(windows))]
fn wait_for_parent(pid: u32) -> io::Result<()> {
    while Path::new(&format!("/proc/{pid}")).exists() {
        thread::sleep(Duration::from_millis(100));
    }
    Ok(())
}

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

#[cfg(windows)]
fn invalid_data(error: impl std::fmt::Display) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, error.to_string())
}

#[cfg(test)]
mod tests {
    use super::replace_file;
    use std::fs;

    #[test]
    fn replaces_an_existing_binary_atomically() {
        let directory =
            std::env::temp_dir().join(format!("kitvr-updater-test-{}", std::process::id()));
        fs::create_dir_all(&directory).unwrap();
        let source = directory.join("new");
        let target = directory.join("app.bin");
        fs::write(&source, b"new version").unwrap();
        fs::write(&target, b"old version").unwrap();

        replace_file(&source, &target).unwrap();

        assert_eq!(fs::read(&target).unwrap(), b"new version");
        assert!(!source.exists());
        let _ = fs::remove_dir_all(directory);
    }
}
