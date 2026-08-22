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

	[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
	private static extern IntPtr SendMessageBuffer(IntPtr window, uint message, UIntPtr wParam, StringBuilder lParam);

	// GetWindowText 는 다른 프로세스의 컨트롤 본문을 읽지 못한다(캡션 없는 컨트롤은 빈 문자열이다).
	// WM_GETTEXT 는 OS 가 프로세스 경계를 마샬링하므로 그쪽으로 읽는다(실측 2026-08-21 D-04 1차 -
	// 편집기 본문이 항상 비어 ListEnter 술어가 거짓이었다).
	public static string WindowTextValue(IntPtr window)
	{
		var length = SendMessage(window, 0x000E, UIntPtr.Zero, IntPtr.Zero).ToInt32();   // WM_GETTEXTLENGTH
		if (length <= 0) return String.Empty;
		var value = new StringBuilder(length + 1);
		SendMessageBuffer(window, 0x000D, new UIntPtr((uint)(length + 1)), value);   // WM_GETTEXT
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

	// 아래는 W3 셸 스파인(G4) 확장 전용 프로브다. 선행 24 술어가 쓰는 멤버는 손대지 않는다.
	[StructLayout(LayoutKind.Sequential)]
	private struct GUITHREADINFO
	{
		public int Size;
		public int Flags;
		public IntPtr Active;
		public IntPtr Focus;
		public IntPtr Capture;
		public IntPtr MenuOwner;
		public IntPtr MoveSize;
		public IntPtr Caret;
		public RECT CaretFrame;
	}

	[DllImport("user32.dll")]
	private static extern bool GetGUIThreadInfo(uint thread, ref GUITHREADINFO information);
	[DllImport("user32.dll")]
	private static extern IntPtr GetWindow(IntPtr window, uint command);

	public static IntPtr Send(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam)
	{
		return SendMessage(window, message, wParam, lParam);
	}

	// 전경 전환은 호출 프로세스에 전경 권한이 있어야 성립한다(SetForegroundWindow 제약). 배경 pwsh 는
	// 권한이 없을 때가 있어 활성화·가속기가 한꺼번에 실패했다(실측 2026-08-21 D-04, 실행마다 흔들림).
	// 현재 전경 스레드에 입력 큐를 붙여 전환하고, 그래도 거부되면 합성 입력(Shift 탭)으로 "마지막 입력
	// 프로세스"가 된 뒤 재시도한다. 반환값은 실제 전경이 그 창인지다 - 호출부는 참일 때만 진행한다.
	public static bool Activate(IntPtr window)
	{
		if (GetForegroundWindow() == window) return true;
		var foreground = GetForegroundWindow();
		uint foregroundProcess;
		var foregroundThread = foreground != IntPtr.Zero ? GetWindowThreadProcessId(foreground, out foregroundProcess) : 0u;
		var self = GetCurrentThreadId();
		var attached = foregroundThread != 0 && foregroundThread != self && AttachThreadInput(self, foregroundThread, true);
		try
		{
			SetForegroundWindow(window);
			if (GetForegroundWindow() != window)
			{
				keybd_event(0x10, 0, 0, UIntPtr.Zero);
				keybd_event(0x10, 0, 2, UIntPtr.Zero);
				SetForegroundWindow(window);
			}
			var deadline = DateTime.UtcNow.AddSeconds(2);
			while (GetForegroundWindow() != window && DateTime.UtcNow < deadline)
			{
				System.Threading.Thread.Sleep(25);
				SetForegroundWindow(window);
			}
			return GetForegroundWindow() == window;
		}
		finally { if (attached) AttachThreadInput(self, foregroundThread, false); }
	}

	public static string ClassNameOf(IntPtr window)
	{
		var className = new StringBuilder(256);
		GetClassName(window, className, className.Capacity);
		return className.ToString();
	}

	public static IntPtr MenuOf(IntPtr window)
	{
		return GetMenu(window);
	}

	// 포커스는 스레드 큐 상태라 다른 프로세스에서는 GetGUIThreadInfo 로만 관측된다.
	public static IntPtr FocusedWindow(IntPtr window)
	{
		uint processId;
		var thread = GetWindowThreadProcessId(window, out processId);
		var information = new GUITHREADINFO();
		information.Size = Marshal.SizeOf(typeof(GUITHREADINFO));
		return GetGUIThreadInfo(thread, ref information) ? information.Focus : IntPtr.Zero;
	}

	public static IntPtr ChildByClass(IntPtr window, string expectedClass)
	{
		IntPtr result = IntPtr.Zero;
		EnumChildWindows(window, (child, parameter) =>
		{
			var className = new StringBuilder(256);
			GetClassName(child, className, className.Capacity);
			if (!String.Equals(className.ToString(), expectedClass, StringComparison.Ordinal)) return true;
			result = child;
			return false;
		}, IntPtr.Zero);
		return result;
	}

	// 창마다 자기 모달리스 셸을 하나씩 소유하므로 클래스만으로는 어느 창 것인지 가려지지 않는다.
	public static IntPtr OwnedWindowByClass(int processId, IntPtr owner, string expectedClass)
	{
		IntPtr result = IntPtr.Zero;
		EnumWindows((window, parameter) =>
		{
			uint current;
			GetWindowThreadProcessId(window, out current);
			if (current != processId) return true;
			var className = new StringBuilder(256);
			GetClassName(window, className, className.Capacity);
			if (!String.Equals(className.ToString(), expectedClass, StringComparison.Ordinal)) return true;
			if (GetWindow(window, 4) != owner) return true;
			result = window;
			return false;
		}, IntPtr.Zero);
		return result;
	}

	// SendChord 와 같은 규약이되 수식 키가 여러 개이고 게시 대상 창을 따로 지정한다.
	// Ctrl+Enter 는 편집기 HWND 로 가야 페이지 pre-translation 이 받고, 가속기는 활성 창
	// 큐의 GetKeyState 로 수식 키를 읽으므로 활성화가 먼저다.
	[DllImport("user32.dll")]
	private static extern IntPtr GetForegroundWindow();
	[DllImport("user32.dll")]
	private static extern bool AttachThreadInput(uint attach, uint attachTo, bool attaching);
	[DllImport("kernel32.dll")]
	private static extern uint GetCurrentThreadId();

	// 수식 키와 본 키를 전부 실입력(keybd_event)으로 보낸다. 종전엔 수식 키만 입력 큐, 본 키는
	// PostMessage 였는데 GetMessage 는 포스트 메시지를 입력보다 먼저 꺼내므로 대상 스레드가 바쁘면
	// 본 키가 Ctrl 보다 먼저 처리돼 가속기가 간헐 실패했다(실측 2026-08-21 D-04: Ctrl+S 미저장 →
	// 더티 종료 프롬프트 → 종료 대기 타임아웃). 활성화는 Activate(전경 권한 획득·검증)로 하고,
	// 전경이 실제로 바뀐 뒤에만 입력을 보낸다.
	// target 은 실입력이 포커스 창으로 가는 성질상 참고값이다 - 포커스가 그 창 트리 안에 있는지는
	// 호출부 술어(FocusedWindow)가 본다.
	public static bool SendChordEx(IntPtr activate, IntPtr target, byte[] modifiers, byte key)
	{
		if (!Activate(activate)) return false;
		foreach (var modifier in modifiers) keybd_event(modifier, 0, 0, UIntPtr.Zero);
		keybd_event(key, 0, 0, UIntPtr.Zero);
		keybd_event(key, 0, 2, UIntPtr.Zero);
		for (var index = modifiers.Length - 1; index >= 0; --index)
		{
			keybd_event(modifiers[index], 0, 2, UIntPtr.Zero);
		}
		System.Threading.Thread.Sleep(150);
		return true;
	}

	[DllImport("kernel32.dll", SetLastError = true)]
	private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
	[DllImport("kernel32.dll", SetLastError = true)]
	private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, UIntPtr size, uint allocationType, uint protect);
	[DllImport("kernel32.dll", SetLastError = true)]
	private static extern bool VirtualFreeEx(IntPtr process, IntPtr address, UIntPtr size, uint freeType);
	[DllImport("kernel32.dll", SetLastError = true)]
	private static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, UIntPtr size, out UIntPtr read);
	[DllImport("kernel32.dll", SetLastError = true)]
	private static extern bool CloseHandle(IntPtr handle);

	// 상태 바는 창 텍스트가 아니라 파트 텍스트를 갖는다. SB_GETTEXTLENGTHW(0x040C) 가
	// 하위 워드에 길이를 주고 SB_GETTEXTW(0x040D) 가 그 버퍼를 채운다.
	// SB_GETTEXTW 는 WM_GETTEXT 와 달리 OS 가 프로세스 경계를 마샬링하지 않는다 - 이쪽 버퍼
	// 주소를 그대로 보내면 상태 바(comctl32)가 대상 프로세스 안에서 남의 주소에 쓰다 앱을
	// 죽인다(실측 2026-08-21: 0xC000041D, D-04 1차). 대상 프로세스 안에 버퍼를 잡아 주고 읽어 온다.
	public static string StatusPartText(IntPtr status, int part)
	{
		var length = SendMessage(status, 0x040C, new UIntPtr((uint)part), IntPtr.Zero).ToInt32() & 0xFFFF;
		if (length <= 0) return String.Empty;
		uint processId;
		GetWindowThreadProcessId(status, out processId);
		var process = OpenProcess(0x0008 | 0x0010 | 0x0020, false, processId); // VM_OPERATION | VM_READ | VM_WRITE
		if (process == IntPtr.Zero) return String.Empty;
		try
		{
			var bytes = new UIntPtr((uint)((length + 1) * 2));
			var remote = VirtualAllocEx(process, IntPtr.Zero, bytes, 0x1000 | 0x2000, 0x04); // MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
			if (remote == IntPtr.Zero) return String.Empty;
			try
			{
				SendMessage(status, 0x040D, new UIntPtr((uint)part), remote);
				var buffer = new byte[(length + 1) * 2];
				UIntPtr read;
				if (!ReadProcessMemory(process, remote, buffer, bytes, out read)) return String.Empty;
				var text = Encoding.Unicode.GetString(buffer, 0, (int)read.ToUInt32());
				var terminator = text.IndexOf('\0');
				return terminator >= 0 ? text.Substring(0, terminator) : text;
			}
			finally { VirtualFreeEx(process, remote, UIntPtr.Zero, 0x8000); } // MEM_RELEASE
		}
		finally { CloseHandle(process); }
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

# 아래 보조 함수는 W3 셸 스파인(G4) 확장 전용이다. 선행 24 술어의 경로는 건드리지 않는다.

# UI 전이 대기는 기존 폴링(50ms 간격 + 마감 시각)과 같은 모양을 쓴다.
function Wait-Condition([scriptblock]$Condition, [int]$TimeoutSeconds = 10) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Send-ProbeMessage([IntPtr]$Window, [int]$Message, [int]$WParam = 0, [int]$LParam = 0) {
    return [NoteExWindowProbe]::Send($Window, [uint32]$Message, [UIntPtr]$WParam, [IntPtr]$LParam).ToInt32()
}

# RichEdit 은 단락 구분을 CR/CRLF 로 돌려준다. 제품 CDocumentPage 의 utf8() 과 같은 규칙으로
# 접어서 창 표시 값을 비교한다 - 저장 본문의 LF 계약은 DB 바이트 술어가 따로 강제한다.
function ConvertTo-LfText([string]$Value) {
    return ($Value -replace "`r`n", "`n" -replace "`r", "`n")
}

# 살아 있는 제품이 DB 를 쓰기로 열고 있어 Get-FileHash/Copy-Item 의 FileShare.Read 로는
# 공유 위반이 난다. FileShare.ReadWrite 로 스냅샷만 뜨고 해시·바이트 검색은 사본에서 한다.
# -shm 은 행 데이터를 담지 않으므로 대상이 아니다.
function Copy-LiveDatabase([string]$DatabasePath, [string]$ScanRoot) {
    $stamp = [guid]::NewGuid().ToString('N')
    $copies = New-Object System.Collections.Generic.List[object]
    foreach ($path in @($DatabasePath, "$DatabasePath-wal")) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $name = Split-Path -Leaf $path
        $copy = Join-Path $ScanRoot "$name-$stamp"
        $source = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $target = [IO.File]::Create($copy)
            try { $source.CopyTo($target) } finally { $target.Dispose() }
        } finally { $source.Dispose() }
        $copies.Add([PSCustomObject]@{ Name = $name; Path = $copy })
    }
    return $copies.ToArray()
}

