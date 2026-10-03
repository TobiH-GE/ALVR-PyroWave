// How busy the machine and this process are (logged every 5 s by the video send thread, "Machine
// load: ..."): the encoder, SteamVR and the game share both the CPU and the GPU, and a run that
// looks bad is worth nothing without knowing whether either was saturated at the time. "This
// process" is vrserver, which hosts the driver: SteamVR's server plus the whole streamer, encoder
// included; the game and SteamVR's compositor are other processes. Ported from the JPEG XS
// streamer.
#[cfg(windows)]
pub mod load {
    use std::ffi::c_void;

    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct FileTime {
        low: u32,
        high: u32,
    }
    impl FileTime {
        fn as_u64(self) -> u64 {
            (self.high as u64) << 32 | self.low as u64
        }
    }

    // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, one per logical processor (48 bytes). Times in
    // 100 ns units; kernel includes idle, as with GetSystemTimes.
    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct ProcessorTimes {
        idle: i64,
        kernel: i64,
        user: i64,
        dpc: i64,
        interrupt: i64,
        interrupt_count: u32,
    }
    const SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION: u32 = 8;

    const SYSTEM_PROCESS_INFORMATION: u32 = 5;
    const STATUS_INFO_LENGTH_MISMATCH: i32 = 0xC000_0004_u32 as i32;
    // SYSTEM_PROCESS_INFORMATION (x64): header 0x100 bytes, then NumberOfThreads entries of
    // SYSTEM_THREAD_INFORMATION, 0x50 bytes each. Read by offset from the returned buffer.
    const PROCESS_ENTRY_SIZE: usize = 0x100;
    const THREAD_ENTRY_SIZE: usize = 0x50;

    #[link(name = "user32")]
    extern "system" {
        fn GetForegroundWindow() -> *mut c_void;
        fn GetWindowThreadProcessId(window: *mut c_void, process_id: *mut u32) -> u32;
    }

    #[link(name = "ntdll")]
    extern "system" {
        fn NtQuerySystemInformation(
            class: u32,
            info: *mut c_void,
            length: u32,
            returned: *mut u32,
        ) -> i32;
    }

    #[link(name = "kernel32")]
    extern "system" {
        fn GetSystemTimes(idle: *mut FileTime, kernel: *mut FileTime, user: *mut FileTime) -> i32;
        fn GetCurrentProcess() -> *mut c_void;
        fn GetCurrentProcessId() -> u32;
        fn GetProcessTimes(
            process: *mut c_void,
            creation: *mut FileTime,
            exit: *mut FileTime,
            kernel: *mut FileTime,
            user: *mut FileTime,
        ) -> i32;
    }

    #[derive(Default)]
    pub struct Load {
        prev_idle: u64,
        prev_busy: u64,
        prev_process: u64,
        prev_cores: Vec<(u64, u64)>,
        cores: f64,
        gpu: Option<Nvml>,
        // Per-thread accounting (threads_line): the query buffer, kept between calls, and each
        // thread's CPU time (100 ns) at the last call, by thread id.
        process_buffer: Vec<u64>,
        prev_threads: std::collections::HashMap<u64, u64>,
        prev_processes: std::collections::HashMap<u64, u64>,
    }

    fn u32_at(bytes: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
    }
    fn u64_at(bytes: &[u8], offset: usize) -> u64 {
        u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
    }

    struct ThreadSample {
        pid: u64,
        tid: u64,
        time: u64,
    }

    impl Load {
        pub fn new() -> Self {
            Load {
                cores: std::thread::available_parallelism().map_or(1.0, |n| n.get() as f64),
                gpu: Nvml::open(),
                ..Default::default()
            }
        }

        // The four busiest logical processors over the window as (busy %, index), and how many
        // are over 90 % and over 70 %. On an SMT CPU a saturated thread that moves between the two
        // logical processors of one core reads as two at 60-70 %, hence the list and the 70 %
        // count rather than the single busiest one.
        fn busiest_cores(&mut self) -> Option<(Vec<(f64, usize)>, usize, usize)> {
            let (_, _, _, all) = self.core_percentages()?;
            let over_90 = all.iter().filter(|&&(p, _)| p > 90.0).count();
            let over_70 = all.iter().filter(|&&(p, _)| p > 70.0).count();
            let mut top = all;
            top.sort_by(|a, b| b.0.total_cmp(&a.0));
            top.truncate(4);
            Some((top, over_90, over_70))
        }

