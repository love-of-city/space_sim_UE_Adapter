param(
    [string]$RecordingPath = '',
    [string]$BasiliskRoot = '',
    [switch]$ReuseRecording
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Set-BskPythonPath
if (!$RecordingPath) { $RecordingPath = Join-Path $ProjectRoot 'Saved\Recordings\demo8-test.bskrec' }
$RecordingPath = [IO.Path]::GetFullPath($RecordingPath)
if (!$ReuseRecording -or !(Test-Path -LiteralPath $RecordingPath)) {
    $basiliskSource = Resolve-BasiliskRoot $BasiliskRoot
    $scenario = Join-Path $ProjectRoot 'examples\scenario_mjscene_unreal.py'
    & conda run --no-capture-output -n mujoco-dev python $scenario --basilisk-root $basiliskSource --output $RecordingPath
    if ($LASTEXITCODE -ne 0) { throw 'Demo 8 recording failed.' }
}
$validator = Join-Path $ProjectRoot 'tests\validate_demo8_recording.py'
& conda run --no-capture-output -n mujoco-dev python $validator $RecordingPath
if ($LASTEXITCODE -ne 0) { throw 'Demo 8 validation failed.' }
