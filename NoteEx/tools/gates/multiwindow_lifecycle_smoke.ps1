param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class NoteExWindowProbe
{
    public sealed class WindowInfo
    {
        public IntPtr Handle { get; set; }
        public int ProcessId { get; set; }
        public string ClassName { get; set; }
        public string Title { get; set; }
		public RECT Frame { get; set; }
    }

	[StructLayout(LayoutKind.Sequential)]
	public struct RECT
	{
		public int Left;
		public int Top;
		public int Right;
		public int Bottom;
	}

	[StructLayout(LayoutKind.Sequential)]
	private struct MONITORINFO
	{
		public int Size;
		public RECT Monitor;
		public RECT Work;
		public int Flags;
	}

    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
	private delegate bool EnumChildProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder value, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder value, int count);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);
	[DllImport("user32.dll")]
	private static extern bool EnumChildWindows(IntPtr parent, EnumChildProc callback, IntPtr parameter);
	[DllImport("user32.dll")]
	private static extern IntPtr SendMessage(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);
	[DllImport("user32.dll")]
	public static extern bool IsWindowVisible(IntPtr window);
	[DllImport("user32.dll")]
	private static extern bool SetForegroundWindow(IntPtr window);
	[DllImport("user32.dll")]
	private static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
	[DllImport("user32.dll")]
	private static extern bool GetWindowRect(IntPtr window, out RECT value);
	[DllImport("user32.dll")]
	private static extern bool GetClientRect(IntPtr window, out RECT value);
	[DllImport("user32.dll")]
	private static extern uint GetDpiForWindow(IntPtr window);
	[DllImport("user32.dll")]
	private static extern IntPtr GetWindowDpiAwarenessContext(IntPtr window);
	[DllImport("user32.dll")]
	private static extern bool AreDpiAwarenessContextsEqual(IntPtr first, IntPtr second);
	[DllImport("user32.dll")]
	private static extern IntPtr GetMenu(IntPtr window);
	[DllImport("user32.dll")]
	private static extern IntPtr GetSubMenu(IntPtr menu, int position);
	[DllImport("user32.dll")]
	private static extern uint GetMenuItemID(IntPtr menu, int position);
	[DllImport("user32.dll")]
	private static extern int GetDlgCtrlID(IntPtr window);
	[DllImport("user32.dll")]
	private static extern int GetMenuItemCount(IntPtr menu);
	[DllImport("user32.dll")]
	private static extern uint GetMenuState(IntPtr menu, uint command, uint flags);
	[DllImport("user32.dll")]
	private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
	[DllImport("user32.dll")]
	private static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
	[DllImport("user32.dll")]
	private static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO information);

    public static WindowInfo[] MainWindows(int processId)
    {
        var result = new List<WindowInfo>();
        EnumWindows((window, parameter) =>
        {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != processId) return true;
            var className = new StringBuilder(256);
            GetClassName(window, className, className.Capacity);
            if (!String.Equals(className.ToString(), "NoteExMainWindow", StringComparison.Ordinal)) return true;
            var title = new StringBuilder(1024);
            GetWindowText(window, title, title.Capacity);
			RECT frame;
			GetWindowRect(window, out frame);
            result.Add(new WindowInfo
            {
                Handle = window,
                ProcessId = processId,
                ClassName = className.ToString(),
				Title = title.ToString(),
				Frame = frame
            });
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }

	public static bool IsPerMonitorV2(IntPtr window)
	{
		return AreDpiAwarenessContextsEqual(
			GetWindowDpiAwarenessContext(window), new IntPtr(-4));
	}

	public static uint FirstViewCommand(IntPtr window)
	{
		var menu = GetMenu(window);
		var view = GetSubMenu(menu, 1);
		return view == IntPtr.Zero ? UInt32.MaxValue : GetMenuItemID(view, 0);
	}

	public static int[] PaneHostIds(IntPtr window)
	{
		var result = new List<int>();
		EnumChildWindows(window, (child, parameter) =>
		{
			var className = new StringBuilder(256);
			GetClassName(child, className, className.Capacity);
			if (String.Equals(className.ToString(), "NoteExPaneHost", StringComparison.Ordinal))
				result.Add(GetDlgCtrlID(child));
			return true;
		}, IntPtr.Zero);
		result.Sort();
		return result.ToArray();
	}

	public static IntPtr ChildById(IntPtr window, int id)
	{
		IntPtr result = IntPtr.Zero;
		EnumChildWindows(window, (child, parameter) =>
		{
			if (GetDlgCtrlID(child) == id) { result = child; return false; }
			return true;
		}, IntPtr.Zero);
		return result;
	}

	public static string WindowTextValue(IntPtr window)
	{
		var value = new StringBuilder(32768);
		GetWindowText(window, value, value.Capacity);
		return value.ToString();
	}

	public static IntPtr ProcessWindowByClass(int processId, string expectedClass)
	{
		IntPtr result = IntPtr.Zero;
		EnumWindows((window, parameter) =>
		{
			uint owner;
			GetWindowThreadProcessId(window, out owner);
			if (owner != processId) return true;
			var className = new StringBuilder(256);
			GetClassName(window, className, className.Capacity);
			if (String.Equals(className.ToString(), expectedClass, StringComparison.Ordinal))
			{
				result = window;
				return false;
			}
			return true;
		}, IntPtr.Zero);
		return result;
	}

	public static uint CommandState(IntPtr root, uint command)
	{
		var direct = GetMenuState(root, command, 0);
		if (direct != UInt32.MaxValue) return direct;
		var count = GetMenuItemCount(root);
		for (var index = 0; index < count; ++index)
		{
			var child = GetSubMenu(root, index);
			if (child == IntPtr.Zero) continue;
			var nested = CommandState(child, command);
			if (nested != UInt32.MaxValue) return nested;
		}
		return UInt32.MaxValue;
	}

	public static void SendCharacter(IntPtr window, char value)
	{
		SendMessage(window, 0x0102, new UIntPtr(value), IntPtr.Zero);
	}

	public static void SendListEnter(IntPtr window)
	{
		SendMessage(window, 0x0186, UIntPtr.Zero, IntPtr.Zero);
		SendMessage(window, 0x0100, new UIntPtr(0x0D), IntPtr.Zero);
	}

	public static bool SendChord(IntPtr window, byte modifier, byte key)
	{
		SetForegroundWindow(window);
		if (modifier != 0) keybd_event(modifier, 0, 0, UIntPtr.Zero);
		var posted = PostMessage(window, 0x0100, new UIntPtr(key), IntPtr.Zero);
		System.Threading.Thread.Sleep(150);
		if (modifier != 0) keybd_event(modifier, 0, 2, UIntPtr.Zero);
		return posted;
	}

	public static bool PaneHostsArePermanentAndOrdered(IntPtr window)
	{
		IntPtr left = IntPtr.Zero;
		IntPtr right = IntPtr.Zero;
		EnumChildWindows(window, (child, parameter) =>
		{
			var className = new StringBuilder(256);
			GetClassName(child, className, className.Capacity);
			if (!String.Equals(className.ToString(), "NoteExPaneHost", StringComparison.Ordinal)) return true;
			var id = GetDlgCtrlID(child);
			if (id == 2001) left = child;
			if (id == 2002) right = child;
			return true;
		}, IntPtr.Zero);
		if (left == IntPtr.Zero || right == IntPtr.Zero || left == right) return false;
		RECT leftFrame;
		RECT rightFrame;
		return GetWindowRect(left, out leftFrame) && GetWindowRect(right, out rightFrame) &&
			leftFrame.Right > leftFrame.Left && rightFrame.Right > rightFrame.Left &&
			leftFrame.Left < rightFrame.Left && leftFrame.Right <= rightFrame.Left;
	}

	public static RECT WorkArea(IntPtr window)
	{
		var information = new MONITORINFO();
		information.Size = Marshal.SizeOf(typeof(MONITORINFO));
		var monitor = MonitorFromWindow(window, 2);
		if (monitor == IntPtr.Zero || !GetMonitorInfo(monitor, ref information))
			throw new InvalidOperationException("GetMonitorInfo failed");
		return information.Work;
	}

	public static bool Move(IntPtr window, int x, int y, int width, int height)
	{
		return SetWindowPos(window, IntPtr.Zero, x, y, width, height, 0x0014);
	}

	public static RECT Frame(IntPtr window)
	{
		RECT value;
		if (!GetWindowRect(window, out value)) throw new InvalidOperationException("GetWindowRect failed");
		return value;
	}

	public static bool IsDefaultCentered(IntPtr window)
	{
		RECT client;
		RECT frame;
		if (!GetClientRect(window, out client) || !GetWindowRect(window, out frame)) return false;
		var work = WorkArea(window);
		var dpi = GetDpiForWindow(window);
		var expectedWidth = (960 * (int)dpi + 48) / 96;
		var expectedHeight = (640 * (int)dpi + 48) / 96;
		return Math.Abs((client.Right - client.Left) - expectedWidth) <= 1 &&
			Math.Abs((client.Bottom - client.Top) - expectedHeight) <= 1 &&
			Math.Abs((frame.Left + frame.Right) - (work.Left + work.Right)) <= 2 &&
			Math.Abs((frame.Top + frame.Bottom) - (work.Top + work.Bottom)) <= 2;
	}

	public static bool IntersectsWorkArea(IntPtr window)
	{
		var frame = Frame(window);
		var work = WorkArea(window);
		return Math.Max(frame.Left, work.Left) < Math.Min(frame.Right, work.Right) &&
			Math.Max(frame.Top, work.Top) < Math.Min(frame.Bottom, work.Bottom);
	}
}
'@

