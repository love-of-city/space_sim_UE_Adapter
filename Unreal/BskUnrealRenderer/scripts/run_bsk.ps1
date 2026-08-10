param(
    [string]$HostName = '127.0.0.1',
    [int]$Port = 5558,
    [double]$Duration = 60.0
)

. (Join-Path $PSScriptRoot 'common.ps1')
Set-BskPythonPath
$example = Join-Path $ProjectRoot 'examples\bsk_two_spacecraft_stream.py'
& conda run --no-capture-output -n mujoco-dev python $example --host $HostName --port $Port --duration $Duration
exit $LASTEXITCODE
