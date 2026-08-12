param(
    [string]$ListenAddress = '127.0.0.1',
    [ValidateRange(1, 65535)]
    [int]$Port = 5560,
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Set-BskPythonPath
if (!$OutputDirectory) { $OutputDirectory = Join-Path $ProjectRoot 'Saved\BskCaptureNetwork' }
$receiver = Join-Path $ProjectRoot 'examples\receive_camera_products.py'
& conda run --no-capture-output -n mujoco-dev python $receiver --host $ListenAddress --port $Port --output ([IO.Path]::GetFullPath($OutputDirectory))
exit $LASTEXITCODE
