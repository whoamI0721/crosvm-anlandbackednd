#![allow(unused)]

use std::ffi::OsStr;
use std::os::unix::io::RawFd;

#[derive(Debug)]
pub struct Error(pub libc::c_int);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "minijail error: {}", self.0)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

pub struct Minijail;

impl Minijail {
    pub fn new() -> Result<Self> { Ok(Minijail) }

    // namespace
    pub fn namespace_vfs(&mut self) {}
    pub fn namespace_net(&mut self) {}
    pub fn namespace_pids(&mut self) {}
    pub fn namespace_user(&mut self) {}
    pub fn namespace_user_disable_setgroups(&mut self) {}
    pub fn namespace_cgroups(&mut self) {}
    pub fn no_new_privs(&mut self) {}

    // pivot root
    pub fn enter_pivot_root(&mut self, _: &std::path::Path) -> Result<()> { Ok(()) }

    // remount
    pub fn set_remount_mode(&mut self, _: u64) {}
    pub fn remount_mode(&mut self, _: u64) {}

    // caps
    pub fn use_caps(&mut self, _: u64) {}
    pub fn set_ambient_caps(&mut self) {}

    // uid/gid
    pub fn change_uid(&mut self, _: libc::uid_t) {}
    pub fn change_gid(&mut self, _: libc::gid_t) {}
    pub fn uidmap(&mut self, _: &str) -> Result<()> { Ok(()) }
    pub fn gidmap(&mut self, _: &str) -> Result<()> { Ok(()) }

    // groups
    pub fn inherit_supplementary_groups(&mut self) {}
    pub fn set_inherit_supplementary_groups(&mut self, _: bool) {}
    pub fn keep_supplementary_groups(&mut self) {}

    // proc
    pub fn set_remount_proc_readonly(&mut self, _: bool) {}

    // fd management
    pub fn keep_fds(&mut self, _: &[RawFd]) {}
    pub fn close_open_fds(&mut self) {}

    // rlimit
    pub fn set_rlimit(&mut self, _: i32, _: u64, _: u64) -> Result<()> { Ok(()) }

    // mounts
    pub fn mount_bind<P: AsRef<std::path::Path>>(&mut self, _: P, _: P, _: bool) -> Result<()> { Ok(()) }
    pub fn mount<P: AsRef<std::path::Path>>(&mut self, _: P, _: P, _: &str, _: usize) -> Result<()> { Ok(()) }
    pub fn mount_with_data<P: AsRef<std::path::Path>>(&mut self, _: P, _: P, _: &str, _: usize, _: &str) -> Result<()> { Ok(()) }

    // seccomp
    pub fn parse_seccomp_filters(&mut self, _: &std::path::Path) -> Result<()> { Ok(()) }
    pub fn parse_seccomp_program(&mut self, _: &std::path::Path) -> Result<()> { Ok(()) }
    pub fn parse_seccomp_bytes(&mut self, _: &[u8]) -> Result<()> { Ok(()) }
    pub fn log_seccomp_filter_failures(&mut self) {}
    pub fn set_seccomp_filter_tsync(&mut self) {}
    pub fn use_seccomp_filter(&mut self) {}

    // execution
    pub fn run(&mut self, _: &[&std::ffi::CStr], _: &[String]) -> Result<()> { Ok(()) }
    pub fn disable_multithreaded_check(&mut self) {}
    pub fn fork(&self, _keep_rds: Option<&[RawFd]>) -> Result<libc::pid_t> {
        let pid = unsafe { libc::fork() };
        if pid < 0 { Err(Error(-1)) } else { Ok(pid) }
    }
    pub fn run_as_init(&mut self) -> Result<()> { Ok(()) }
    pub fn kill(&mut self) -> Result<()> { Ok(()) }
    pub fn try_clone(&self) -> Result<Self> { Ok(Minijail) }
    pub fn run_command(&self, _cmd: Command) -> Result<libc::pid_t> {
        Ok(unsafe { libc::fork() })
    }
}

pub struct Command;

impl Command {
    pub fn new_for_path(
        _path: &std::path::Path,
        _fds: &[RawFd],
        _args: &[&str],
        _env: Option<&[&str]>,
    ) -> Result<Self> {
        Ok(Command)
    }
}

pub fn is_inside_minijail() -> bool { false }