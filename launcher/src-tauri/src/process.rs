//! Owns worker lifetime; observing boot and process exit stays with the callers.
use std::io;
use std::ops::{Deref, DerefMut};
use std::process::{Child, Command, ExitStatus};
use std::time::Duration;

pub struct Worker {
    child: Child,
    #[cfg(windows)]
    control: std::sync::Arc<win::Control>,
}

impl Deref for Worker {
    type Target = Child;
    fn deref(&self) -> &Child { &self.child }
}
impl DerefMut for Worker {
    fn deref_mut(&mut self) -> &mut Child { &mut self.child }
}
impl Worker {
    pub fn kill(&mut self) -> io::Result<()> {
        #[cfg(windows)]
        return self.control.terminate();
        #[cfg(not(windows))]
        self.child.kill()
    }

    pub fn wait(&mut self) -> io::Result<ExitStatus> {
        let status = self.child.wait()?;
        #[cfg(windows)]
        if let Err(e) = self.control.terminate() {
            log::warn!("Could not end exited worker's descendants: {e}");
        }
        Ok(status)
    }

    pub fn try_wait(&mut self) -> io::Result<Option<ExitStatus>> {
        let status = self.child.try_wait()?;
        #[cfg(windows)]
        if status.is_some() {
            if let Err(e) = self.control.terminate() {
                log::warn!("Could not end exited worker's descendants: {e}");
            }
        }
        Ok(status)
    }
}

#[cfg(windows)]
impl std::os::windows::io::AsRawHandle for Worker {
    fn as_raw_handle(&self) -> std::os::windows::io::RawHandle {
        std::os::windows::io::AsRawHandle::as_raw_handle(&self.child)
    }
}

#[cfg(windows)]
impl Drop for Worker {
    fn drop(&mut self) {
        // Also covers failure to start the caller's exit-watcher thread.
        let _ = self.control.terminate();
        let _ = self.child.wait();
    }
}

/// Starts a worker with the caller's flags and immediately attaches its lifetime to a job.
pub fn spawn(command: &mut Command, flags: u32) -> io::Result<Worker> {
    #[cfg(windows)]
    return win::spawn(command, flags);
    #[cfg(not(windows))]
    {
        let _ = flags;
        Ok(Worker { child: command.spawn()? })
    }
}

/// Reject new starts, request a normal window close, then end and reap every owned job.
pub fn shutdown(deadline: Duration) {
    #[cfg(windows)]
    win::shutdown(deadline);
    #[cfg(not(windows))]
    let _ = deadline;
}