        // Every logical processor's busy % over the window: (busiest %, its index, processors
        // over 90 %, all as (busy %, index)).
        // A game held back by one thread (its main or render thread) shows here long before it
        // shows in the machine total, where one saturated core of 24 is 4 %. Windows moves
        // threads between cores, so a saturated thread can also smear over several at 40-60 %.
        // One call for the calling thread's processor group (up to 64 logical processors).
        fn core_percentages(&mut self) -> Option<(f64, usize, usize, Vec<(f64, usize)>)> {
            let mut times = vec![ProcessorTimes::default(); 64];
            let mut returned = 0u32;
            let size = std::mem::size_of::<ProcessorTimes>();
            let status = unsafe {
                NtQuerySystemInformation(
                    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION,
                    times.as_mut_ptr() as *mut c_void,
                    (times.len() * size) as u32,
                    &mut returned,
                )
            };
            if status != 0 {
                return None;
            }
            let count = (returned as usize / size).min(times.len());
            let now: Vec<(u64, u64)> = times[..count]
                .iter()
                .map(|t| {
                    let idle = t.idle as u64;
                    (idle, (t.kernel as u64 + t.user as u64).saturating_sub(idle))
                })
                .collect();
            let prev = std::mem::replace(&mut self.prev_cores, now);
            if prev.len() != self.prev_cores.len() {
                return None;
            }
            let mut busiest = (0.0, 0, 0, Vec::new());
            for (index, (&(idle, busy), &(prev_idle, prev_busy))) in
                self.prev_cores.iter().zip(prev.iter()).enumerate()
            {
                let d_idle = idle.saturating_sub(prev_idle) as f64;
                let d_busy = busy.saturating_sub(prev_busy) as f64;
                if d_idle + d_busy <= 0.0 {
                    continue;
                }
                let percent = 100.0 * d_busy / (d_idle + d_busy);
                busiest.3.push((percent, index));
                if percent > 90.0 {
                    busiest.2 += 1;
                }
                if percent > busiest.0 {
                    busiest.0 = percent;
                    busiest.1 = index;
                }
            }
            Some(busiest)
        }

        // Every thread's CPU time and every process's image name, in one system call.
        fn thread_snapshot(&mut self) -> Option<(Vec<ThreadSample>, Vec<(u64, String, u64)>)> {
            if self.process_buffer.is_empty() {
                self.process_buffer = vec![0u64; 1 << 17]; // 1 MB
            }
            loop {
                let mut returned = 0u32;
                let length = (self.process_buffer.len() * 8) as u32;
                let status = unsafe {
                    NtQuerySystemInformation(
                        SYSTEM_PROCESS_INFORMATION,
                        self.process_buffer.as_mut_ptr() as *mut c_void,
                        length,
                        &mut returned,
                    )
                };
                if status == STATUS_INFO_LENGTH_MISMATCH && length < 64 << 20 {
                    let words = (returned as usize).max(length as usize * 2) / 8 + 8192;
                    self.process_buffer = vec![0u64; words];
                    continue;
                }
                if status != 0 {
                    return None;
                }
                break;
            }
            let bytes: &[u8] = unsafe {
                std::slice::from_raw_parts(
                    self.process_buffer.as_ptr() as *const u8,
                    self.process_buffer.len() * 8,
                )
            };
            let base = bytes.as_ptr() as usize;
            let mut threads = Vec::new();
            let mut processes = Vec::new();
            let mut offset = 0usize;
            while offset + PROCESS_ENTRY_SIZE <= bytes.len() {
                let count = u32_at(bytes, offset + 0x04) as usize;
                let pid = u64_at(bytes, offset + 0x50);
                let time = u64_at(bytes, offset + 0x28) + u64_at(bytes, offset + 0x30);
                // ImageName: Length u16 @0x38, Buffer @0x40, pointing into this same buffer.
                let name_length = (u32_at(bytes, offset + 0x38) & 0xffff) as usize;
                let name_pointer = u64_at(bytes, offset + 0x40) as usize;
                let name = if name_pointer >= base
                    && name_pointer + name_length <= base + bytes.len()
                    && name_length > 0
                {
                    let start = name_pointer - base;
                    let units: Vec<u16> = bytes[start..start + name_length]
                        .chunks_exact(2)
                        .map(|c| u16::from_le_bytes([c[0], c[1]]))
                        .collect();
                    String::from_utf16_lossy(&units)
                } else {
                    "System Idle".to_string()
                };
                processes.push((pid, name, time));
                // Process 0 is the idle loop: one "thread" per processor, always busy.
                if pid != 0 {
                    for index in 0..count {
                        let entry = offset + PROCESS_ENTRY_SIZE + index * THREAD_ENTRY_SIZE;
                        if entry + THREAD_ENTRY_SIZE > bytes.len() {
                            break;
                        }
                        threads.push(ThreadSample {
                            pid,
                            tid: u64_at(bytes, entry + 0x30),
                            time: u64_at(bytes, entry) + u64_at(bytes, entry + 0x08),
                        });
                    }
                }
                let next = u32_at(bytes, offset) as usize;
                if next == 0 {
                    break;
                }
                offset += next;
            }
            Some((threads, processes))
        }

