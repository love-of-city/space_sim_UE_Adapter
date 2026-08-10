param(
    [string]$UnrealRoot = '',
    [switch]$SkipBuild,
    [switch]$SkipUnreal
)

. (Join-Path $PSScriptRoot 'common.ps1')
Set-BskPythonPath
$tests = Join-Path $ProjectRoot 'tests'
& conda run --no-capture-output -n mujoco-dev python -m unittest discover -s $tests -v
if ($LASTEXITCODE -ne 0) { throw 'Python contract tests failed.' }

if (!$SkipUnreal) {
    $ue = Resolve-UnrealRoot $UnrealRoot
    if (!$SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -UnrealRoot $ue }
    $editorCmd = Join-Path $ue 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    & $editorCmd $ProjectFile -unattended -nop4 -nosplash -NullRHI '-ExecCmds=Automation RunTests BskUnreal;Quit' '-TestExit=Automation Test Queue Empty' -log
    if ($LASTEXITCODE -ne 0) { throw "Unreal automation tests failed with exit code $LASTEXITCODE" }
}