function Get-WindowSet([System.Diagnostics.Process]$Process, [int]$ExpectedCount) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "NoteEx exited before reaching $ExpectedCount windows (exit=$($Process.ExitCode))"
        }
        $windows = @([NoteExWindowProbe]::MainWindows($Process.Id))
        if ($windows.Count -eq $ExpectedCount) { return $windows }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $ExpectedCount NoteExMainWindow windows in PID $($Process.Id)"
}

function Wait-ProcessExit([System.Diagnostics.Process]$Process) {
    if (-not $Process.WaitForExit(20000)) {
        throw "Timed out waiting for NoteEx PID $($Process.Id) to exit"
    }
}

function Start-NoteEx([string]$DatabasePath) {
    $argument = '--database="' + $DatabasePath + '"'
    return Start-Process -FilePath $Executable -ArgumentList $argument -PassThru
}

function Get-DatabaseFingerprint([string]$DatabasePath) {
    $result = [ordered]@{}
    foreach ($path in @($DatabasePath, "$DatabasePath-wal", "$DatabasePath-shm")) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $file = Get-Item -LiteralPath $path
            $result[$path] = "$($file.Length):$((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash)"
        } else {
            $result[$path] = '<absent>'
        }
    }
    return ($result | ConvertTo-Json -Compress)
}

