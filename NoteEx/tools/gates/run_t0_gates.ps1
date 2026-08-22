# W0-T0 동결 게이트 일괄 실행기 — 종료코드를 로그에 결속한다
#
# 왜 필요한가: 게이트 로그가 표준 출력만 담으면 대장 표의 `rc` 열은 로그 해시로
# 인증되지 않는 별개 주장이 된다. 래퍼가 성공처럼 보이는 출력을 낸 뒤 0 이 아닌
# 코드로 끝나도 해시는 그대로 맞는다. 그래서 각 게이트 로그 마지막 줄에 실제
# 프로세스 종료코드를 적고, 명령 원문·작업 디렉터리·시작·종료 시각·종료코드·로그
# SHA-256 을 하나의 트랜스크립트에 모은다(W0지시서 §3.1 증거 묶음 요구).
#
# 이 실행기는 기대 종료코드도 함께 검사한다. seeded known-bad 가 조용히 통과하면
# 실행기 자신이 실패한다 — 계측기의 양방향성을 실행기 수준에서 강제하는 장치다.
#
# 종료 코드: 0 전건 기대와 일치 / 1 하나 이상 불일치 / 2 사용법·환경 오류

[CmdletBinding()]
param(
    [string] $RepoRoot = '',
    [string] $LogDir = '',
    [string] $Prefix = 'T0G',
    [string] $MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe',
    [string] $DumpBin = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\dumpbin.exe',
    [string] $Python = '',
    # 14번 스모크는 전경 창을 잡아 실입력을 넣으므로 기본 목록에서 뺀다(대화형 데스크톱 전용).
    [switch] $WithSmoke,
    [int] $SmokeIdleSeconds = 0,
    [int] $SmokeIdleWaitMinutes = 10
)

$ErrorActionPreference = 'Stop'

# D-09 경로 파라미터화: 미지정 시 스크립트 위치 기준 자동탐지 — 게이트 semantics 불변, 경로만.
$repoRoot  = if ($RepoRoot) { $RepoRoot } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
$noteExDir = Join-Path $repoRoot 'NoteEx'
$python    = if ($Python) { $Python } else { Join-Path $repoRoot '.venv\Scripts\python.exe' }
if (-not $LogDir) { $LogDir = Join-Path $repoRoot 'scratchpad\orchestration\wtl-port\logs' }
$gatesDir  = Join-Path $noteExDir 'tools\gates'
$smoke     = Join-Path $gatesDir 'shell_smoke.ps1'
$multiSmoke = Join-Path $gatesDir 'multiwindow_lifecycle_smoke.ps1'
$outDir    = Join-Path $noteExDir 'x64\ReleaseMD'

$required = @($MSBuild, $DumpBin, $python, $smoke)
if ($WithSmoke) { $required += $multiSmoke }
foreach ($p in $required) {
    if (-not (Test-Path -LiteralPath $p)) {
        Write-Output ("[오류] 필수 실행 파일·스크립트 없음: {0}" -f $p)
        exit 2
    }
}
if (-not (Test-Path -LiteralPath $LogDir)) {
    Write-Output ("[오류] 로그 디렉터리 없음: {0}" -f $LogDir)
    exit 2
}

# 셸 프로브(09~11)는 W3 계약으로 돈다(2026-08-21, 사용자 A 확정): 실행마다 임시 루트 아래 격리 DB 와
# 임시 LOCALAPPDATA 를 만들어 앱이 사용자 실데이터(%APPDATA% 기본 DB·%LOCALAPPDATA% INI)를 열지 않게
# 하고, 제목은 빈 DB 첫 문서 `Note 1 — pyNote`, INI 는 격리 루트 안 경로를 명시한다. 루트는 끝에 지운다.
$shellRoot  = Join-Path ([System.IO.Path]::GetTempPath()) ('NoteEx-T0-shell-' + [guid]::NewGuid().ToString('N'))
$shellLocal = Join-Path $shellRoot 'local'
$shellDb    = Join-Path $shellRoot 'db\primary.sqlite3'
$shellIni   = Join-Path $shellLocal 'pyNote\pyNote\NoteEx.ini'
$shellTitle = 'Note 1 — pyNote'
New-Item -ItemType Directory -Path $shellLocal -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $shellDb) -Force | Out-Null

# 14번은 전경 창을 잡아 실입력을 넣는다 — 사용자가 키보드를 만지는 중이면 전경 경합으로 술어가
# 무더기 실패한다. 유휴 대기와 아래 3줄 기록은 그 경합과 제품 결함을 사후에 가르는 장치다.
if ($WithSmoke) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class T0GateInputProbe
{
    [StructLayout(LayoutKind.Sequential)]
    private struct LASTINPUTINFO
    {
        public uint cbSize;
        public uint dwTime;
    }

    [DllImport("user32.dll")]
    private static extern bool GetLastInputInfo(ref LASTINPUTINFO plii);

    [DllImport("kernel32.dll")]
    private static extern uint GetTickCount();

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(IntPtr hWnd, StringBuilder text, int count);

    public static double IdleSeconds()
    {
        LASTINPUTINFO info = new LASTINPUTINFO();
        info.cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO));
        if (!GetLastInputInfo(ref info)) { return -1.0; }
        return (GetTickCount() - info.dwTime) / 1000.0;
    }

    public static IntPtr Foreground() { return GetForegroundWindow(); }

    public static string ForegroundTitle()
    {
        IntPtr hWnd = GetForegroundWindow();
        if (hWnd == IntPtr.Zero) { return string.Empty; }
        StringBuilder text = new StringBuilder(512);
        int length = GetWindowTextW(hWnd, text, text.Capacity);
        return length > 0 ? text.ToString() : string.Empty;
    }
}
'@
}

