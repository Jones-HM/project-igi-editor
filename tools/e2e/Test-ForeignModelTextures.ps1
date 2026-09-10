[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateRange(1,14)][int]$SourceLevel,
    [Parameter(Mandatory)][ValidateRange(1,14)][int]$DestinationLevel,
    [Parameter(Mandatory)][string]$ModelId,
    [Parameter(Mandatory)][string]$GameRoot,
    [Parameter(Mandatory)][string]$EditorExePath,
    [Parameter(Mandatory)][string]$ArtifactsRoot,
    [switch]$AutomaticSource
)

$ErrorActionPreference = 'Stop'

function FullPath([string]$Path) { return [IO.Path]::GetFullPath($Path) }
function Sha256([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function RequireFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required file is missing: $Path" }
}
function LevelFile([string]$Root, [int]$Level, [string]$Relative) {
    return Join-Path $Root ("missions/location0/level{0}/{1}" -f $Level, $Relative)
}
function IsUnder([string]$Child, [string]$Parent) {
    $c = (FullPath $Child).TrimEnd('\') + '\'
    $p = (FullPath $Parent).TrimEnd('\') + '\'
    return $c.StartsWith($p, [StringComparison]::OrdinalIgnoreCase)
}
function ReadDatModel([string]$DatPath, [string]$Converter, [string]$WantedModel, [switch]$AllowMissing) {
    $lines = @(& $Converter dat export $DatPath --filter $WantedModel 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "DAT export failed for $DatPath." }
    $text = ($lines -join [Environment]::NewLine)
    $jsonStart = $text.IndexOf('{')
    if ($jsonStart -lt 0) { throw "DAT export did not produce JSON for $DatPath." }
    $report = $text.Substring($jsonStart) | ConvertFrom-Json
    $model = @($report.models | Where-Object { [string]$_.modelName -eq $WantedModel }) | Select-Object -First 1
    if ($null -eq $model) {
        if ($AllowMissing) { return $null }
        throw "DAT mapping '$WantedModel' was not found in $DatPath."
    }
    return $model
}
function ExtractEntry([string]$ResPath, [string]$EntryName, [string]$OutDir, [string]$Converter, [switch]$AllowMissing) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    & $Converter res extract $ResPath --file $EntryName -o $OutDir | Out-Null
    if ($LASTEXITCODE -ne 0) {
        if ($AllowMissing) { return $null }
        throw "RES extraction failed for '$EntryName' from $ResPath."
    }
    $file = Get-ChildItem -LiteralPath $OutDir -File | Select-Object -First 1
    if ($null -eq $file) {
        if ($AllowMissing) { return $null }
        throw "RES entry '$EntryName' was not extracted from $ResPath."
    }
    return $file.FullName
}
function Snapshot([string]$Root, [int]$Source, [int]$Destination, [string]$Model, [string]$Converter, [string]$OutputRoot) {
    $files = [ordered]@{}
    foreach ($level in @($Source, $Destination) | Sort-Object -Unique) {
        $files["level${level}.dat"] = @{ level = $level; path = (LevelFile $Root $level "level$level.dat"); sha256 = (Sha256 (LevelFile $Root $level "level$level.dat")) }
        $files["level${level}.mtp"] = @{ level = $level; path = (LevelFile $Root $level "level$level.mtp"); sha256 = (Sha256 (LevelFile $Root $level "level$level.mtp")) }
        $files["models-level${level}.res"] = @{ level = $level; path = (LevelFile $Root $level "models/level$level.res"); sha256 = (Sha256 (LevelFile $Root $level "models/level$level.res")) }
        $files["textures-level${level}.res"] = @{ level = $level; path = (LevelFile $Root $level "textures/level$level.res"); sha256 = (Sha256 (LevelFile $Root $level "textures/level$level.res")) }
    }
    $sourceModel = ReadDatModel $files["level${Source}.dat"].path $Converter $Model
    $destinationModel = ReadDatModel $files["level${Destination}.dat"].path $Converter $Model -AllowMissing
    $sourceTexDir = Join-Path $OutputRoot "source-textures"
    $destinationTexDir = Join-Path $OutputRoot "destination-textures"
    $sourceModelFile = ExtractEntry $files["models-level${Source}.res"].path ("LOCAL:models/{0}.mef" -f $Model) (Join-Path $OutputRoot 'source-model') $Converter
    $destinationModelFile = ExtractEntry $files["models-level${Destination}.res"].path ("LOCAL:models/{0}.mef" -f $Model) (Join-Path $OutputRoot 'destination-model') $Converter -AllowMissing
    $textureRows = @()
    foreach ($texture in @($sourceModel.textures)) {
        $sourceTextureFile = ExtractEntry $files["textures-level${Source}.res"].path ("LOCAL:textures/{0}.tex" -f $texture) (Join-Path $sourceTexDir ([string]$texture)) $Converter
        $destinationTextureFile = ExtractEntry $files["textures-level${Destination}.res"].path ("LOCAL:textures/{0}.tex" -f $texture) (Join-Path $destinationTexDir ([string]$texture)) $Converter -AllowMissing
        $textureRows += [pscustomobject]@{
            id = [string]$texture
            sourcePath = $sourceTextureFile
            sourceSha256 = Sha256 $sourceTextureFile
            destinationPath = $destinationTextureFile
            destinationSha256 = Sha256 $destinationTextureFile
        }
    }
    return [pscustomobject]@{
        sourceLevel = $Source; destinationLevel = $Destination; modelId = $Model
        files = $files
        sourceMapping = @($sourceModel.textures)
        destinationMapping = if ($null -eq $destinationModel) { @() } else { @($destinationModel.textures) }
        sourceModel = @{ path = $sourceModelFile; sha256 = Sha256 $sourceModelFile }
        destinationModel = @{ path = $destinationModelFile; sha256 = Sha256 $destinationModelFile }
        textures = @($textureRows)
    }
}

$gameRoot = FullPath $GameRoot
$editorPath = FullPath $EditorExePath
$artifactRoot = FullPath $ArtifactsRoot
RequireFile $editorPath
if (-not (Test-Path -LiteralPath $gameRoot -PathType Container)) { throw "Game root is missing: $gameRoot" }
if (Test-Path -LiteralPath $artifactRoot) { throw 'Use a fresh artifact directory.' }
if (IsUnder $artifactRoot $gameRoot -or IsUnder $gameRoot $artifactRoot) {
    throw 'ArtifactsRoot and GameRoot must be disjoint disposable roots.'
}
New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null

$converter = Join-Path ([IO.Path]::GetDirectoryName($editorPath)) 'editor/tools/igi1conv/igi1conv.exe'
RequireFile $converter
$sourceDat = LevelFile $gameRoot $SourceLevel "level$SourceLevel.dat"
$destinationDat = LevelFile $gameRoot $DestinationLevel "level$DestinationLevel.dat"
$sourceModelRes = LevelFile $gameRoot $SourceLevel "models/level$SourceLevel.res"
$destinationModelRes = LevelFile $gameRoot $DestinationLevel "models/level$DestinationLevel.res"
$sourceTextureRes = LevelFile $gameRoot $SourceLevel "textures/level$SourceLevel.res"
$destinationTextureRes = LevelFile $gameRoot $DestinationLevel "textures/level$DestinationLevel.res"
foreach ($path in @($sourceDat, $destinationDat, $sourceModelRes, $destinationModelRes, $sourceTextureRes, $destinationTextureRes)) { RequireFile $path }

$previousGamePath = [Environment]::GetEnvironmentVariable('IGI_GAME_PATH', 'Process')
$before = Snapshot $gameRoot $SourceLevel $DestinationLevel $ModelId $converter $artifactRoot
$before | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $artifactRoot 'before.json') -Encoding UTF8
$process = $null
$exitCode = $null
try {
    $env:IGI_GAME_PATH = $gameRoot
    $wmi = [wmiclass]'\\.\root\cimv2:Win32_Process'
    $command = ('"{0}" --game-path "{1}" --import-model {2} "{3}"' -f $editorPath, $gameRoot, $DestinationLevel, $ModelId)
    if (-not $AutomaticSource) { $command += (' --source-level {0}' -f $SourceLevel) }
    $created = $wmi.Create($command, $gameRoot)
    if ([int]$created.ReturnValue -ne 0) { throw "WMI editor launch failed: $($created.ReturnValue)." }
    $process = [Diagnostics.Process]::GetProcessById([int]$created.ProcessId)
    if ($process.SessionId -ne 1) { throw "Editor import ran in Session $($process.SessionId), expected Session 1." }
    $observedSessionId = $process.SessionId
    # Acquire the process handle while the WMI-created process is still alive;
    # after termination Windows may no longer expose ExitCode through a newly
    # opened Process wrapper.
    $processHandle = $process.Handle
    $observedWorkingSet = [int64]$process.WorkingSet64
    if ($observedWorkingSet -le 30MB) { throw 'Editor import process did not reach the required 30 MB memory footprint.' }
    if (-not $process.WaitForExit(180000)) { throw 'Editor import did not exit within 180 seconds.' }
    try {
        $process.Refresh()
        $exitCode = $process.ExitCode
    } catch { $exitCode = $null }
} finally {
    if ($null -ne $previousGamePath) { $env:IGI_GAME_PATH = $previousGamePath }
    else { Remove-Item Env:IGI_GAME_PATH -ErrorAction SilentlyContinue }
}

if ($null -eq $exitCode) { throw 'Editor import exit code was not observable.' }
if ($exitCode -ne 0) {
    if ($AutomaticSource) {
        throw "Automatic source selection rejected the import as ambiguous (editor exit code $exitCode)."
    }
    throw "Explicit-source model import failed (editor exit code $exitCode)."
}

$after = Snapshot $gameRoot $SourceLevel $DestinationLevel $ModelId $converter $artifactRoot
$after | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $artifactRoot 'after.json') -Encoding UTF8
if (($after.destinationMapping -join "`n") -ne ($before.sourceMapping -join "`n")) { throw 'Destination DAT mapping does not match the selected source mapping.' }
foreach ($row in @($after.textures)) {
    $sourceRow = @($before.textures | Where-Object id -eq $row.id) | Select-Object -First 1
    if ($null -eq $sourceRow -or $row.destinationSha256 -ne $sourceRow.sourceSha256) {
        throw "Destination texture '$($row.id)' does not match selected source bytes."
    }
}
if ($after.destinationModel.sha256 -ne $before.sourceModel.sha256) {
    throw 'Destination model bytes do not match selected source bytes.'
}
$report = [ordered]@{
    schemaVersion = 1; status = 'PASS'; sourceLevel = $SourceLevel; automaticSource = [bool]$AutomaticSource; destinationLevel = $DestinationLevel; modelId = $ModelId
    gameRoot = $gameRoot; editorExePath = $editorPath; converter = $converter; editorExitCode = $exitCode
    editorSessionId = $observedSessionId; editorWorkingSetBytes = $observedWorkingSet
    editorExitCodeObserved = ($null -ne $exitCode)
    before = (Join-Path $artifactRoot 'before.json'); after = (Join-Path $artifactRoot 'after.json')
    destinationFilesChanged = @($after.files.GetEnumerator() | Where-Object {
        $_.Value.level -eq $DestinationLevel -and $_.Value.sha256 -ne $before.files[$_.Key].sha256
    } | ForEach-Object Key)
}
$report | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $artifactRoot 'report.json') -Encoding UTF8
Write-Output "Foreign model texture import PASS: $ModelId source level $SourceLevel -> destination level $DestinationLevel."
