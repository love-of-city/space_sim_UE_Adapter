param(
    [Parameter(Mandatory=$true)]
    [string]$MjcfPath,
    [Parameter(Mandatory=$true)]
    [string]$Destination,
    [string]$CatalogPath = '',
    [double]$BuildScale = 100.0,
    [double]$ComponentScale = 1.0,
    [string]$UnrealRoot = '',
    [ValidateSet('auto', 'preserve', 'recompute')]
    [string]$NormalMode = 'auto',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Set-BskPythonPath
$ue = Resolve-UnrealRoot $UnrealRoot
$resolvedMjcf = [IO.Path]::GetFullPath($MjcfPath)
if (!(Test-Path -LiteralPath $resolvedMjcf)) { throw "MJCF file does not exist: $resolvedMjcf" }
if (!$Destination.StartsWith('/Game/')) { throw '-Destination must start with /Game/.' }
if (!$CatalogPath) {
    $safeName = [IO.Path]::GetFileNameWithoutExtension($resolvedMjcf) -replace '[^A-Za-z0-9_-]', '_'
    $CatalogPath = Join-Path $ProjectRoot "Config\BskAssets\$safeName.json"
}
$resolvedCatalog = [IO.Path]::GetFullPath($CatalogPath)
$safeImportName = ([IO.Path]::GetFileNameWithoutExtension($resolvedCatalog) -replace '[^A-Za-z0-9_-]', '_')
$importSettings = Join-Path $ProjectRoot "Saved\AssetImport\$safeImportName.json"
$meshSettingsMarker = Join-Path $ProjectRoot "Saved\AssetImport\$safeImportName.mesh_settings"
$meshSettingsSignature = "$NormalMode|build=$BuildScale|component=$ComponentScale|nanite=off|lod0=full|v=1"

& conda run --no-capture-output -n mujoco-dev python (Join-Path $PSScriptRoot 'generate_mjcf_asset_catalog.py') `
    --mjcf $resolvedMjcf --destination $Destination --catalog $resolvedCatalog --import-settings $importSettings `
    --build-scale $BuildScale --component-scale $ComponentScale
if ($LASTEXITCODE -ne 0) { throw 'Failed to generate the MJCF asset catalog.' }

$catalogData = Get-Content -Raw -LiteralPath $resolvedCatalog | ConvertFrom-Json
$expectedMeshes = @($catalogData.assets.PSObject.Properties).Count
$expectedTextures = if ($catalogData.textures) { @($catalogData.textures.PSObject.Properties).Count } else { 0 }
$expectedAssets = $expectedMeshes + $expectedTextures
$relativeContent = $Destination.Substring('/Game/'.Length).Replace('/', '\')
$contentDirectory = Join-Path (Join-Path $ProjectRoot 'Content') $relativeContent
$existingAssets = @(Get-ChildItem -LiteralPath $contentDirectory -Filter '*.uasset' -File -ErrorAction SilentlyContinue).Count
$editorCmd = Join-Path $ue 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$needsImport = $Force -or $existingAssets -lt $expectedAssets
if ($needsImport) {
    Write-Output "Importing $expectedMeshes MJCF meshes and $expectedTextures textures into $Destination ..."
    & $editorCmd $ProjectFile -run=ImportAssets "-importsettings=$importSettings" "-dest=$Destination" -replaceexisting -nosourcecontrol -unattended -nop4 -nosplash
    $importExitCode = $LASTEXITCODE
    $existingAssets = @(Get-ChildItem -LiteralPath $contentDirectory -Filter '*.uasset' -File -ErrorAction SilentlyContinue).Count
    if ($existingAssets -lt $expectedAssets) {
        throw "UE asset import exited with $importExitCode; expected $expectedAssets assets, found $existingAssets."
    }
} else {
    Write-Output "MJCF assets already imported ($existingAssets .uasset files); applying normal mode '$NormalMode'."
}
$configuredSignature = if (Test-Path -LiteralPath $meshSettingsMarker) { (Get-Content -Raw -LiteralPath $meshSettingsMarker).Trim() } else { '' }
if ($needsImport -or $configuredSignature -ne $meshSettingsSignature) {
    $env:BSK_MJCF_ASSET_CATALOG = $resolvedCatalog
    $env:BSK_MJCF_NORMAL_MODE = $NormalMode
    try {
        $meshConfigScript = Join-Path $PSScriptRoot 'configure_imported_meshes.py'
        # StaticMeshEditorSubsystem rejects changes from PythonScriptCommandlet.
        # Execute in a headless editor session so LOD build settings are really
        # written, rebuilt, and saved rather than only logged as configured.
        & $editorCmd $ProjectFile "-ExecutePythonScript=$meshConfigScript" -unattended -nop4 -nosplash
        if ($LASTEXITCODE -ne 0) { throw "MJCF mesh build-setting configuration failed with exit code $LASTEXITCODE." }
        Set-Content -LiteralPath $meshSettingsMarker -Value $meshSettingsSignature -Encoding ascii
    } finally {
        Remove-Item Env:\BSK_MJCF_ASSET_CATALOG -ErrorAction SilentlyContinue
        Remove-Item Env:\BSK_MJCF_NORMAL_MODE -ErrorAction SilentlyContinue
    }
} else {
    Write-Output "MJCF mesh settings are already applied: $meshSettingsSignature"
}
Write-Output "MJCF assets imported successfully ($existingAssets .uasset files): $resolvedCatalog"