        // "Busiest threads: ..." line: the three busiest threads on the machine with their
        // process, and the foreground process (the game, normally) as cores used with its own
        // busiest thread. 100 % is one logical processor for the whole window. A game held back
        // by one thread shows a thread near 100 % here however Windows spreads it over cores.
        // Walks every thread on the machine (about a millisecond), so it must not run on a
        // thread with deadlines.
        pub fn threads_line(&mut self, seconds: f64) -> String {
            let Some((threads, processes)) = self.thread_snapshot() else {
                return String::new();
            };
            let window = seconds * 1e7; // 100 ns units
            let names: std::collections::HashMap<u64, &str> = processes
                .iter()
                .map(|(pid, name, _)| (*pid, name.as_str()))
                .collect();
            let primed = !self.prev_threads.is_empty() && window > 0.0;

            let mut busiest: Vec<(f64, u64)> = Vec::new();
            let mut by_process: std::collections::HashMap<u64, f64> = Default::default();
            let mut now_threads = std::collections::HashMap::with_capacity(threads.len());
            for t in &threads {
                if let Some(&prev) = self.prev_threads.get(&t.tid) {
                    if primed && t.time >= prev {
                        let percent = 100.0 * (t.time - prev) as f64 / window;
                        busiest.push((percent, t.pid));
                        let top = by_process.entry(t.pid).or_default();
                        *top = top.max(percent);
                    }
                }
                now_threads.insert(t.tid, t.time);
            }
            self.prev_threads = now_threads;

            let mut foreground_pid = 0u32;
            unsafe {
                GetWindowThreadProcessId(GetForegroundWindow(), &mut foreground_pid);
            }
            let foreground_pid = foreground_pid as u64;
            let mut foreground_cores = None;
            let mut now_processes = std::collections::HashMap::with_capacity(processes.len());
            for (pid, _, time) in &processes {
                if let Some(&prev) = self.prev_processes.get(pid) {
                    if primed && *pid == foreground_pid && *time >= prev {
                        foreground_cores = Some((*time - prev) as f64 / window);
                    }
                }
                now_processes.insert(*pid, *time);
            }
            self.prev_processes = now_processes;
            if !primed {
                return String::new();
            }

            busiest.sort_by(|a, b| b.0.total_cmp(&a.0));
            let top = busiest
                .iter()
                .take(3)
                .map(|(percent, pid)| {
                    format!("{} {percent:.0} %", names.get(pid).copied().unwrap_or("?"))
                })
                .collect::<Vec<_>>()
                .join(", ");
            let foreground = match foreground_cores {
                Some(cores) => format!(
                    "foreground {}: {cores:.1} cores, busiest thread {:.0} %",
                    names.get(&foreground_pid).copied().unwrap_or("?"),
                    by_process.get(&foreground_pid).copied().unwrap_or(0.0)
                ),
                None => "foreground: not found".to_string(),
            };
            format!("{top} | {foreground}")
        }