function Get-RegistryFingerprint {
    $path = 'HKCU:\Software\pyNote\pyNote'
    if (-not (Test-Path -LiteralPath $path)) { return '<absent>' }
    $properties = Get-ItemProperty -LiteralPath $path
    $result = [ordered]@{}
    foreach ($name in @($properties.PSObject.Properties.Name | Where-Object { $_ -notmatch '^PS' } | Sort-Object)) {
        $result[$name] = [string]$properties.$name
    }
    return ($result | ConvertTo-Json -Compress)
}

function Test-RectNear($Actual, $Expected, [int]$Tolerance = 4) {
    return [Math]::Abs($Actual.Left - $Expected.Left) -le $Tolerance -and
        [Math]::Abs($Actual.Top - $Expected.Top) -le $Tolerance -and
        [Math]::Abs($Actual.Right - $Expected.Right) -le $Tolerance -and
        [Math]::Abs($Actual.Bottom - $Expected.Bottom) -le $Tolerance
}

$predicates = [ordered]@{
    PrimaryStartupOneWindow = $false
    SecondaryExitZero = $false
    SecondaryCreatesSecondPrimaryWindow = $false
    DistinctDocumentTitles = $false
    SecondaryDatabaseNotCreated = $false
    NonLastCloseLeavesOneWindowAndProcess = $false
    LastCloseExits = $false
    RestartAfterNonLastRestoresOne = $false
    FullQuitPreservesTwo = $false
    RestartAfterFullQuitRestoresTwo = $false
    ForcedExitReacquiresIdentity = $false
    D8IniExists = $false
    DefaultDatabaseUntouched = $false
    RegistryUntouched = $false
    NoOwnedProcessLeak = $false
    TemporaryRootRemoved = $false
    EmbeddedManifestPerMonitorV2 = $false
    ActualHwndPerMonitorV2 = $false
    ViewFirstResetGeometry = $false
    ResetCenteredDefaultDip = $false
    PermanentSplitterHosts = $false
    IndependentGeometryRestored = $false
    OffscreenGeometryCorrected = $false
    PerWindowGeometryKeys = $false
}