# id / 이름 / 작업 디렉터리 / 실행 파일 / 인자 / 기대 종료코드
$gates = @(
    @{ id = '01'; name = 'ReleaseMD|x64 클린 리빌드';                cwd = $noteExDir; exe = $MSBuild; args = @('NoteEx.sln', '/t:Rebuild', '/p:Configuration=ReleaseMD;Platform=x64'); expect = 0 }
    @{ id = '02'; name = 'Catch2 시험 전건';                          cwd = $noteExDir; exe = (Join-Path $outDir 'NoteExTests.exe'); args = @(); expect = 0 }
    @{ id = '03'; name = 'core 헤더 격리 자기시험';                   cwd = $repoRoot;  exe = $python; args = @((Join-Path $gatesDir 'check_core_isolation.py'), '--self-test'); expect = 0 }
    @{ id = '04'; name = 'core 헤더 격리 실트리';                     cwd = $repoRoot;  exe = $python; args = @((Join-Path $gatesDir 'check_core_isolation.py'), '--roots', (Join-Path $noteExDir 'core')); expect = 0 }
    @{ id = '05'; name = 'core 격리 seeded known-bad(실픽스처)';      cwd = $repoRoot;  exe = $python; args = @((Join-Path $gatesDir 'check_core_isolation.py'), '--roots', (Join-Path $gatesDir 'fixtures\bad')); expect = 1 }
    @{ id = '06'; name = '스키마 동등성 자기시험';                    cwd = $repoRoot;  exe = $python; args = @((Join-Path $gatesDir 'check_schema_parity.py'), '--self-test'); expect = 0 }
    @{ id = '07'; name = '스키마 동등성 실대조(공개 소비 smoke)';     cwd = $repoRoot;  exe = $python; args = @((Join-Path $gatesDir 'check_schema_parity.py')); expect = 0 }
    @{ id = '08'; name = '시험 추적성';                               cwd = $repoRoot;  exe = $python; args = @((Join-Path $gatesDir 'check_test_traceability.py')); expect = 0 }
    @{ id = '09'; name = '셸 실기동/INI known-good(격리 DB)';         cwd = $noteExDir; exe = 'pwsh'; args = @('-NoProfile', '-File', $smoke, '-Database', $shellDb, '-LocalAppData', $shellLocal, '-IniPath', $shellIni, '-ExpectedTitle', $shellTitle); expect = 0 }
    @{ id = '10'; name = '셸 프로브 known-bad: 창 없음';              cwd = $noteExDir; exe = 'pwsh'; args = @('-NoProfile', '-File', $smoke, '-Exe', (Join-Path $outDir 'NoteExTests.exe'), '-WindowWaitMs', '3000'); expect = 1 }
    @{ id = '11'; name = '셸 프로브 known-bad: INI 미기록';           cwd = $noteExDir; exe = 'pwsh'; args = @('-NoProfile', '-File', $smoke, '-Database', $shellDb, '-LocalAppData', $shellLocal, '-ExpectedTitle', $shellTitle, '-IniPath', (Join-Path $outDir 'NoteEx.seeded-absent.ini')); expect = 1 }
    @{ id = '12'; name = '셸 프로브 known-bad: 실행 파일 부재';       cwd = $noteExDir; exe = 'pwsh'; args = @('-NoProfile', '-File', $smoke, '-Exe', (Join-Path $outDir 'NoSuchBinary.exe')); expect = 2 }
    @{ id = '13'; name = '배포 의존성 dumpbin /dependents';           cwd = $noteExDir; exe = $DumpBin; args = @('/dependents', (Join-Path $outDir 'NoteEx.exe')); expect = 0 }
)
if ($WithSmoke) {
    $gates += @{ id = '14'; name = '다중 창 수명주기 스모크 37 술어(대화형 전용)'; cwd = $noteExDir; exe = 'pwsh'; args = @('-NoProfile', '-File', $multiSmoke, '-Executable', (Join-Path $outDir 'NoteEx.exe')); expect = 0 }
}

$transcript = Join-Path $LogDir ("{0}_transcript.md" -f $Prefix)
$rows = New-Object System.Collections.Generic.List[string]
$rows.Add('# W0-T0 게이트 실행 트랜스크립트')
$rows.Add('')
$rows.Add(('실행기: `{0}`' -f $PSCommandPath))
$rows.Add(('머신: {0} / pwsh {1}' -f $env:COMPUTERNAME, $PSVersionTable.PSVersion))
$rows.Add(('시작(로컬): {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')))
$rows.Add('')
$rows.Add('| # | 게이트 | 기대 rc | 실제 rc | 판정 | 시작 | 종료 | 로그 (SHA-256) |')
$rows.Add('|---|---|---|---|---|---|---|---|')

