#!/usr/bin/env python3
"""winmin.py [--hide] <exe> [args...]: run a program with its window minimized and not activated, for test runs.

The window opens minimized on the taskbar and the window in front keeps the focus (deslib.sh's des_window_launch,
DES_WINDOW). With --hide it never shows. Its output goes where ours goes, and we exit with its exit status.

Two ways in, as a program shows its first window either way:
  - STARTUPINFO's show flag (SW_SHOWMINNOACTIVE, or SW_HIDE), which Windows puts in place of a plain first show;
  - a window event hook on the program's job (the program and what it starts, as test_rsx_synth starts its
    children): a window that shows anyway (a test run's window shows with SW_SHOWNOACTIVATE, PS3_NO_USER_INPUT,
    which Windows leaves as it is, and a child's window ignores our STARTUPINFO) is minimized or hidden as it
    shows, once.

The program runs in a job object that ends it when this process ends, so `timeout <s> winmin.py <exe>` still stops
the program (and what it starts) when the time is up.
"""
import ctypes
import subprocess
import sys
import threading
from ctypes import wintypes

SW_HIDE, SW_SHOWMINNOACTIVE = 0, 7
EVENT_OBJECT_SHOW, OBJID_WINDOW, WINEVENT_OUTOFCONTEXT, GA_ROOT, WM_QUIT = 0x8002, 0, 0, 2, 0x0012
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
JobObjectExtendedLimitInformation = 9


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                                                 "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION), ("IoInfo", IO_COUNTERS),
                ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]


def kill_with_us(proc):
    """Put proc in a job that ends with this process (its handle closes when we end, however we end)."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateJobObjectW.restype = wintypes.HANDLE
    job = k32.CreateJobObjectW(None, None)
    if not job:
        return None
    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    k32.SetInformationJobObject(wintypes.HANDLE(job), JobObjectExtendedLimitInformation, ctypes.byref(info),
                                ctypes.sizeof(info))
    k32.AssignProcessToJobObject(wintypes.HANDLE(job), wintypes.HANDLE(int(proc._handle)))
    return job   # (kept open for our lifetime)


def in_job(job, pids):
    """in_job(job, pids)(pid): whether the process is in the job (the answer kept per pid in pids)."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE

    def check(pid):
        if pid not in pids:
            h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
            yes = wintypes.BOOL(False)
            if h:
                k32.IsProcessInJob(wintypes.HANDLE(h), wintypes.HANDLE(job), ctypes.byref(yes))
                k32.CloseHandle(wintypes.HANDLE(h))
            pids[pid] = bool(yes.value)
        return pids[pid]
    return check


def watch_windows(pid, job, show, ready, out):
    """On this thread: hook the windows of the program's job (only the program's, without a job) as they show,
    and give each top-level one `show` once."""
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    proc_t = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.DWORD, wintypes.HWND, wintypes.LONG, wintypes.LONG,
                                wintypes.DWORD, wintypes.DWORD)
    u32.SetWinEventHook.restype = wintypes.HANDLE
    u32.GetAncestor.restype = wintypes.HWND
    u32.GetWindowThreadProcessId.restype = wintypes.DWORD
    done = set()
    ours = in_job(job, {pid: True}) if job else (lambda p: p == pid)

    def on_show(_hook, _event, hwnd, obj, child, _thread, _time):
        if obj != OBJID_WINDOW or child != 0 or not hwnd or hwnd in done:
            return
        if u32.GetAncestor(hwnd, GA_ROOT) != hwnd:
            return
        owner = wintypes.DWORD(0)
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if not ours(owner.value):
            return
        done.add(hwnd)
        u32.ShowWindow(hwnd, show)

    cb = proc_t(on_show)
    h = u32.SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, None, cb, 0 if job else pid, 0,
                             WINEVENT_OUTOFCONTEXT)
    out.append(ctypes.windll.kernel32.GetCurrentThreadId())
    ready.set()
    msg = wintypes.MSG()
    while u32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
        u32.TranslateMessage(ctypes.byref(msg))
        u32.DispatchMessageW(ctypes.byref(msg))
    if h:
        u32.UnhookWinEvent(wintypes.HANDLE(h))


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if len(sys.argv) >= 2 else 2
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    args = sys.argv[1:]
    hide = args[0] == "--hide"
    if hide:
        args = args[1:]
    if not args:
        print(__doc__.strip())
        return 2
    si.wShowWindow = SW_HIDE if hide else SW_SHOWMINNOACTIVE
    try:
        proc = subprocess.Popen(args, startupinfo=si)
    except OSError as e:
        print(f"winmin.py: cannot start {args[0]}: {e}", file=sys.stderr)
        return 127
    job = kill_with_us(proc)
    ready, tid = threading.Event(), []
    threading.Thread(target=watch_windows, args=(proc.pid, job, si.wShowWindow, ready, tid), daemon=True).start()
    ready.wait(5)
    try:
        return proc.wait()
    except KeyboardInterrupt:
        proc.kill()
        return 130
    finally:
        if tid:
            ctypes.windll.user32.PostThreadMessageW(tid[0], WM_QUIT, 0, 0)
        del job


if __name__ == "__main__":
    sys.exit(main())
