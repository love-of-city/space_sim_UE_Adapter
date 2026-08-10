param(
    [string]$UnrealRoot = '',
    [string]$ListenAddress = '127.0.0.1',
    [int]$Port = 5558,
    [int]$Width = 1280,
    [int]$Height = 720,
    [string]$ReplayPath = '',
    [double]$ReplayRate = 1.0,
    [string]$ScreenshotPath = '',
    [switch]$Foreground
)

. (Join-Path $PSScriptRoot 'common.ps1')
$ue = Resolve-UnrealRoot $UnrealRoot
$editor = Join-Path $ue 'Engine\Binaries\Win64\UnrealEditor.exe'
$arguments = @(
    $ProjectFile, '-game', '-windowed', "-ResX=$Width", "-ResY=$Height",
    "-BskListen=$ListenAddress", "-BskPort=$Port", '-log'
)
if ($ReplayPath) {
    $resolvedReplay = [IO.Path]::GetFullPath($ReplayPath)
    $arguments += @("-BskReplay=$resolvedReplay", "-BskReplayRate=$ReplayRate")
}
if ($ScreenshotPath) {
    $arguments += "-BskScreenshot=$([IO.Path]::GetFullPath($ScreenshotPath))"
}
if ($Foreground) {
    & $editor @arguments
    exit $LASTEXITCODE
}
$saved = Join-Path $ProjectRoot 'Saved'
New-Item -ItemType Directory -Path $saved -Force | Out-Null
$process = Start-Process -FilePath $editor -ArgumentList $arguments -PassThru
Set-Content -LiteralPath (Join-Path $saved 'BskRenderer.pid') -Value $process.Id -Encoding ascii
if ($ReplayPath) {
    Write-Output "BSK Unreal Renderer started (PID $($process.Id)), replaying $resolvedReplay at ${ReplayRate}x"
} else {
    Write-Output "BSK Unreal Renderer started (PID $($process.Id)), listening on $ListenAddress`:$Port"
}