$detail = New-Object System.Collections.Generic.List[string]
$failed = 0

foreach ($g in $gates) {
    $log = Join-Path $LogDir ("{0}_{1}.log" -f $Prefix, $g.id)
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }

    $gateNotes = New-Object System.Collections.Generic.List[string]
    if ($g.id -eq '14') {
        $deadline = (Get-Date).AddMinutes($SmokeIdleWaitMinutes)
        $bExceeded = $false
        while ($SmokeIdleSeconds -gt 0 -and [T0GateInputProbe]::IdleSeconds() -lt $SmokeIdleSeconds) {
            if ((Get-Date) -ge $deadline) { $bExceeded = $true; break }
            Start-Sleep -Seconds 5
        }
        $dIdle = [T0GateInputProbe]::IdleSeconds()
        $sSession = ((query session 2>&1) | Where-Object { $_ -match '^\s*>' } |
            ForEach-Object { $_.ToString().Trim() }) -join ' / '
        if (-not $sSession) { $sSession = '(query session 활성 세션 줄 없음)' }
        $hForeground = [T0GateInputProbe]::Foreground()
        $sForeground = [T0GateInputProbe]::ForegroundTitle()
        $gateNotes.Add(('- 실행 직전 유휴 초: {0:0.0} (요구 {1}, 대기 상한 {2}분)' -f $dIdle, $SmokeIdleSeconds, $SmokeIdleWaitMinutes))
        $gateNotes.Add(('- query session 활성 세션: `{0}`' -f $sSession))
        $gateNotes.Add(('- 전경 창: hwnd=0x{0:X} 제목 `{1}`' -f [int64]$hForeground, $sForeground))
        if ($bExceeded) {
            $gateNotes.Add('- 유휴 대기 상한 초과 — 요구 유휴에 이르지 못한 채 그대로 실행했다(스킵 없음).')
            Write-Output ('14 유휴 대기 상한 초과: 요구 {0}초 / 실측 {1:0.0}초' -f $SmokeIdleSeconds, $dIdle)
        }
    }

    $start = Get-Date
    Push-Location -LiteralPath $g.cwd
    try {
        if ($g.args.Count -gt 0) {
            & $g.exe @($g.args) *> $log
        } else {
            & $g.exe *> $log
        }
        $rc = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    $end = Get-Date

    Add-Content -LiteralPath $log -Value ("[runner] exit_code={0}" -f $rc)
    $hash = (Get-FileHash -LiteralPath $log -Algorithm SHA256).Hash.ToLower()
    $ok = ($rc -eq $g.expect)
    if (-not $ok) { $failed++ }

    # 표 셀 안의 파이프는 이스케이프한다 — `ReleaseMD|x64` 처럼 이름에 들어 있으면 열이 밀린다.
    $cellName = $g.name -replace '\|', '\|'
    $rows.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | `{7}` (`{8}`) |' -f `
        $g.id, $cellName, $g.expect, $rc, ($(if ($ok) { '일치' } else { '**불일치**' })),
        $start.ToString('HH:mm:ss'), $end.ToString('HH:mm:ss'),
        (Split-Path -Leaf $log), $hash))

    $detail.Add(('### {0} {1}' -f $g.id, $g.name))
    $detail.Add('')
    $detail.Add(('- 작업 디렉터리: `{0}`' -f $g.cwd))
    $detail.Add(('- 명령: `{0}`' -f ((@($g.exe) + $g.args) -join ' ')))
    $detail.Add(('- 기대 rc {0} / 실제 rc {1} / {2}' -f $g.expect, $rc, ($(if ($ok) { '일치' } else { '불일치' }))))
    $detail.Add(('- 로그: `{0}` SHA-256 `{1}`' -f $log, $hash))
    foreach ($note in $gateNotes) { $detail.Add($note) }
    $detail.Add('')

    Write-Output ('{0} {1}: 기대 {2} / 실제 {3} / {4}' -f $g.id, $g.name, $g.expect, $rc, ($(if ($ok) { 'OK' } else { 'MISMATCH' })))
}

$rows.Add('')
$rows.Add(('종료(로컬): {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')))
$rows.Add(('불일치 게이트: {0}건' -f $failed))
$rows.Add('')
$rows.Add('## 게이트별 상세')
$rows.Add('')

if (Test-Path -LiteralPath $shellRoot) { Remove-Item -LiteralPath $shellRoot -Recurse -Force -ErrorAction SilentlyContinue }
$rows.Add(('셸 프로브 임시 루트 정리: {0} (잔존={1})' -f $shellRoot, (Test-Path -LiteralPath $shellRoot)))
$rows.Add('')

Set-Content -LiteralPath $transcript -Value ($rows + $detail) -Encoding utf8NoBOM
Write-Output ('트랜스크립트: {0}' -f $transcript)
Write-Output ('불일치 {0}건' -f $failed)

if ($failed -gt 0) { exit 1 }
exit 0