        // "cpu 34 % of the machine, this process 12 % (2.9 of 24 cores), busiest cores 97 % (cpu 5),
        // ...; 1 over 90 %, 2 over 70 % | gpu 61 % total, ... | ~9.4 ms gpu per frame at 89.8 fps"
        // frames and seconds: video frames sent in the window, for the GPU time per frame. The
        // GPU percentage hides a frame rate locked to half the display rate: the same ~10 ms of
        // GPU work per frame reads 90 % at 90 fps and 45 % at 45 fps.
        pub fn line(&mut self, frames: u32, seconds: f64) -> String {
            let (mut idle, mut kernel, mut user) = Default::default();
            let (mut c, mut e, mut pkernel, mut puser) = Default::default();
            unsafe {
                GetSystemTimes(&mut idle, &mut kernel, &mut user);
                GetProcessTimes(
                    GetCurrentProcess(),
                    &mut c,
                    &mut e,
                    &mut pkernel,
                    &mut puser,
                );
            }
            // The kernel total already includes idle, so busy is kernel + user - idle.
            let idle = idle.as_u64();
            let busy = kernel.as_u64() + user.as_u64() - idle;
            let process = pkernel.as_u64() + puser.as_u64();

            let d_idle = idle.saturating_sub(self.prev_idle) as f64;
            let d_busy = busy.saturating_sub(self.prev_busy) as f64;
            let d_process = process.saturating_sub(self.prev_process) as f64;
            self.prev_idle = idle;
            self.prev_busy = busy;
            self.prev_process = process;

            let total = d_idle + d_busy;
            let (machine, mine, cores_used) = if total > 0.0 {
                (
                    100.0 * d_busy / total,
                    100.0 * d_process / total,
                    d_process / total * self.cores,
                )
            } else {
                (0.0, 0.0, 0.0)
            };

            let busiest = match self.busiest_cores() {
                Some((top, over_90, over_70)) => format!(
                    ", busiest cores {}; {over_90} over 90 %, {over_70} over 70 %",
                    top.iter()
                        .map(|(percent, index)| format!("{percent:.0} % (cpu {index})"))
                        .collect::<Vec<_>>()
                        .join(", ")
                ),
                None => String::new(),
            };

            // The foreground process, normally the game, for its own GPU share.
            let mut foreground_pid = 0u32;
            unsafe {
                GetWindowThreadProcessId(GetForegroundWindow(), &mut foreground_pid);
            }
            let sample = self.gpu.as_mut().and_then(|g| g.sample(foreground_pid));
            // NVML's utilization covers its own sample period (up to a second), not this whole
            // window, so this is an estimate that is good in a steady phase and rough across a
            // change of frame rate.
            let (clock, max_clock, pstate) =
                self.gpu.as_ref().map_or((None, None, None), |g| g.clocks());
            let per_frame = match sample {
                Some((whole, _, _)) if frames > 0 && seconds > 0.0 => {
                    let fps = frames as f64 / seconds;
                    let ms = whole as f64 / 100.0 * 1000.0 / fps;
                    // Scaled to the maximum clock: roughly what the frame would cost at full
                    // speed. Rough, since memory-bound work does not scale with the core clock.
                    let at_max = match (clock, max_clock) {
                        (Some(clock), Some(max)) if clock < max => {
                            format!(" (~{:.1} ms at max clock)", ms * clock as f64 / max as f64)
                        }
                        _ => String::new(),
                    };
                    // The foreground process's own share, measured rather than derived by
                    // subtracting this process from the total.
                    let foreground = match sample {
                        Some((_, _, Some(theirs))) => format!(
                            ", foreground ~{:.1} ms ({theirs} %)",
                            theirs as f64 / 100.0 * 1000.0 / fps
                        ),
                        _ => ", foreground not measurable".to_string(),
                    };
                    format!(" | ~{ms:.1} ms gpu per frame at {fps:.1} fps{at_max}{foreground}")
                }
                _ => String::new(),
            };
            let clocks = match (clock, max_clock) {
                (Some(clock), Some(max)) => format!(", clock {clock} of {max} MHz"),
                (Some(clock), None) => format!(", clock {clock} MHz"),
                _ => String::new(),
            } + &pstate.map_or(String::new(), |p| format!(" P{p}"));
            let gpu = match sample {
                Some((whole, Some(ours), _)) => {
                    format!(
                        "gpu {whole} % total, this process {ours} %, everything else {} %",
                        whole.saturating_sub(ours)
                    )
                }
                // The whole-GPU number is there but no per-process sample came back: with the
                // encoder on the CPU that is the honest answer, the process really does no
                // compute on the GPU beyond the frame readback.
                Some((whole, None, _)) => {
                    format!("gpu {whole} % total, this process not measurable")
                }
                _ => "gpu n/a".to_string(),
            };
            format!(
                "cpu {machine:.0} % of the machine, this process {mine:.0} % ({cores_used:.1} of {:.0} cores){busiest} | {gpu}{clocks}{per_frame}",
                self.cores
            )
        }
    }

