param(
    [string]$UnrealRoot = '',
    [string]$ListenAddress = '127.0.0.1',
    [int]$Port = 5558,
    [int]$Width = 1280,
    [int]$Height = 720,
    [string]$ReplayPath = '',
    [double]$ReplayRate = 1.0,
    [string]$ScreenshotPath = '',
    [string]$CaptureDirectory = '',
    [string[]]$CaptureProducts = @(),
    [ValidateRange(0.0, 60.0)]
    [double]$CaptureRate = 0.0,
    [ValidateRange(0.0, 60.0)]
    [double]$PreviewRate = 0.0,
    [string]$PixelStreamingURL = '',
    [string]$PixelStreamingId = 'BskRenderer',
    [ValidateRange(1, 120)]
    [int]$PixelStreamingFps = 60,
    [string]$CaptureNetworkHost = '127.0.0.1',
    [ValidateRange(0, 65535)]
    [int]$CaptureNetworkPort = 0,
    [string]$AutoCommand = '',
    [switch]$Foreground
)

. (Join-Path $PSScriptRoot 'common.ps1')
$ue = Resolve-UnrealRoot $UnrealRoot
$editor = Join-Path $ue 'Engine\Binaries\Win64\UnrealEditor.exe'
$arguments = @(
    $ProjectFile, '-game', '-windowed', "-ResX=$Width", "-ResY=$Height",
    "-BskListen=$ListenAddress", "-BskPort=$Port", '-log'
)
$normalizedCaptureProducts = @()
foreach ($item in $CaptureProducts) {
    foreach ($product in ($item -split ',')) {
        if ($product.Trim()) { $normalizedCaptureProducts += $product.Trim().ToLowerInvariant() }
    }
}
$unsupportedCaptureProducts = @($normalizedCaptureProducts | Where-Object { $_ -notin @('rgb', 'depth', 'segmentation') })
if ($unsupportedCaptureProducts.Count -gt 0) {
    throw "Unsupported capture products: $($unsupportedCaptureProducts -join ', ')"
}
if ($ReplayPath) {
    $resolvedReplay = [IO.Path]::GetFullPath($ReplayPath)
    $arguments += @("-BskReplay=$resolvedReplay", "-BskReplayRate=$ReplayRate")
}
if ($ScreenshotPath) {
    $arguments += "-BskScreenshot=$([IO.Path]::GetFullPath($ScreenshotPath))"
}
if ($CaptureDirectory) {
    $arguments += "-BskCaptureDir=$([IO.Path]::GetFullPath($CaptureDirectory))"
}
if ($normalizedCaptureProducts.Count -gt 0) {
    # Unreal's FParse::Value treats commas as token delimiters. Use '+' on the
    # command line; the runtime normalizes it back into a product list.
    $arguments += "-BskCaptureProducts=$($normalizedCaptureProducts -join '+')"
}
if ($CaptureRate -gt 0.0) {
    $arguments += "-BskCaptureRate=$CaptureRate"
}
if ($PreviewRate -gt 0.0) {
    $arguments += "-BskPreviewRate=$PreviewRate"
}
if ($PixelStreamingURL) {
    $arguments += @(
        # Pixel Streaming must keep rendering even when no local window is focused/minimized.
        # ForceRes keeps the requested back-buffer size in off-screen mode.
        '-RenderOffscreen',
        '-ForceRes',
        "-PixelStreamingConnectionURL=$PixelStreamingURL",
        "-PixelStreamingID=$PixelStreamingId",
        "-PixelStreamingWebRTCFps=$PixelStreamingFps",
        '-PixelStreamingEncoderCodec=H264',
        '-PixelStreamingEncoderLatencyMode=UltraLowLatency',
        '-PixelStreamingWebRTCDisableTransmitAudio=true',
        '-PixelStreamingWebRTCDisableReceiveAudio=true'
    )
}
if ($CaptureNetworkPort -gt 0) {
    $arguments += @("-BskCaptureHost=$CaptureNetworkHost", "-BskCapturePort=$CaptureNetworkPort")
}
if ($AutoCommand) {
    $arguments += "-BskAutoCommand=$AutoCommand"
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