$ownedProcesses = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temporaryRoot = [IO.Path]::GetFullPath((Join-Path $temporaryParent ("NoteEx-W3-lifecycle-{0}" -f [guid]::NewGuid().ToString('N'))))
$leaf = Split-Path -Leaf $temporaryRoot
if (-not $temporaryRoot.StartsWith($temporaryParent, [StringComparison]::OrdinalIgnoreCase) -or
    -not $leaf.StartsWith('NoteEx-W3-lifecycle-', [StringComparison]::Ordinal)) {
    throw 'Refusing an invalid task temporary root'
}

$savedLocalAppData = $env:LOCALAPPDATA
$database = Join-Path $temporaryRoot 'db\primary.sqlite3'
$secondaryDatabase = Join-Path $temporaryRoot 'db\secondary.sqlite3'
$defaultDatabase = Join-Path $env:APPDATA 'pyNote\pyNote\pynote.sqlite3'
$defaultBefore = Get-DatabaseFingerprint $defaultDatabase
$registryBefore = Get-RegistryFingerprint
$failure = $null

try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    $env:LOCALAPPDATA = Join-Path $temporaryRoot 'local'

    $executableBytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Executable))
    $manifestText = [Text.Encoding]::UTF8.GetString($executableBytes)
    $predicates.EmbeddedManifestPerMonitorV2 =
        $manifestText -match '<dpiAwareness[^>]*>\s*PerMonitorV2\s*</dpiAwareness>' -and
        $manifestText -match '<dpiAware[^>]*>\s*true/pm\s*</dpiAware>'

    $primary = Start-NoteEx $database
    $ownedProcesses.Add($primary)
    $windows = Get-WindowSet $primary 1
    $predicates.PrimaryStartupOneWindow = $true
    $predicates.ActualHwndPerMonitorV2 = [NoteExWindowProbe]::IsPerMonitorV2($windows[0].Handle)
    $predicates.ViewFirstResetGeometry = [NoteExWindowProbe]::FirstViewCommand($windows[0].Handle) -eq 106
    $predicates.PermanentSplitterHosts = [NoteExWindowProbe]::PaneHostsArePermanentAndOrdered($windows[0].Handle)
    if (-not [NoteExWindowProbe]::PostMessage($windows[0].Handle, 0x0111, [UIntPtr]106, [IntPtr]::Zero)) {
        throw 'Failed to post IDM_RESET_GEOMETRY'
    }
    $resetDeadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        if ([NoteExWindowProbe]::IsDefaultCentered($windows[0].Handle)) {
            $predicates.ResetCenteredDefaultDip = $true
            break
        }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $resetDeadline)

    $secondary = Start-NoteEx $secondaryDatabase
    $ownedProcesses.Add($secondary)
    Wait-ProcessExit $secondary
    $predicates.SecondaryExitZero = $secondary.ExitCode -eq 0
    $windows = Get-WindowSet $primary 2
    $predicates.SecondaryCreatesSecondPrimaryWindow =
        (@($windows | Where-Object ProcessId -eq $primary.Id).Count -eq 2)
    $predicates.DistinctDocumentTitles = (@($windows.Title | Sort-Object -Unique).Count -eq 2)
    $predicates.SecondaryDatabaseNotCreated = -not (Test-Path -LiteralPath $secondaryDatabase)

    if (-not [NoteExWindowProbe]::PostMessage($windows[0].Handle, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'Failed to post WM_CLOSE to the non-last window'
    }
    $windows = Get-WindowSet $primary 1
    $predicates.NonLastCloseLeavesOneWindowAndProcess = -not $primary.HasExited
    if (-not [NoteExWindowProbe]::PostMessage($windows[0].Handle, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'Failed to post WM_CLOSE to the last window'
    }
    Wait-ProcessExit $primary
    $predicates.LastCloseExits = $primary.ExitCode -eq 0

    $restartOne = Start-NoteEx $database
    $ownedProcesses.Add($restartOne)
    $windows = Get-WindowSet $restartOne 1
    $predicates.RestartAfterNonLastRestoresOne = $true

    $secondaryTwo = Start-NoteEx $secondaryDatabase
    $ownedProcesses.Add($secondaryTwo)
    Wait-ProcessExit $secondaryTwo
    if ($secondaryTwo.ExitCode -ne 0) { throw 'Second new-window invocation failed' }
    $windows = Get-WindowSet $restartOne 2
    $work = [NoteExWindowProbe]::WorkArea($windows[0].Handle)
    $workWidth = $work.Right - $work.Left
    $workHeight = $work.Bottom - $work.Top
    $frameWidth = [Math]::Min(700, [Math]::Max(400, [int](($workWidth - 120) / 2)))
    $frameHeight = [Math]::Min(520, [Math]::Max(300, $workHeight - 160))
    if (-not [NoteExWindowProbe]::Move($windows[0].Handle,
            $work.Left + 40, $work.Top + 40, $frameWidth, $frameHeight)) {
        throw 'Failed to place the first window for independent geometry'
    }
    if (-not [NoteExWindowProbe]::Move($windows[1].Handle,
            $work.Right - $frameWidth - 40, $work.Top + 80, $frameWidth, $frameHeight)) {
        throw 'Failed to place the second window for independent geometry'
    }
    $windows = Get-WindowSet $restartOne 2
    $savedGeometry = @{}
    foreach ($window in $windows) {
        $savedGeometry[$window.Title] = [NoteExWindowProbe]::Frame($window.Handle)
    }
    if (-not [NoteExWindowProbe]::PostMessage($windows[0].Handle, 0x0111, [UIntPtr]105, [IntPtr]::Zero)) {
        throw 'Failed to post IDM_EXIT for full application quit'
    }
    Wait-ProcessExit $restartOne
    $predicates.FullQuitPreservesTwo = $restartOne.ExitCode -eq 0

    $restartTwo = Start-NoteEx $database
    $ownedProcesses.Add($restartTwo)
    $windows = Get-WindowSet $restartTwo 2
    $predicates.RestartAfterFullQuitRestoresTwo = $true
    $independentGeometry = $true
    foreach ($window in $windows) {
        if (-not $savedGeometry.ContainsKey($window.Title) -or
            -not (Test-RectNear ([NoteExWindowProbe]::Frame($window.Handle)) $savedGeometry[$window.Title])) {
            $independentGeometry = $false
        }
    }
    $predicates.IndependentGeometryRestored = $independentGeometry

    $offscreenFrame = [NoteExWindowProbe]::Frame($windows[0].Handle)
    if (-not [NoteExWindowProbe]::Move($windows[0].Handle, 100000, 100000,
            $offscreenFrame.Right - $offscreenFrame.Left,
            $offscreenFrame.Bottom - $offscreenFrame.Top)) {
        throw 'Failed to place a window fully off-screen'
    }
    if (-not [NoteExWindowProbe]::PostMessage($windows[0].Handle, 0x0111, [UIntPtr]105, [IntPtr]::Zero)) {
        throw 'Failed to quit after placing a window off-screen'
    }
    Wait-ProcessExit $restartTwo

    $corrected = Start-NoteEx $database
    $ownedProcesses.Add($corrected)
    $windows = Get-WindowSet $corrected 2
    $predicates.OffscreenGeometryCorrected =
        @($windows | Where-Object { -not [NoteExWindowProbe]::IntersectsWorkArea($_.Handle) }).Count -eq 0
    Stop-Process -Id $corrected.Id -Force
    Wait-ProcessExit $corrected

    $reacquired = Start-NoteEx $database
    $ownedProcesses.Add($reacquired)
    $windows = Get-WindowSet $reacquired 2
    $predicates.ForcedExitReacquiresIdentity = $true
    if (-not [NoteExWindowProbe]::PostMessage($windows[0].Handle, 0x0111, [UIntPtr]105, [IntPtr]::Zero)) {
        throw 'Failed to request orderly final quit'
    }
    Wait-ProcessExit $reacquired

    $iniPath = Join-Path $env:LOCALAPPDATA 'pyNote\pyNote\NoteEx.ini'
    $predicates.D8IniExists = Test-Path -LiteralPath $iniPath -PathType Leaf
    if ($predicates.D8IniExists) {
        $iniText = [IO.File]::ReadAllText($iniPath, [Text.Encoding]::UTF8)
        $predicates.PerWindowGeometryKeys =
            [regex]::Matches($iniText, '(?m)^windows/[^/]+/geometry=x:[0-9a-f]+$').Count -ge 2
    }
    $predicates.DefaultDatabaseUntouched = (Get-DatabaseFingerprint $defaultDatabase) -ceq $defaultBefore
    $predicates.RegistryUntouched = (Get-RegistryFingerprint) -ceq $registryBefore
} catch {
    $failure = $_
} finally {
    foreach ($process in $ownedProcesses) {
        try {
            $process.Refresh()
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id -Force -ErrorAction Stop
                $process.WaitForExit(10000) | Out-Null
            }
        } catch {
            if ($null -eq $failure) { $failure = $_ }
        }
    }
    $predicates.NoOwnedProcessLeak = @($ownedProcesses | Where-Object {
        try { $_.Refresh(); -not $_.HasExited } catch { $false }
    }).Count -eq 0
    $env:LOCALAPPDATA = $savedLocalAppData
    try {
        if (Test-Path -LiteralPath $temporaryRoot) {
            Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
        }
        $predicates.TemporaryRootRemoved = -not (Test-Path -LiteralPath $temporaryRoot)
    } catch {
        if ($null -eq $failure) { $failure = $_ }
    }
}

foreach ($entry in $predicates.GetEnumerator()) {
    "PREDICATE $($entry.Key)=$([bool]$entry.Value)"
}
if ($null -ne $failure) {
    Write-Error $failure
    exit 1
}
if (@($predicates.Values | Where-Object { -not $_ }).Count -ne 0) {
    Write-Error 'One or more multi-window lifecycle predicates failed'
    exit 1
}
exit 0