#[cfg(windows)]
mod win {
    use super::*;
    use std::mem::size_of;
    use std::os::windows::io::{AsHandle, AsRawHandle, FromRawHandle, OwnedHandle};
    use std::os::windows::process::CommandExt;
    use std::sync::{Arc, Mutex, OnceLock, Weak};
    use std::time::Instant;
    use windows::core::{BOOL, PCWSTR};
    use windows::Win32::Foundation::{HANDLE, HWND, LPARAM, WPARAM};
    use windows::Win32::System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation,
        SetInformationJobObject, TerminateJobObject, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    };
    use windows::Win32::System::Threading::{
        WaitForSingleObject, CREATE_BREAKAWAY_FROM_JOB, INFINITE,
    };
    use windows::Win32::UI::WindowsAndMessaging::{
        EnumWindows, GetWindowThreadProcessId, IsWindowVisible, PostMessageW, WM_CLOSE,
    };

    fn handle(owned: &OwnedHandle) -> HANDLE { HANDLE(owned.as_raw_handle()) }
    fn owned(raw: HANDLE) -> OwnedHandle {
        // SAFETY: each caller transfers a newly created, valid owned Win32 handle.
        unsafe { OwnedHandle::from_raw_handle(raw.0) }
    }
    fn error(e: windows::core::Error) -> io::Error { io::Error::other(e) }

    pub(super) struct Control {
        job: OwnedHandle,
        process: OwnedHandle,
        pid: u32,
    }
    impl Control {
        pub(super) fn terminate(&self) -> io::Result<()> {
            // SAFETY: both handles stay owned for the duration of the call.
            unsafe { TerminateJobObject(handle(&self.job), 1) }.map_err(error)
        }
    }

    #[derive(Default)]
    struct Registry { stopping: bool, workers: Vec<Weak<Control>> }
    fn registry() -> &'static Mutex<Registry> {
        static REGISTRY: OnceLock<Mutex<Registry>> = OnceLock::new();
        REGISTRY.get_or_init(|| Mutex::new(Registry::default()))
    }

    fn new_job() -> io::Result<OwnedHandle> {
        // No SECURITY_ATTRIBUTES: the unnamed job handle is non-inheritable. Neither breakaway
        // flag is enabled, so children and grandchildren stay in the job, including nested jobs.
        let job = owned(unsafe { CreateJobObjectW(None, PCWSTR::null()) }.map_err(error)?);
        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        unsafe {
            SetInformationJobObject(handle(&job), JobObjectExtendedLimitInformation,
                &limits as *const _ as *const _, size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32)
        }.map_err(error)?;
        Ok(job)
    }

    fn attach(child: &mut Child, job: HANDLE) -> io::Result<()> {
        let result = unsafe { AssignProcessToJobObject(job, HANDLE(child.as_raw_handle())) }.map_err(error);
        if let Err(e) = result {
            // Assignment failure must not leave the worker running without containment.
            let _ = child.kill();
            let _ = child.wait();
            return Err(e);
        }
        Ok(())
    }

    pub(super) fn spawn(command: &mut Command, flags: u32) -> io::Result<Worker> {
        let mut registry = registry().lock().unwrap_or_else(|e| e.into_inner());
        if registry.stopping { return Err(io::Error::other("Launcher is shutting down")); }
        let job = new_job()?;
        // Rust's Command restricts inherited handles to its stdio handles. Our job is never one
        // of them. Do not allow a caller to request CREATE_BREAKAWAY_FROM_JOB.
        let flags = flags & !CREATE_BREAKAWAY_FROM_JOB.0;
        // A child started in the instant before assignment would escape this job. The emulator
        // and warm-up helper start no child that early; this avoids an unreliable suspended-start
        // thread search while retaining containment for descendants started after assignment.
        let mut child = command.creation_flags(flags).spawn()?;
        attach(&mut child, handle(&job))?;
        let process = match child.as_handle().try_clone_to_owned() {
            Ok(process) => process,
            Err(e) => {
                let _ = unsafe { TerminateJobObject(handle(&job), 1) };
                let _ = child.kill();
                let _ = child.wait();
                return Err(e);
            }
        };
        let control = Arc::new(Control { job, process, pid: child.id() });
        registry.workers.retain(|w| w.strong_count() != 0);
        registry.workers.push(Arc::downgrade(&control));
        Ok(Worker { child, control })
    }

    unsafe extern "system" fn close_window(hwnd: HWND, param: LPARAM) -> BOOL {
        let mut pid = 0;
        unsafe { GetWindowThreadProcessId(hwnd, Some(&mut pid)); }
        if pid == param.0 as u32 && unsafe { IsWindowVisible(hwnd) }.as_bool() {
            let _ = unsafe { PostMessageW(Some(hwnd), WM_CLOSE, WPARAM(0), LPARAM(0)) };
        }
        BOOL(1)
    }

    fn stop(workers: Vec<Arc<Control>>, grace: Duration) {
        let deadline = Instant::now() + grace;
        for worker in &workers {
            // WM_CLOSE is the existing Qt shutdown request. A headless warm-up has no window
            // or graceful pipe protocol today; it reaches the same bounded fallback below.
            let _ = unsafe { EnumWindows(Some(close_window), LPARAM(worker.pid as isize)) };
        }
        for worker in &workers {
            let millis = deadline.saturating_duration_since(Instant::now()).as_millis().min(u32::MAX as u128 - 1) as u32;
            unsafe { WaitForSingleObject(handle(&worker.process), millis); }
        }
        for worker in &workers {
            if let Err(e) = worker.terminate() { log::warn!("Could not end worker job: {e}"); }
            // A killed job's process becomes signalled; this does not race the caller's waiter.
            unsafe { WaitForSingleObject(handle(&worker.process), INFINITE); }
        }
    }

    pub(super) fn shutdown(grace: Duration) {
        let workers = {
            let mut registry = registry().lock().unwrap_or_else(|e| e.into_inner());
            registry.stopping = true;
            registry.workers.drain(..).filter_map(|w| w.upgrade()).collect()
        };
        stop(workers, grace);
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use std::io::Write;
        use std::process::Stdio;
        use windows::Win32::Foundation::{GetHandleInformation, HANDLE_FLAG_INHERIT, WAIT_OBJECT_0};
        use windows::Win32::System::Diagnostics::ToolHelp::{
            CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
        };
        use windows::Win32::System::Threading::{
            GetCurrentProcess, GetProcessHandleCount, OpenProcess, PROCESS_SYNCHRONIZE,
        };

        fn command() -> Command {
            // Absolute system program, no user shell autorun, no fixture executable or game.
            let root = std::path::PathBuf::from(std::env::var_os("SystemRoot").expect("SystemRoot"));
            let mut command = Command::new(root.join("System32/cmd.exe"));
            // Wait for the test to assign all jobs before allowing cmd to start ping.
            // /s /c strips the outer quotes around the command script.
            let mut script = std::ffi::OsString::from("\"set /p GUARANA_JOB_TEST_READY= & \"");
            script.push(root.join("System32/ping.exe"));
            script.push("\" -t 127.0.0.1 >nul\"");
            command.args(["/d", "/s", "/c"]).raw_arg(script)
                .stdin(Stdio::piped()).stdout(Stdio::null()).stderr(Stdio::null());
            command
        }

        fn descendant(child: &mut Child) -> OwnedHandle {
            let pid = child.id();
            child.stdin.take().expect("fixture stdin").write_all(b"ready\r\n").unwrap();
            let deadline = Instant::now() + Duration::from_secs(10);
            loop {
                let snapshot = owned(unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) }.unwrap());
                let mut entry = PROCESSENTRY32W { dwSize: size_of::<PROCESSENTRY32W>() as u32, ..Default::default() };
                unsafe { Process32FirstW(handle(&snapshot), &mut entry) }.unwrap();
                let mut observed = Vec::new();
                loop {
                    let name_len = entry.szExeFile.iter().position(|c| *c == 0).unwrap_or(entry.szExeFile.len());
                    let name = String::from_utf16_lossy(&entry.szExeFile[..name_len]);
                    if entry.th32ParentProcessID == pid {
                        observed.push(format!("{}:{name}", entry.th32ProcessID));
                    }
                    if entry.th32ParentProcessID == pid && name.eq_ignore_ascii_case("ping.exe") {
                        return owned(unsafe { OpenProcess(PROCESS_SYNCHRONIZE, false, entry.th32ProcessID) }.unwrap());
                    }
                    if unsafe { Process32NextW(handle(&snapshot), &mut entry) }.is_err() { break; }
                }
                let state = child.try_wait().expect("fixture process state");
                assert!(state.is_none() && Instant::now() < deadline,
                    "cmd did not start its ping child: pid={pid}, state={}, exit_code={:?}, snapshot children={observed:?}",
                    if state.is_some() { "exited" } else { "still running" },
                    state.and_then(|status| status.code()));
                std::thread::sleep(Duration::from_millis(20));
            }
        }

        fn ended(process: &OwnedHandle) {
            assert_eq!(unsafe { WaitForSingleObject(handle(process), 5000) }, WAIT_OBJECT_0);
        }

        #[test]
        fn closing_job_ends_child_and_grandchild_in_nested_jobs() {
            let outer = new_job().unwrap();
            let job = new_job().unwrap();
            let mut flags = 0;
            unsafe { GetHandleInformation(handle(&job), &mut flags) }.unwrap();
            assert_eq!(flags & HANDLE_FLAG_INHERIT.0, 0);
            let mut child = command().creation_flags(0x0800_0000).spawn().unwrap();
            let process = child.as_handle().try_clone_to_owned().unwrap();
            attach(&mut child, handle(&outer)).unwrap();
            attach(&mut child, handle(&job)).unwrap();
            let grandchild = descendant(&mut child);
            // No TerminateJobObject call: this models the launcher's last handle closing on death.
            drop(job);
            ended(&process);
            ended(&grandchild);
            child.wait().unwrap();
            drop(outer);
        }

        #[test]
        fn assignment_failure_terminates_and_reaps_the_child() {
            let mut child = command().creation_flags(0x0800_0000).spawn().unwrap();
            let process = child.as_handle().try_clone_to_owned().unwrap();
            assert!(attach(&mut child, HANDLE::default()).is_err());
            ended(&process);
            assert!(child.try_wait().unwrap().is_some());
        }

        #[test]
        fn cancellation_and_deadline_end_descendants_and_allow_restart() {
            struct ResetStopping;
            impl Drop for ResetStopping {
                fn drop(&mut self) {
                    registry().lock().unwrap_or_else(|e| e.into_inner()).stopping = false;
                }
            }
            let reset = ResetStopping;
            let mut first = spawn(&mut command(), 0x0800_0000).unwrap();
            let grandchild = descendant(&mut first);
            first.kill().unwrap();
            first.wait().unwrap();
            ended(&grandchild);
            drop(first);
            let mut next = spawn(&mut command(), 0x0800_0000).unwrap();
            let process = next.child.as_handle().try_clone_to_owned().unwrap();
            let grandchild = descendant(&mut next);
            let started = Instant::now();
            super::super::shutdown(Duration::from_millis(50));
            assert!(started.elapsed() < Duration::from_secs(5));
            assert!(spawn(&mut command(), 0x0800_0000).is_err());
            assert!(registry().lock().unwrap_or_else(|e| e.into_inner()).workers.is_empty());
            ended(&process);
            ended(&grandchild);
            next.wait().unwrap();
            drop(next);
            drop(reset);
            drop(spawn(&mut command(), 0x0800_0000).unwrap());
        }

        fn handle_count() -> u32 {
            let mut count = 0;
            unsafe { GetProcessHandleCount(GetCurrentProcess(), &mut count) }.unwrap();
            count
        }

        #[test]
        fn repeated_starts_release_handles() {
            // Warm up Rust/Win32 lazy state before taking the baseline. CI serializes tests so
            // unrelated crate tests cannot change this process-wide count during the assertion.
            drop(spawn(&mut command(), 0x0800_0000).unwrap());
            let before = handle_count();
            for _ in 0..32 {
                let worker = spawn(&mut command(), 0x0800_0000).unwrap();
                drop(worker);
            }
            assert!(handle_count() <= before + 2, "worker handles leaked across 32 starts");
        }
    }
}