    // NVML, loaded at runtime so a machine without the NVIDIA driver just reports "n/a".
    type NvmlDevice = *mut c_void;
    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct Utilization {
        gpu: u32,
        memory: u32,
    }
    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct ProcessSample {
        pid: u32,
        timestamp: u64,
        sm_util: u32,
        mem_util: u32,
        enc_util: u32,
        dec_util: u32,
    }

    type ClockFn = unsafe extern "C" fn(NvmlDevice, u32, *mut u32) -> i32;
    type PstateFn = unsafe extern "C" fn(NvmlDevice, *mut u32) -> i32;
    const NVML_CLOCK_GRAPHICS: u32 = 0;

    pub struct Nvml {
        device: NvmlDevice,
        // Optional: older drivers without them still give the utilization.
        clock: Option<ClockFn>,
        max_clock: Option<ClockFn>,
        pstate: Option<PstateFn>,
        utilization: unsafe extern "C" fn(NvmlDevice, *mut Utilization) -> i32,
        process_utilization:
            unsafe extern "C" fn(NvmlDevice, *mut ProcessSample, *mut u32, u64) -> i32,
        pid: u32,
    }

    impl Nvml {
        fn open() -> Option<Self> {
            use std::ffi::CString;
            #[link(name = "kernel32")]
            extern "system" {
                fn LoadLibraryA(name: *const u8) -> *mut c_void;
                fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
            }
            unsafe {
                let name = CString::new("nvml.dll").ok()?;
                let mut module = LoadLibraryA(name.as_ptr() as *const u8);
                if module.is_null() {
                    // Not on PATH on every driver version; this is where the installer puts it.
                    let full =
                        CString::new("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll")
                            .ok()?;
                    module = LoadLibraryA(full.as_ptr() as *const u8);
                }
                if module.is_null() {
                    return None;
                }
                let get = |symbol: &str| -> Option<*mut c_void> {
                    let symbol = CString::new(symbol).ok()?;
                    let address = GetProcAddress(module, symbol.as_ptr() as *const u8);
                    (!address.is_null()).then_some(address)
                };
                let init: unsafe extern "C" fn() -> i32 = std::mem::transmute(get("nvmlInit_v2")?);
                if init() != 0 {
                    return None;
                }
                let handle: unsafe extern "C" fn(u32, *mut NvmlDevice) -> i32 =
                    std::mem::transmute(get("nvmlDeviceGetHandleByIndex_v2")?);
                let mut device: NvmlDevice = std::ptr::null_mut();
                if handle(0, &mut device) != 0 {
                    return None;
                }
                let optional = |symbol: &str| get(symbol);
                Some(Nvml {
                    device,
                    clock: optional("nvmlDeviceGetClockInfo")
                        .map(|a| std::mem::transmute::<*mut c_void, ClockFn>(a)),
                    max_clock: optional("nvmlDeviceGetMaxClockInfo")
                        .map(|a| std::mem::transmute::<*mut c_void, ClockFn>(a)),
                    pstate: optional("nvmlDeviceGetPerformanceState")
                        .map(|a| std::mem::transmute::<*mut c_void, PstateFn>(a)),
                    utilization: std::mem::transmute(get("nvmlDeviceGetUtilizationRates")?),
                    process_utilization: std::mem::transmute(get(
                        "nvmlDeviceGetProcessUtilization",
                    )?),
                    pid: GetCurrentProcessId(),
                })
            }
        }

