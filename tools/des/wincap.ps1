# wincap.ps1 -ProcessId <pid> -Out <png> [-Every <s>]: save the client area of that des_boot.exe's window with
# PrintWindow (the window's own content, also when it is covered or in the background; never the screen) and
# print one line per capture:
#   mean <brightness 0-255> size <w>x<h> title <window title>
# or "no window" when the process has no visible titled window yet. With -Every, one capture every <s> seconds,
# saved as <Out without .png>-HHmmss.png, until the process exits. Read-only: it never focuses, moves or resizes
# the window. perflib.sh's perf_wincap / perf_black use it for knobab.sh's window check.
param([Parameter(Mandatory = $true)][int]$ProcessId, [Parameter(Mandatory = $true)][string]$Out, [int]$Every = 0)
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies ([System.Drawing.Bitmap].Assembly.Location) @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class WinCap {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

    // Window sizes in real pixels, whatever the display scaling.
    static WinCap() { SetProcessDPIAware(); }

    // The game's visible, titled top-level window; not a console window (Windows reports a console
    // window as belonging to the program attached to it).
    public static IntPtr Find(uint want) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != want || !IsWindowVisible(h) || GetWindowTextLength(h) == 0) return true;
            var cls = new System.Text.StringBuilder(256);
            GetClassName(h, cls, cls.Capacity);
            string c = cls.ToString();
            if (c == "ConsoleWindowClass" || c == "PseudoConsoleWindow" || c == "CASCADIA_HOSTING_WINDOW_CLASS") return true;
            found = h; return false;
        }, IntPtr.Zero);
        return found;
    }

    public static string Title(IntPtr h) {
        var sb = new System.Text.StringBuilder(512);
        GetWindowText(h, sb, sb.Capacity);
        return sb.ToString();
    }

    // Saves the client area to path; returns its mean brightness (0-255), or -1 when it has no area.
    public static double Capture(IntPtr h, string path, out int cw, out int ch) {
        RECT wr, cr; POINT o = new POINT();
        cw = ch = 0;
        if (!GetWindowRect(h, out wr) || !GetClientRect(h, out cr) || !ClientToScreen(h, ref o)) return -1;
        int w = wr.R - wr.L, wh = wr.B - wr.T;
        if (w <= 0 || wh <= 0) return -1;
        using (var full = new Bitmap(w, wh, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(full)) {
                IntPtr dc = g.GetHdc();
                PrintWindow(h, dc, 2);   // PW_RENDERFULLCONTENT: includes the swap chain's content
                g.ReleaseHdc(dc);
            }
            int x = Math.Max(0, o.X - wr.L), y = Math.Max(0, o.Y - wr.T);
            cw = Math.Min(cr.R - cr.L, w - x); ch = Math.Min(cr.B - cr.T, wh - y);
            if (cw <= 0 || ch <= 0) return -1;
            using (var c = full.Clone(new Rectangle(x, y, cw, ch), PixelFormat.Format32bppArgb)) {
                c.Save(path, ImageFormat.Png);
                var d = c.LockBits(new Rectangle(0, 0, cw, ch), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                int stride = d.Stride;
                var px = new byte[stride * ch];
                Marshal.Copy(d.Scan0, px, 0, px.Length);
                c.UnlockBits(d);
                double sum = 0;
                for (int yy = 0; yy < ch; yy++) {
                    int row = yy * stride;
                    for (int xx = 0; xx < cw; xx++) {
                        int p = row + xx * 4;   // B, G, R, A
                        sum += 0.114 * px[p] + 0.587 * px[p + 1] + 0.299 * px[p + 2];
                    }
                }
                return sum / ((double)cw * ch);
            }
        }
    }
}
"@
$inv = [Globalization.CultureInfo]::InvariantCulture
$p = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
if (-not $p -or $p.ProcessName -ne 'des_boot') { "no game with pid $ProcessId"; exit 2 }
$dir = Split-Path -Parent $Out
if ($dir) { New-Item -ItemType Directory -Force $dir | Out-Null }
function Shot([string]$path) {
    $h = [WinCap]::Find([uint32]$ProcessId)
    if ($h -eq [IntPtr]::Zero) { return 'no window' }
    $cw = 0; $ch = 0
    $m = [WinCap]::Capture($h, $path, [ref]$cw, [ref]$ch)
    if ($m -lt 0) { return 'no window area' }
    return 'mean ' + $m.ToString('F1', $inv) + ' size ' + $cw + 'x' + $ch + ' title ' + [WinCap]::Title($h)
}
if ($Every -le 0) {
    $r = Shot $Out
    $r
    if ($r -like 'mean *') { exit 0 } else { exit 1 }
}
$base = [IO.Path]::Combine([IO.Path]::GetDirectoryName($Out), [IO.Path]::GetFileNameWithoutExtension($Out))
while (-not $p.HasExited) {
    $f = $base + '-' + (Get-Date).ToString('HHmmss', $inv) + '.png'
    (Get-Date).ToString('HH:mm:ss', $inv) + ' ' + (Shot $f)
    if ($p.WaitForExit($Every * 1000)) { break }
}