function Get-LiveDatabaseFingerprint($Copies) {
    $result = [ordered]@{}
    foreach ($copy in $Copies) {
        $file = Get-Item -LiteralPath $copy.Path
        $result[$copy.Name] = "$($file.Length):$((Get-FileHash -LiteralPath $copy.Path -Algorithm SHA256).Hash)"
    }
    return ($result | ConvertTo-Json -Compress)
}

# sqlite3 CLI 도 System.Data.SQLite 도 이 호스트에 보장되지 않는다. SQLite 는 TEXT 를 페이지
# 이미지에 무압축으로 싣고 레코드 값은 열 순서대로 붙으므로, cards 의 source||body 인접
# 바이트열을 사본에서 찾으면 카드 행의 출처와 본문을 한 번에 확인할 수 있다(draft_text 같은
# 다른 열은 이 접두사를 갖지 않는다). 1 바이트 = 1 문자인 ISO-8859-1 로 디코드해 Ordinal
# 검색하므로 바이트 비교와 동치다.
function Test-CopiesContainUtf8($Copies, [string]$Needle) {
    $latin = [Text.Encoding]::GetEncoding(28591)
    $pattern = $latin.GetString([Text.Encoding]::UTF8.GetBytes($Needle))
    foreach ($copy in $Copies) {
        $image = $latin.GetString([IO.File]::ReadAllBytes($copy.Path))
        if ($image.IndexOf($pattern, [StringComparison]::Ordinal) -ge 0) { return $true }
    }
    return $false
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
    ActualPageChildWindows = $false
    FirstPasteCreatesConnectedCard = $false
    ListEnterOpensStoredCard = $false
    CtrlEnterLfSavesExactDbBody = $false
    FindReplaceVisibilityAndFocus = $false
    HistoryToCardListModeAndFocus = $false
    RuntimeMenuCommandsAndAcceleratorDispatch = $false
    ModelessSearchQueryFocus = $false
    FocusModeHidesAndRestoresShell = $false
    RestartRestoresPageUiState = $false
    StatusBarReflectsPasteAndSave = $false
    WindowTitleShowsDocumentTitle = $false
    StartupMaintenanceProducesBackup = $false
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
$savedClipboardText = $null
$clipboardCaptured = $false
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
    # 창이 열거된 직후는 첫 레이아웃 전이라 pane 폭이 0 일 수 있다 - 정착까지 기다려 관측한다.
    $predicates.PermanentSplitterHosts = Wait-Condition { [NoteExWindowProbe]::PaneHostsArePermanentAndOrdered($windows[0].Handle) }
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
    # 복원 위치는 기동 직후 몇 프레임 뒤에 정착한다 - 두 창 전부 저장 위치 근방에 놓일 때까지 기다린다.
    $predicates.IndependentGeometryRestored = Wait-Condition {
        $script:windows = @([NoteExWindowProbe]::MainWindows($restartTwo.Id))
        $ok = @($script:windows).Count -eq 2
        foreach ($window in $script:windows) {
            if (-not $savedGeometry.ContainsKey($window.Title) -or
                -not (Test-RectNear ([NoteExWindowProbe]::Frame($window.Handle)) $savedGeometry[$window.Title])) { $ok = $false }
        }
        return $ok
    }
    $windows = @([NoteExWindowProbe]::MainWindows($restartTwo.Id))

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

    # --- W3 셸 스파인(G4 확장) ---
    # 선행 24 술어의 순서와 의미를 흔들지 않도록 마지막 재획득 종료 뒤에 덧붙인다. G4 10번
    # 항목(기본 DB/레지스트리/프로세스/임시 루트·재획득 불변식)은 이 구간 뒤에서 그대로
    # 평가되는 선행 술어가 소유한다 - 셸 구간이 만든 프로세스와 쓰기가 모두 그 판정에 든다.
    $scanRoot = Join-Path $temporaryRoot 'scan'
    New-Item -ItemType Directory -Path $scanRoot | Out-Null

    $shell = Start-NoteEx $database
    $ownedProcesses.Add($shell)
    $windows = Get-WindowSet $shell 2
    $shellWindow = $windows[0].Handle
    $shellTitle = $windows[0].Title

    # W3 창 제목 계약: "{문서 제목} — pyNote" 축자다(CAP-FI-015). 문서 제목 부분이
    # 비면 접미사만 남으므로 길이로 그 자리를 함께 굳힌다.
    $titleSuffix = ' — pyNote'
    $predicates.WindowTitleShowsDocumentTitle =
        @($windows).Count -eq 2 -and
        (@($windows | Where-Object {
            $_.Title.EndsWith($titleSuffix, [StringComparison]::Ordinal) -and
            $_.Title.Length -gt $titleSuffix.Length
        }).Count -eq 2)

    # 기동 유지보수가 격리 루트 안 backups/ 에 자동 백업 산출물을 남긴다. 백업 디렉터리
    # 규칙은 backup/location 이 비면 DB 부모/backups 다(원본 app.py:446~449). 기본 사용자
    # 자산 불변은 같은 자리에서 함께 본다 - 이 술어가 사용자 DB/레지스트리를 건드리는
    # 백업 경로를 통과시키면 안 된다.
    $backupDirectory = Join-Path (Split-Path -Parent $database) 'backups'
    $backupAppeared = Wait-Condition {
        (Test-Path -LiteralPath $backupDirectory -PathType Container) -and
        @(Get-ChildItem -LiteralPath $backupDirectory -File -Filter '*.auto-*.sqlite3').Count -ge 1
    }
    $predicates.StartupMaintenanceProducesBackup =
        $backupAppeared -and
        ((Get-DatabaseFingerprint $defaultDatabase) -ceq $defaultBefore) -and
        ((Get-RegistryFingerprint) -ceq $registryBefore)

    # G4-1: 실제 상태바/목록/편집기/이력 자식 HWND. 자리표시 페인트 호스트가 아님을 클래스로 굳힌다.
    $status = [NoteExWindowProbe]::ChildById($shellWindow, 2106)       # IDC_MAIN_STATUS
    $cardList = [NoteExWindowProbe]::ChildById($shellWindow, 2101)     # IDC_DOCUMENT_CARD_LIST
    $editor = [NoteExWindowProbe]::ChildById($shellWindow, 2102)       # IDC_DOCUMENT_EDITOR
    $findInput = [NoteExWindowProbe]::ChildById($shellWindow, 2103)    # IDC_DOCUMENT_FIND
    $replaceInput = [NoteExWindowProbe]::ChildById($shellWindow, 2104) # IDC_DOCUMENT_REPLACE
    $historyList = [NoteExWindowProbe]::ChildById($shellWindow, 2105)  # IDC_DOCUMENT_HISTORY
    # 자식 HWND 생성·가시화가 창 열거보다 늦을 수 있다 - 정착까지 기다려 관측한다.
    $predicates.ActualPageChildWindows = Wait-Condition {
        $status = [NoteExWindowProbe]::ChildById($shellWindow, 2106); $cardList = [NoteExWindowProbe]::ChildById($shellWindow, 2101)
        $editor = [NoteExWindowProbe]::ChildById($shellWindow, 2102); $findInput = [NoteExWindowProbe]::ChildById($shellWindow, 2103)
        $replaceInput = [NoteExWindowProbe]::ChildById($shellWindow, 2104); $historyList = [NoteExWindowProbe]::ChildById($shellWindow, 2105)
        return (
        $status -ne [IntPtr]::Zero -and $cardList -ne [IntPtr]::Zero -and
        $editor -ne [IntPtr]::Zero -and $historyList -ne [IntPtr]::Zero -and
        $findInput -ne [IntPtr]::Zero -and $replaceInput -ne [IntPtr]::Zero -and
        [NoteExWindowProbe]::ClassNameOf($status) -eq 'msctls_statusbar32' -and
        [NoteExWindowProbe]::ClassNameOf($cardList) -eq 'NoteExCardList' -and
        [NoteExWindowProbe]::ClassNameOf($historyList) -eq 'ListBox' -and
        [NoteExWindowProbe]::ClassNameOf($editor) -eq 'RICHEDIT50W' -and
        [NoteExWindowProbe]::IsWindowVisible($status) -and
        [NoteExWindowProbe]::IsWindowVisible($cardList) -and
        [NoteExWindowProbe]::IsWindowVisible($editor) -and
        -not [NoteExWindowProbe]::IsWindowVisible($historyList)) }
    $status = [NoteExWindowProbe]::ChildById($shellWindow, 2106); $cardList = [NoteExWindowProbe]::ChildById($shellWindow, 2101)
    $editor = [NoteExWindowProbe]::ChildById($shellWindow, 2102); $findInput = [NoteExWindowProbe]::ChildById($shellWindow, 2103)
    $replaceInput = [NoteExWindowProbe]::ChildById($shellWindow, 2104); $historyList = [NoteExWindowProbe]::ChildById($shellWindow, 2105)

    # G4-2: 첫 붙여넣기가 DB 카드 하나를 만들고 paste 출처와 편집기 연결을 남긴다.
    # 포커스는 GUI 스레드 큐에 하나뿐이고 두 창이 같은 스레드에 산다. 이후 관측이 이 창을
    # 가리키도록 먼저 활성화한다(가속기 라우팅도 활성 창 기준이다).
    [void][NoteExWindowProbe]::Activate($shellWindow)
    $pasteBody = 'SMOKEPASTEALPHA'
    try { $savedClipboardText = Get-Clipboard -Raw -ErrorAction Stop } catch { $savedClipboardText = $null }
    $clipboardCaptured = $true
    Set-Clipboard -Value $pasteBody
    $listCountBeforePaste = Send-ProbeMessage $cardList 0x018B          # LB_GETCOUNT
    $beforePaste = Get-LiveDatabaseFingerprint (Copy-LiveDatabase $database $scanRoot)
    [void][NoteExWindowProbe]::Send($editor, 0x0302, [UIntPtr]::Zero, [IntPtr]::Zero)   # WM_PASTE
    $pasteLanded = Wait-Condition { (Send-ProbeMessage $cardList 0x018B) -eq 1 }
    $afterPaste = Copy-LiveDatabase $database $scanRoot
    $predicates.FirstPasteCreatesConnectedCard =
        $listCountBeforePaste -eq 0 -and $pasteLanded -and
        (Send-ProbeMessage $cardList 0x0188) -eq 0 -and                 # LB_GETCURSEL
        [NoteExWindowProbe]::FocusedWindow($shellWindow) -eq $editor -and
        (Get-LiveDatabaseFingerprint $afterPaste) -cne $beforePaste -and
        (Test-CopiesContainUtf8 $afterPaste ('paste' + $pasteBody))

    # G4-3: 정리된 이탈 뒤 목록 Enter 가 저장된 카드와 그 현재 리비전 본문을 연다.
    if (-not [NoteExWindowProbe]::PostMessage($shellWindow, 0x0111, [UIntPtr]123, [IntPtr]::Zero)) {
        throw 'Failed to post IDM_BACK for the clean leave'
    }
    $leftEditor = Wait-Condition { [NoteExWindowProbe]::FocusedWindow($shellWindow) -eq $cardList }
    $clearedText = ConvertTo-LfText ([NoteExWindowProbe]::WindowTextValue($editor))
    [NoteExWindowProbe]::SendListEnter($cardList)
    $reopened = Wait-Condition { [NoteExWindowProbe]::FocusedWindow($shellWindow) -eq $editor }
    $predicates.ListEnterOpensStoredCard =
        $leftEditor -and $clearedText -ceq '' -and $reopened -and
        (ConvertTo-LfText ([NoteExWindowProbe]::WindowTextValue($editor))) -ceq $pasteBody -and
        (Send-ProbeMessage $cardList 0x0188) -eq 0

    # G4-3: Ctrl+Enter 는 LF 하나만 넣고 Ctrl+S 는 그 본문 그대로 DB 에 남긴다.
    # 저장 본문 계약은 LF 다 - 편집기가 CR 을 남기면 이 술어가 소리내어 깨진다.
    $savedBody = $pasteBody + "`n" + 'SMOKEBETA'
    if (-not [NoteExWindowProbe]::SendChordEx($shellWindow, $editor, [byte[]]@(0x11), [byte]0x0D)) {
        throw 'Failed to post Ctrl+Enter to the actual editor'
    }
    foreach ($character in 'SMOKEBETA'.ToCharArray()) {
        [NoteExWindowProbe]::SendCharacter($editor, $character)
    }
    if (-not [NoteExWindowProbe]::SendChordEx($shellWindow, $shellWindow, [byte[]]@(0x11), [byte]0x53)) {
        throw 'Failed to post Ctrl+S through the runtime accelerator table'
    }
    $bodyPersisted = Wait-Condition -TimeoutSeconds 5 -Condition {
        Test-CopiesContainUtf8 (Copy-LiveDatabase $database $scanRoot) ('paste' + $savedBody)
    }
    $predicates.CtrlEnterLfSavesExactDbBody =
        $bodyPersisted -and
        (Get-LiveDatabaseFingerprint (Copy-LiveDatabase $database $scanRoot)) -cne
            (Get-LiveDatabaseFingerprint $afterPaste)

    # 붙여넣기·저장 뒤 상태 바가 seam 조립기 문안 축자다. 문자 수는 코드포인트 계수라
    # 여기 본문(ASCII)에서는 .NET 길이와 같다 - 계수 단위 자체는 G3' 시험이 한국어로 굳힌다.
    $expectedStatus = "1개 카드 · $($savedBody.Length)자 · 모든 변경 저장됨 · 로컬 DB"
    $predicates.StatusBarReflectsPasteAndSave = Wait-Condition -TimeoutSeconds 5 -Condition {
        [NoteExWindowProbe]::StatusPartText($status, 0) -ceq $expectedStatus
    }

    # G4-4: Ctrl+F 는 찾기만, Ctrl+H 는 바꾸기까지 보이고 각각 그 입력에 포커스를 준다.
    $findHiddenBefore = -not [NoteExWindowProbe]::IsWindowVisible($findInput) -and
        -not [NoteExWindowProbe]::IsWindowVisible($replaceInput)
    if (-not [NoteExWindowProbe]::SendChordEx($shellWindow, $shellWindow, [byte[]]@(0x11), [byte]0x46)) {
        throw 'Failed to post Ctrl+F through the runtime accelerator table'
    }
    $findFocused = Wait-Condition { [NoteExWindowProbe]::FocusedWindow($shellWindow) -eq $findInput }
    $findOnly = [NoteExWindowProbe]::IsWindowVisible($findInput) -and
        -not [NoteExWindowProbe]::IsWindowVisible($replaceInput)
    if (-not [NoteExWindowProbe]::SendChordEx($shellWindow, $shellWindow, [byte[]]@(0x11), [byte]0x48)) {
        throw 'Failed to post Ctrl+H through the runtime accelerator table'
    }
    $replaceFocused = Wait-Condition { [NoteExWindowProbe]::FocusedWindow($shellWindow) -eq $replaceInput }
    $predicates.FindReplaceVisibilityAndFocus =
        $findHiddenBefore -and $findFocused -and $findOnly -and $replaceFocused -and
        [NoteExWindowProbe]::IsWindowVisible($findInput) -and
        [NoteExWindowProbe]::IsWindowVisible($replaceInput)

    # G4-6: 실제 런타임 HMENU 의 명령 ID 와 HACCEL 발송. 미구현 스텁은 회색으로만 노출된다.
    $runtimeMenu = [NoteExWindowProbe]::MenuOf($shellWindow)
    $menuWired = $runtimeMenu -ne [IntPtr]::Zero
    foreach ($command in @(104, 105, 106, 110, 112, 113, 118, 119, 120, 121, 122, 124)) {
        $state = [NoteExWindowProbe]::CommandState($runtimeMenu, [uint32]$command)
        if ($state -eq [uint32]::MaxValue -or ($state -band 0x0003) -ne 0) { $menuWired = $false }
    }
    # 131 = IDM_FIRST_RUN_GUIDE (구 128 - IDR_MAINFRAME 충돌로 재조정, Resource.h)
    foreach ($command in @(111, 114, 115, 116, 117, 125, 126, 127, 131)) {
        $state = [NoteExWindowProbe]::CommandState($runtimeMenu, [uint32]$command)
        if ($state -eq [uint32]::MaxValue -or ($state -band 0x0001) -eq 0) { $menuWired = $false }
    }
    if (-not [NoteExWindowProbe]::SendChordEx($shellWindow, $shellWindow, [byte[]]@(0x11, 0x10), [byte]0x48)) {
        throw 'Failed to post Ctrl+Shift+H through the runtime accelerator table'
    }
    $historyDispatched = Wait-Condition { [NoteExWindowProbe]::FocusedWindow($shellWindow) -eq $historyList }
    $predicates.RuntimeMenuCommandsAndAcceleratorDispatch =
        $menuWired -and $historyDispatched -and
        [NoteExWindowProbe]::IsWindowVisible($historyList) -and
        -not [NoteExWindowProbe]::IsWindowVisible($cardList)

    # G4-5: 이력에서 카드 목록 모드로 돌아오면 목록이 다시 보이고 포커스를 가져간다.
    if (-not [NoteExWindowProbe]::SendChordEx($shellWindow, $shellWindow, [byte[]]@(0x11, 0x10), [byte]0x50)) {
        throw 'Failed to post Ctrl+Shift+P through the runtime accelerator table'
    }
    $listFocused = Wait-Condition { [NoteExWindowProbe]::FocusedWindow($shellWindow) -eq $cardList }
    $predicates.HistoryToCardListModeAndFocus =
        $listFocused -and [NoteExWindowProbe]::IsWindowVisible($cardList) -and
        -not [NoteExWindowProbe]::IsWindowVisible($historyList)

    # G4-7: Ctrl+P 가 모달리스 검색 셸을 소유·표시하고 실제 유니코드 질의 EDIT 에 포커스를 준다.
    $searchAbsentBefore =
        [NoteExWindowProbe]::ProcessWindowByClass($shell.Id, 'NoteExSearchDialog') -eq [IntPtr]::Zero
    if (-not [NoteExWindowProbe]::SendChordEx($shellWindow, $shellWindow, [byte[]]@(0x11), [byte]0x50)) {
        throw 'Failed to post Ctrl+P through the runtime accelerator table'
    }
    $searchShown = Wait-Condition {
        $candidate = [NoteExWindowProbe]::ProcessWindowByClass($shell.Id, 'NoteExSearchDialog')
        return ($candidate -ne [IntPtr]::Zero -and [NoteExWindowProbe]::IsWindowVisible($candidate))
    }
    $searchWindow = [NoteExWindowProbe]::ProcessWindowByClass($shell.Id, 'NoteExSearchDialog')
    $searchQuery = if ($searchWindow -eq [IntPtr]::Zero) { [IntPtr]::Zero }
        else { [NoteExWindowProbe]::ChildById($searchWindow, 2107) }   # IDC_SEARCH_QUERY
    $predicates.ModelessSearchQueryFocus =
        $searchAbsentBefore -and $searchShown -and $searchQuery -ne [IntPtr]::Zero -and
        [NoteExWindowProbe]::ClassNameOf($searchQuery) -eq 'Edit' -and
        [NoteExWindowProbe]::FocusedWindow($searchWindow) -eq $searchQuery

    # G4-8: F11 이 메뉴/상태 HWND/문서 목록 셸을 함께 감추고 체크 상태를 뒤집었다가 되돌린다.
    # 집중 모드에서는 메뉴가 창에서 떼어지므로 체크 상태는 붙잡아 둔 HMENU 로 읽는다.
    $documentShell =
        [NoteExWindowProbe]::OwnedWindowByClass($shell.Id, $shellWindow, 'NoteExDocumentListShell')
    $focusModeReady = $documentShell -ne [IntPtr]::Zero -and
        [NoteExWindowProbe]::IsWindowVisible($documentShell) -and
        [NoteExWindowProbe]::IsWindowVisible($status) -and
        ([NoteExWindowProbe]::CommandState($runtimeMenu, [uint32]124) -band 0x0008) -eq 0
    if (-not [NoteExWindowProbe]::SendChordEx($shellWindow, $shellWindow, [byte[]]@(), [byte]0x7A)) {
        throw 'Failed to post F11 through the runtime accelerator table'
    }
    $focusModeEntered = Wait-Condition { [NoteExWindowProbe]::MenuOf($shellWindow) -eq [IntPtr]::Zero }
    $focusModeHides = $focusModeEntered -and
        -not [NoteExWindowProbe]::IsWindowVisible($status) -and
        -not [NoteExWindowProbe]::IsWindowVisible($documentShell) -and
        ([NoteExWindowProbe]::CommandState($runtimeMenu, [uint32]124) -band 0x0008) -ne 0
    if (-not [NoteExWindowProbe]::SendChordEx($shellWindow, $shellWindow, [byte[]]@(), [byte]0x7A)) {
        throw 'Failed to post the second F11 through the runtime accelerator table'
    }
    $focusModeLeft = Wait-Condition { [NoteExWindowProbe]::MenuOf($shellWindow) -eq $runtimeMenu }
    $predicates.FocusModeHidesAndRestoresShell =
        $focusModeReady -and $focusModeHides -and $focusModeLeft -and
        [NoteExWindowProbe]::IsWindowVisible($status) -and
        [NoteExWindowProbe]::IsWindowVisible($documentShell) -and
        ([NoteExWindowProbe]::CommandState($runtimeMenu, [uint32]124) -band 0x0008) -eq 0

    # G4-9: 재시작이 구조화된 페이지 UI 상태를 되살린다. 분할 크기는 사용자 드래그로만 바뀌므로
    # 스플리터 바에 실제 마우스 메시지를 넣어 기본값(가용 폭의 1/3)에서 떼어 놓고 비교한다.
    $splitter = [NoteExWindowProbe]::ChildByClass($shellWindow, 'NoteExWindowSplitter')
    $leftPane = [NoteExWindowProbe]::ChildById($shellWindow, 2001)
    $rightPane = [NoteExWindowProbe]::ChildById($shellWindow, 2002)
    $splitterFrame = [NoteExWindowProbe]::Frame($splitter)
    $leftFrame = [NoteExWindowProbe]::Frame($leftPane)
    $rightFrame = [NoteExWindowProbe]::Frame($rightPane)
    $leftWidthBeforeDrag = $leftFrame.Right - $leftFrame.Left
    $barX = [int](($leftFrame.Right + $rightFrame.Left) / 2) - $splitterFrame.Left
    $barY = [int](($leftFrame.Top + $leftFrame.Bottom) / 2) - $splitterFrame.Top
    $targetX = [int](($splitterFrame.Right - $splitterFrame.Left) / 2)
    [void][NoteExWindowProbe]::Activate($shellWindow)
    [void](Send-ProbeMessage $splitter 0x0201 1 (($barY -shl 16) -bor ($barX -band 0xFFFF)))    # WM_LBUTTONDOWN
    [void](Send-ProbeMessage $splitter 0x0200 1 (($barY -shl 16) -bor ($targetX -band 0xFFFF))) # WM_MOUSEMOVE
    [void](Send-ProbeMessage $splitter 0x0202 0 (($barY -shl 16) -bor ($targetX -band 0xFFFF))) # WM_LBUTTONUP
    $draggedFrame = [NoteExWindowProbe]::Frame($leftPane)
    $draggedWidth = $draggedFrame.Right - $draggedFrame.Left
    [void](Send-ProbeMessage $editor 0x00B1 5 5)                        # EM_SETSEL
    $selectionBefore = Send-ProbeMessage $editor 0x00B0                 # EM_GETSEL
    $listCountBefore = Send-ProbeMessage $cardList 0x018B
    $listSelectionBefore = Send-ProbeMessage $cardList 0x0188
    if (-not [NoteExWindowProbe]::PostMessage($shellWindow, 0x0111, [UIntPtr]105, [IntPtr]::Zero)) {
        throw 'Failed to post IDM_EXIT after the shell spine phase'
    }
    Wait-ProcessExit $shell

    $restarted = Start-NoteEx $database
    $ownedProcesses.Add($restarted)
    $windows = Get-WindowSet $restarted 2
    # 창 열거 직후 제목은 생성 중 기본값일 수 있다(OnCreate 말미 update_title_) - 제목이 정착할 때까지 기다린다.
    [void](Wait-Condition { $script:windows = @([NoteExWindowProbe]::MainWindows($restarted.Id)); @($script:windows | Where-Object { $_.Title -ceq $shellTitle }).Count -eq 1 })
    $windows = @([NoteExWindowProbe]::MainWindows($restarted.Id))
    $restoredMatches = @($windows | Where-Object { $_.Title -ceq $shellTitle })
    if ($restoredMatches.Count -eq 1) {
        $restoredWindow = $restoredMatches[0].Handle
        $restoredEditor = [NoteExWindowProbe]::ChildById($restoredWindow, 2102)
        $restoredList = [NoteExWindowProbe]::ChildById($restoredWindow, 2101)
        # 복원 직후에는 레이아웃 전이라 pane 폭이 0 으로 읽힐 수 있다 - 정착까지 기다려 관측한다.
        $predicates.RestartRestoresPageUiState = Wait-Condition {
            $restoredLeftFrame = [NoteExWindowProbe]::Frame(
                [NoteExWindowProbe]::ChildById($restoredWindow, 2001))
            $script:restoredLeftWidth = $restoredLeftFrame.Right - $restoredLeftFrame.Left
            return (
                $draggedWidth -ge ($leftWidthBeforeDrag + 16) -and
                $selectionBefore -eq ((5 -shl 16) -bor 5) -and
                (ConvertTo-LfText ([NoteExWindowProbe]::WindowTextValue($restoredEditor))) -ceq $savedBody -and
                (Send-ProbeMessage $restoredEditor 0x00B0) -eq $selectionBefore -and
                (Send-ProbeMessage $restoredList 0x018B) -eq $listCountBefore -and
                (Send-ProbeMessage $restoredList 0x0188) -eq $listSelectionBefore -and
                [Math]::Abs($script:restoredLeftWidth - $draggedWidth) -le 4) }
        $restoredLeftWidth = $script:restoredLeftWidth
    }
    if (-not [NoteExWindowProbe]::PostMessage($windows[0].Handle, 0x0111, [UIntPtr]105, [IntPtr]::Zero)) {
        throw 'Failed to post IDM_EXIT after the shell spine restart'
    }
    Wait-ProcessExit $restarted

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
    # 붙여넣기 출처 검증에 실제 클립보드가 필요해 텍스트만 저장·복원한다. 원래 내용이
    # 텍스트가 아니었으면 되돌릴 수 없으므로 표식만 지우고 만다.
    if ($clipboardCaptured) {
        try {
            if ([string]::IsNullOrEmpty($savedClipboardText)) { Set-Clipboard -Value ' ' }
            else { Set-Clipboard -Value $savedClipboardText }
        } catch {
            if ($null -eq $failure) { $failure = $_ }
        }
    }
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