        // (graphics clock MHz, its maximum MHz, P-state) right now, each if NVML has it. A GPU
        // with little to do per display period clocks down; the same frame then takes longer
        // and reads as a higher utilization than the work it is.
        fn clocks(&self) -> (Option<u32>, Option<u32>, Option<u32>) {
            let read = |f: Option<ClockFn>| {
                f.and_then(|f| {
                    let mut mhz = 0u32;
                    (unsafe { f(self.device, NVML_CLOCK_GRAPHICS, &mut mhz) } == 0 && mhz > 0)
                        .then_some(mhz)
                })
            };
            let pstate = self.pstate.and_then(|f| {
                let mut state = 0u32;
                (unsafe { f(self.device, &mut state) } == 0 && state < 32).then_some(state)
            });
            (read(self.clock), read(self.max_clock), pstate)
        }

        // (whole GPU %, this process's share of the SMs %, other_pid's share), the shares if NVML
        // reported one. NVML's per-process figures are sampled on their own and come back empty
        // in many windows, so they are a cross-check, not a replacement for the whole-GPU number.
        fn sample(&mut self, other_pid: u32) -> Option<(u32, Option<u32>, Option<u32>)> {
            let mut whole = Utilization::default();
            if unsafe { (self.utilization)(self.device, &mut whole) } != 0 {
                return None;
            }
            let mut samples = [ProcessSample::default(); 128];
            let mut count = samples.len() as u32;
            // NVML wants microseconds since the epoch and returns only samples newer than that.
            // Asking from one second ago keeps it to this window; carrying the newest timestamp
            // forward instead can miss the buffer entirely once a call comes back empty.
            let since = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map_or(0, |d| d.as_micros() as u64)
                .saturating_sub(1_000_000);
            let rc = unsafe {
                (self.process_utilization)(self.device, samples.as_mut_ptr(), &mut count, since)
            };
            let mut ours = None;
            let mut other = None;
            if rc == 0 {
                for sample in samples.iter().take(count as usize) {
                    if sample.pid == self.pid {
                        ours = Some(ours.unwrap_or(0u32).max(sample.sm_util));
                    } else if other_pid != 0 && sample.pid == other_pid {
                        other = Some(other.unwrap_or(0u32).max(sample.sm_util));
                    }
                }
            }
            Some((whole.gpu, ours, other))
        }
    }
}

#[cfg(not(windows))]
pub mod load {
    #[derive(Default)]
    pub struct Load;
    impl Load {
        pub fn new() -> Self {
            Load
        }
        pub fn line(&mut self, _frames: u32, _seconds: f64) -> String {
            String::new()
        }
        pub fn threads_line(&mut self, _seconds: f64) -> String {
            String::new()
        }
    }
}

