# NoteEx 셸 실기동·정상 종료·INI 영속 프로브 (W0-T0 동결 게이트)
#
# 판별 장치: 실행 전 INI 를 삭제하고 실행 후 재생성을 확인한다. 이 대조가 없으면
# 이전 실행이 남긴 INI 를 이번 실행의 산출로 오인한다.
#
# -IniPath 는 검사할 INI 경로를 명시 고정한다. 기본값(실행 파일과 같은 이름의 .ini)은
# 앱의 실제 기록 위치를 추론한 값이므로, 게이트가 그 추론에 의존하지 않게 하는 인자다.
# 앱이 쓰지 않을 경로를 주면 창·정상 종료 술어를 통과한 뒤 INI 술어에서만 실패하므로,
# INI 분기 자체의 판별력을 시험하는 seeded known-bad 가 된다.
#
# 종료 코드: 0 통과 / 1 술어 실패 / 2 사용법·환경 오류(실행 파일 없음 등)

[CmdletBinding()]
param(
    [string] $Exe = (Join-Path $PSScriptRoot '..\..\x64\ReleaseMD\NoteEx.exe'),
    [string] $IniPath = '',
    [int]    $WindowWaitMs = 10000,
    [int]    $ExitWaitMs   = 8000,
    [int]    $KillWaitMs   = 3000,
    [string] $ExpectedTitle = 'NoteEx'
)

$ErrorActionPreference = 'Stop'

function Write-Stamp([string] $Label) {
    Write-Output ("시각({0}): {1}" -f $Label, (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
}

Write-Output '=== NoteEx 셸 실기동/INI 영속 게이트 ==='
Write-Stamp '시작'

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Output ("[오류] 실행 파일 없음: {0}" -f $Exe)
    exit 2
}
$exePath = (Resolve-Path -LiteralPath $Exe).Path
Write-Output ("대상: {0}" -f $exePath)
Write-Output ("exe SHA-256: {0}" -f (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash)

if ([string]::IsNullOrWhiteSpace($IniPath)) {
    $iniPath = [System.IO.Path]::ChangeExtension($exePath, '.ini')
} else {
    $iniPath = [System.IO.Path]::GetFullPath($IniPath)
    Write-Output ("[준비] INI 경로를 인자로 고정했다: {0}" -f $iniPath)
}
if (Test-Path -LiteralPath $iniPath) {
    Remove-Item -LiteralPath $iniPath -Force
    Write-Output ("[준비] 기존 INI 삭제: {0}" -f $iniPath)
} else {
    Write-Output ("[준비] 기존 INI 없음: {0}" -f $iniPath)
}
if (Test-Path -LiteralPath $iniPath) {
    Write-Output '[오류] INI 삭제 실패 - 판별력 없는 실행이므로 중단한다.'
    exit 2
}
Write-Output '[준비] INI 존재: False'

$proc = $null
$verdict = 1
try {
    Write-Output ("`$ Start-Process {0} -PassThru" -f $exePath)
    $proc = Start-Process -FilePath $exePath -PassThru
    Write-Output ("[관찰] Id={0} HasExited={1}" -f $proc.Id, $proc.HasExited)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($WindowWaitMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $proc.Refresh()
        if ($proc.HasExited) { break }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    $proc.Refresh()
    Write-Output ("[관찰] MainWindowHandle={0} MainWindowTitle='{1}'" -f $proc.MainWindowHandle, $proc.MainWindowTitle)

    if ($proc.HasExited) {
        Write-Output ("[실패] 창 생성 전 종료했다. ExitCode={0}" -f $proc.ExitCode)
        exit 1
    }
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) {
        Write-Output ("[실패] {0}ms 안에 유효 HWND 를 얻지 못했다." -f $WindowWaitMs)
        exit 1
    }
    if ($proc.MainWindowTitle -ne $ExpectedTitle) {
        Write-Output ("[실패] 창 제목 불일치 - 기대 '{0}'." -f $ExpectedTitle)
        exit 1
    }

    Write-Output '$ (Process).CloseMainWindow()'
    $closed = $proc.CloseMainWindow()
    Write-Output ("[관찰] CloseMainWindow={0}" -f $closed)
    if (-not $closed) {
        Write-Output '[실패] CloseMainWindow 가 거부됐다 - 정상 종료 경로를 시험하지 못했다.'
        exit 1
    }
    $exited = $proc.WaitForExit($ExitWaitMs)
    $proc.Refresh()
    Write-Output ("[관찰] WaitForExit({0})={1} HasExited={2}" -f $ExitWaitMs, $exited, $proc.HasExited)
    if (-not $exited) {
        Write-Output '[실패] 정상 종료 대기 시간을 넘겼다.'
        exit 1
    }
    Write-Output ("[관찰] ExitCode={0}" -f $proc.ExitCode)
    if ($proc.ExitCode -ne 0) {
        Write-Output '[실패] 종료 코드가 0 이 아니다.'
        exit 1
    }

    $iniExists = Test-Path -LiteralPath $iniPath
    Write-Output ("[관찰] INI 존재: {0}" -f $iniExists)
    if (-not $iniExists) {
        Write-Output '[실패] INI 가 재생성되지 않았다 - 위치 영속이 깨졌다.'
        exit 1
    }
    Write-Output '--- NoteEx.ini 전문 ---'
    Get-Content -LiteralPath $iniPath | ForEach-Object { Write-Output $_ }
    Write-Output ("INI SHA-256: {0}" -f (Get-FileHash -LiteralPath $iniPath -Algorithm SHA256).Hash)

    $verdict = 0
}
finally {
    if ($null -ne $proc) {
        $proc.Refresh()
        if (-not $proc.HasExited) {
            Write-Output ("[정리] 잔존 프로세스 Id={0} 강제 종료 - 이 실행은 통과가 아니다." -f $proc.Id)
            $proc.Kill()
            $killed = $proc.WaitForExit($KillWaitMs)
            $proc.Refresh()
            Write-Output ("[정리] Kill 후 WaitForExit({0})={1} HasExited={2}" -f $KillWaitMs, $killed, $proc.HasExited)
            if (-not $proc.HasExited) {
                Write-Output '[정리] 강제 종료가 완료되지 않았다 - 다음 실행의 파일 잠금을 의심하라.'
            }
            $verdict = 1
        }
    }
    Write-Stamp '종료'
    Write-Output ("rc={0}" -f $verdict)
}

exit $verdict