// Keeps the calling thread on the fastest cores, out of power throttling and at raised priority.
// Measured in the JPEG XS streamer (2026-09-22) on a hybrid CPU (i9-12900KF, Balanced plan): one
// socket send cost 3.3 us on a performance core, 9.5 us on an efficiency core and 23.6 us on a
// power-throttled one (EcoQoS), and Windows moved the send thread between them on its own. On a
// CPU with a single core class nothing is pinned. Returns what was done, for the log.
#[cfg(windows)]
pub fn keep_thread_on_performance_cores() -> String {
    use std::ffi::c_void;
    type Handle = *mut c_void;
    #[link(name = "kernel32")]
    extern "system" {
        fn GetCurrentThread() -> Handle;
        fn GetCurrentProcess() -> Handle;
        fn SetThreadPriority(thread: Handle, priority: i32) -> i32;
        fn SetThreadInformation(thread: Handle, class: i32, info: *const c_void, size: u32) -> i32;
        fn GetSystemCpuSetInformation(
            info: *mut u8,
            length: u32,
            returned: *mut u32,
            process: Handle,
            flags: u32,
        ) -> i32;
        fn SetThreadSelectedCpuSets(thread: Handle, ids: *const u32, count: u32) -> i32;
    }
    const THREAD_PRIORITY_HIGHEST: i32 = 2;
    const THREAD_POWER_THROTTLING: i32 = 3;
    const THREAD_POWER_THROTTLING_EXECUTION_SPEED: u32 = 1;
    #[repr(C)]
    struct PowerThrottlingState {
        version: u32,
        control_mask: u32,
        state_mask: u32,
    }

    let mut notes = Vec::new();
    unsafe {
        let thread = GetCurrentThread();

        let ok = SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST) != 0;
        notes.push(format!(
            "priority highest {}",
            if ok { "ok" } else { "FAILED" }
        ));

        // Control bit set, state bit clear: this thread opts out of EcoQoS.
        let state = PowerThrottlingState {
            version: 1,
            control_mask: THREAD_POWER_THROTTLING_EXECUTION_SPEED,
            state_mask: 0,
        };
        let ok = SetThreadInformation(
            thread,
            THREAD_POWER_THROTTLING,
            &state as *const _ as *const c_void,
            std::mem::size_of::<PowerThrottlingState>() as u32,
        ) != 0;
        notes.push(format!(
            "power throttling off {}",
            if ok { "ok" } else { "FAILED" }
        ));

        // SYSTEM_CPU_SET_INFORMATION entries: Size u32 @0, Type u32 @4, Id u32 @8,
        // EfficiencyClass u8 @18. A higher class is a faster core.
        let mut length = 0u32;
        GetSystemCpuSetInformation(std::ptr::null_mut(), 0, &mut length, GetCurrentProcess(), 0);
        let mut buffer = vec![0u8; length as usize];
        if length == 0
            || GetSystemCpuSetInformation(
                buffer.as_mut_ptr(),
                length,
                &mut length,
                GetCurrentProcess(),
                0,
            ) == 0
        {
            notes.push("cpu sets unavailable".into());
            return notes.join(", ");
        }
        let mut sets = Vec::new();
        let mut offset = 0usize;
        while offset + 20 <= length as usize {
            let size = u32::from_le_bytes(buffer[offset..offset + 4].try_into().unwrap()) as usize;
            if size < 20 {
                break;
            }
            let kind = u32::from_le_bytes(buffer[offset + 4..offset + 8].try_into().unwrap());
            if kind == 0 {
                let id = u32::from_le_bytes(buffer[offset + 8..offset + 12].try_into().unwrap());
                sets.push((id, buffer[offset + 18]));
            }
            offset += size;
        }
        let fastest = sets.iter().map(|&(_, class)| class).max().unwrap_or(0);
        let slowest = sets.iter().map(|&(_, class)| class).min().unwrap_or(0);
        if fastest == slowest {
            notes.push(format!(
                "{} logical cpus, all one class: no pinning",
                sets.len()
            ));
        } else {
            let ids: Vec<u32> = sets
                .iter()
                .filter(|&&(_, class)| class == fastest)
                .map(|&(id, _)| id)
                .collect();
            let ok = SetThreadSelectedCpuSets(thread, ids.as_ptr(), ids.len() as u32) != 0;
            notes.push(format!(
                "pinned to {} of {} logical cpus (efficiency class {}) {}",
                ids.len(),
                sets.len(),
                fastest,
                if ok { "ok" } else { "FAILED" }
            ));
        }
    }
    notes.join(", ")
}

#[cfg(not(windows))]
pub fn keep_thread_on_performance_cores() -> String {
    "not on Windows: no pinning".into()
}
