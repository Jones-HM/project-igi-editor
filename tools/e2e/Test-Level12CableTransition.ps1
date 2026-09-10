[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ArtifactsRoot,
    [string]$GameRoot = 'D:\IGI1',
    [string]$EditorExePath = '',
    [string]$RouteManifest = '',
    [string]$CameraPath = '',
    [ValidateRange(1,14)][int]$Level = 12,
    [switch]$PrepareOnly,
    [ValidateRange(1,10000)][int]$SettleMilliseconds = 750
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Unquote([string]$Token) {
    $value = $Token.Trim()
    if ($value.Length -ge 2 -and $value[0] -eq '"' -and $value[$value.Length - 1] -eq '"') {
        return $value.Substring(1, $value.Length - 2).Replace('\\"', '"').Replace('\\\\', '\\')
    }
    return $value
}

function Get-TaskCalls([string]$Source) {
    $calls = [Collections.Generic.List[object]]::new()
    foreach ($match in [regex]::Matches($Source, 'Task_New\s*\(')) {
        $open = $Source.IndexOf('(', $match.Index)
        $depth = 0
        $quoted = $false
        $escaped = $false
        $close = -1
        for ($i = $open; $i -lt $Source.Length; $i++) {
            $ch = $Source[$i]
            if ($quoted) {
                if ($escaped) { $escaped = $false }
                elseif ($ch -eq '\\') { $escaped = $true }
                elseif ($ch -eq '"') { $quoted = $false }
                continue
            }
            if ($ch -eq '"') { $quoted = $true; continue }
            if ($ch -eq '(') { $depth++ }
            elseif ($ch -eq ')') {
                $depth--
                if ($depth -eq 0) { $close = $i; break }
            }
        }
        if ($close -gt $open) {
            $calls.Add([pscustomobject]@{
                start = $match.Index
                end = $close + 1
                text = $Source.Substring($match.Index, $close - $match.Index + 1)
            })
        }
    }
    return @($calls | Sort-Object start)
}

function Get-TopLevelArguments([string]$CallText) {
    $open = $CallText.IndexOf('(')
    $body = $CallText.Substring($open + 1, $CallText.Length - $open - 2)
    $args = [Collections.Generic.List[string]]::new()
    $start = 0
    $depth = 0
    $quoted = $false
    $escaped = $false
    for ($i = 0; $i -lt $body.Length; $i++) {
        $ch = $body[$i]
        if ($quoted) {
            if ($escaped) { $escaped = $false }
            elseif ($ch -eq '\\') { $escaped = $true }
            elseif ($ch -eq '"') { $quoted = $false }
            continue
        }
        if ($ch -eq '"') { $quoted = $true }
        elseif ($ch -eq '(') { $depth++ }
        elseif ($ch -eq ')') { $depth-- }
        elseif ($ch -eq ',' -and $depth -eq 0) {
            $args.Add($body.Substring($start, $i - $start).Trim())
            $start = $i + 1
        }
    }
    if ($start -lt $body.Length) { $args.Add($body.Substring($start).Trim()) }
    return @($args)
}

function Read-Double([string]$Token) {
    return [double]::Parse((Unquote $Token), [Globalization.NumberStyles]::Float, $Invariant)
}

function Read-RouteManifest([string]$QscPath, [string]$SourceHash) {
    $source = [IO.File]::ReadAllText($QscPath)
    $calls = @(Get-TaskCalls $source)
    $routes = [Collections.Generic.List[object]]::new()
    $authoredPlayerPosition = $null
    $buildingPositions = [Collections.Generic.List[object]]::new()
    foreach ($call in $calls) {
        $args = @(Get-TopLevelArguments $call.text)
        if ($args.Count -lt 6) { continue }
        $type = Unquote $args[1]
        if ($type -eq 'HumanPlayer') {
            $authoredPlayerPosition = @((Read-Double $args[3]), (Read-Double $args[4]), (Read-Double $args[5]))
        } elseif ($type -eq 'Building') {
            $buildingPositions.Add(@((Read-Double $args[3]), (Read-Double $args[4]), (Read-Double $args[5])))
        }
    }
    foreach ($parent in $calls) {
        $args = @(Get-TopLevelArguments $parent.text)
        if ($args.Count -lt 8 -or (Unquote $args[1]) -ne 'SplineObj') { continue }
        $children = @($calls | Where-Object { $_.start -gt $parent.start -and $_.end -lt $parent.end })
        $waypoints = [Collections.Generic.List[object]]::new()
        foreach ($child in $children) {
            $childArgs = @(Get-TopLevelArguments $child.text)
            if ($childArgs.Count -lt 9 -or (Unquote $childArgs[1]) -ne 'SplineObjWaypoint') { continue }
            $segment = if ($childArgs.Count -gt 10) { Unquote $childArgs[10] } else { '' }
            $waypoints.Add([pscustomobject]@{
                position = @((Read-Double $childArgs[6]), (Read-Double $childArgs[7]), (Read-Double $childArgs[8]))
                waypointModelId = if ($childArgs.Count -gt 9) { Unquote $childArgs[9] } else { '' }
                segmentModelId = $segment
                sourceOffset = $child.start
            })
        }
        if ($waypoints.Count -lt 2 -or @($waypoints | Where-Object segmentModelId).Count -eq 0) { continue }
        $routes.Add([pscustomobject]@{
            sourceOffset = $parent.start
            sourceTaskId = Unquote $args[0]
            linearSegments = ((Unquote $args[3]) -match '^(TRUE|true|1)$')
            splineSegmentCount = [int](Read-Double $args[7])
            waypoints = @($waypoints)
        })
    }
    if ($routes.Count -lt 2) { throw "Expected at least two authored spline routes with segment models in $QscPath; found $($routes.Count)." }
    $routeReference = @($routes[0].waypoints[0].position)
    $referenceBuildingPosition = $null
    if ($buildingPositions.Count -gt 0) {
        $referenceBuildingPosition = @($buildingPositions | ForEach-Object {
            $dx = [double]$_[0] - [double]$routeReference[0]
            $dy = [double]$_[1] - [double]$routeReference[1]
            $dz = [double]$_[2] - [double]$routeReference[2]
            [pscustomobject]@{ position = @($_[0], $_[1], $_[2]); distanceSquared = $dx * $dx + $dy * $dy + $dz * $dz }
        } | Sort-Object distanceSquared | Select-Object -First 1).position
    }
    return [pscustomobject]@{
        schemaVersion = 1
        level = $Level
        sourcePath = (Resolve-Path -LiteralPath (Join-Path $GameRoot "MISSIONS/location0/level$Level/objects.qvm")).Path
        sourceSha256 = $SourceHash
        generatedFrom = $QscPath
        cameraAnchors = [pscustomobject]@{
            authoredPlayerPosition = $authoredPlayerPosition
            referenceBuildingPosition = $referenceBuildingPosition
        }
        routes = @($routes)
    }
}

function Assert-RouteManifest($Manifest) {
    if ($null -eq $Manifest.routes -or @($Manifest.routes).Count -lt 2) { throw 'Route manifest must contain at least two spline routes.' }
    foreach ($route in @($Manifest.routes)) {
        if (@($route.waypoints).Count -lt 2) { throw 'Every cable route must contain at least two waypoints.' }
        if (@($route.waypoints | Where-Object { [string]::IsNullOrWhiteSpace([string]$_.segmentModelId) }).Count -eq @($route.waypoints).Count) {
            throw 'Every cable route must identify at least one segment model.'
        }
        foreach ($waypoint in @($route.waypoints)) {
            if (@($waypoint.position).Count -ne 3) { throw 'A route waypoint does not have a three-component position.' }
            foreach ($value in @($waypoint.position)) {
                if ([double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value)) { throw 'Route coordinates must be finite.' }
            }
        }
    }
}

function Get-LookAtPose($Position, $Target, [string]$Name) {
    $dx = [double]$Target[0] - [double]$Position[0]
    $dy = [double]$Target[1] - [double]$Position[1]
    $dz = [double]$Target[2] - [double]$Position[2]
    $horizontal = [Math]::Sqrt($dx * $dx + $dy * $dy)
    if ($horizontal -le 1e-9) { throw "Cannot derive camera heading for pose $Name." }
    return [pscustomobject]@{
        name = $Name
        position = @([double]$Position[0], [double]$Position[1], [double]$Position[2])
        target = @([double]$Target[0], [double]$Target[1], [double]$Target[2])
        yaw = [Math]::Atan2(-$dx, $dy) * 180.0 / [Math]::PI
        pitch = [Math]::Atan2($dz, $horizontal) * 180.0 / [Math]::PI
    }
}

function Add-Vector([double[]]$Left, [double[]]$Right) {
    $x = ([double]$Left[0]) + ([double]$Right[0])
    $y = ([double]$Left[1]) + ([double]$Right[1])
    $z = ([double]$Left[2]) + ([double]$Right[2])
    return @($x, $y, $z)
}

function Subtract-Vector([double[]]$Left, [double[]]$Right) {
    $x = ([double]$Left[0]) - ([double]$Right[0])
    $y = ([double]$Left[1]) - ([double]$Right[1])
    $z = ([double]$Left[2]) - ([double]$Right[2])
    return @($x, $y, $z)
}

function Scale-Vector([double[]]$Value, [double]$Scale) {
    $x = ([double]$Value[0]) * $Scale
    $y = ([double]$Value[1]) * $Scale
    $z = ([double]$Value[2]) * $Scale
    return @($x, $y, $z)
}

function Normalize-Vector([double[]]$Value, [double]$FallbackX = 0.0, [double]$FallbackY = 1.0, [double]$FallbackZ = 0.0) {
    $x = [double]($Value[0])
    $y = [double]($Value[1])
    $z = [double]($Value[2])
    $length = [Math]::Sqrt(($x * $x) + ($y * $y) + ($z * $z))
    if ($length -le 1e-9) { return @($FallbackX, $FallbackY, $FallbackZ) }
    $normalizedX = $x / $length
    $normalizedY = $y / $length
    $normalizedZ = $z / $length
    return @($normalizedX, $normalizedY, $normalizedZ)
}

function New-CameraPath($Manifest) {
    $route = @($Manifest.routes)[0]
    $waypoints = @($route.waypoints)
    if ($waypoints.Count -lt 3) { throw 'Cannot derive a camera path from fewer than three route waypoints.' }
    $first = @($waypoints[0].position | ForEach-Object { [double]$_ })
    $second = @($waypoints[1].position | ForEach-Object { [double]$_ })
    $third = @($waypoints[2].position | ForEach-Object { [double]$_ })
    $routeDirection = Normalize-Vector (Subtract-Vector $second $first)
    $horizontalDirection = Normalize-Vector @($routeDirection[0], $routeDirection[1], 0.0)
    $sideDirection = @(-[double]$horizontalDirection[1], [double]$horizontalDirection[0], 0.0)
    $dx = ([double]$second[0]) - ([double]$first[0])
    $dy = ([double]$second[1]) - ([double]$first[1])
    $dz = ([double]$second[2]) - ([double]$first[2])
    $firstSpan = [Math]::Sqrt(($dx * $dx) + ($dy * $dy) + ($dz * $dz))
    $observerDistance = [Math]::Min([Math]::Max($firstSpan * 0.5, 30000.0), 120000.0)
    $player = if ($Manifest.cameraAnchors.authoredPlayerPosition) {
        @($Manifest.cameraAnchors.authoredPlayerPosition | ForEach-Object { [double]$_ })
    } else {
        Add-Vector $first (Scale-Vector $sideDirection $observerDistance)
    }
    $building = if ($Manifest.cameraAnchors.referenceBuildingPosition) {
        @($Manifest.cameraAnchors.referenceBuildingPosition | ForEach-Object { [double]$_ })
    } else { @($first[0], $first[1], $first[2]) }
    $buildingExterior = Add-Vector (Add-Vector $building (Scale-Vector $sideDirection $observerDistance)) (Scale-Vector @(0, 0, 1) ($observerDistance * 0.35))
    $buildingInterior = Add-Vector $building (Scale-Vector @(0, 0, 1) ($observerDistance * 0.03))
    $pastPosition = Add-Vector (Add-Vector $third (Scale-Vector $sideDirection $observerDistance)) (Scale-Vector @(0, 0, 1) ($observerDistance * 0.20))
    $poses = [Collections.Generic.List[object]]::new()
    $poses.Add((Get-LookAtPose $player $first 'startup'))
    $poses.Add((Get-LookAtPose (Add-Vector (Subtract-Vector $first (Scale-Vector $horizontalDirection $observerDistance)) (Scale-Vector @(0, 0, 1) ($observerDistance * 0.20))) $first 'approach'))
    $poses.Add((Get-LookAtPose $buildingExterior $building 'doorway'))
    $poses.Add((Get-LookAtPose $buildingInterior (Add-Vector $building (Scale-Vector $horizontalDirection ($observerDistance * 0.20))) 'interior'))
    $poses.Add((Get-LookAtPose $pastPosition $third 'past-house'))
    $poses.Add((Get-LookAtPose $player $first 'reverse'))
    return [pscustomobject]@{
        schemaVersion = 1
        level = $Level
        poses = @($poses)
        derivation = 'authored HumanPlayer, nearest authored Building, and first route geometry'
    }
}

function Write-DesktopCapture([int]$ProcessId, [string]$Path) {
    Add-Type -AssemblyName System.Drawing
    if (-not ('CableTransitionNative' -as [type])) {
        Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CableTransitionNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
}
'@
    }
    $process = Get-Process -Id $ProcessId -ErrorAction Stop
    $handle = $process.MainWindowHandle
    $rect = New-Object CableTransitionNative+RECT
    if ($handle -eq [IntPtr]::Zero -or -not [CableTransitionNative]::GetWindowRect($handle, [ref]$rect)) { throw 'Could not resolve the editor window rectangle.' }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) { throw 'Editor window has an invalid capture rectangle.' }
    $bitmap = [Drawing.Bitmap]::new($width, $height)
    try {
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size) }
        finally { $graphics.Dispose() }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
}

function Read-SplineTrace([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing spline trace: $Path" }
    if ((Get-Item -LiteralPath $Path).Length -le 0) { throw "Empty spline trace: $Path" }
    $trace = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($null -eq $trace.tiles) { throw "Spline trace has no tiles array: $Path" }
    return $trace
}

function Assert-VectorClose($Actual, $Expected, [double]$Tolerance, [string]$Label) {
    if (@($Actual).Count -ne 3 -or @($Expected).Count -ne 3) { throw "Invalid vector in $Label." }
    $distance = 0.0
    for ($i = 0; $i -lt 3; $i++) {
        $delta = [double]$Actual[$i] - [double]$Expected[$i]
        $distance += $delta * $delta
    }
    $distance = [Math]::Sqrt($distance)
    if ($distance -gt $Tolerance) { throw "$Label is not within $Tolerance units (distance=$distance)." }
}

function Get-SplineTraceSignature($Trace) {
    $parts = [Collections.Generic.List[string]]::new()
    foreach ($tile in @($Trace.tiles | Sort-Object routeIndex, intervalIndex, tileIndex)) {
        $matrix = [string]::Join(',', @($tile.model | ForEach-Object { ([double]$_).ToString('R', $Invariant) }))
        $parts.Add(([string]::Join('|', @(
            [int]$tile.routeIndex, [int]$tile.intervalIndex, [int]$tile.tileIndex,
            [int]$tile.steps, [int]$tile.longitudinalAxis, [string]$tile.segmentModelId,
            $matrix
        ))))
    }
    return [string]::Join(';', $parts)
}

function Assert-SplineTrace($Trace, $Manifest, [string]$Label) {
    $expectedCount = 0
    for ($routeIndex = 0; $routeIndex -lt @($Manifest.routes).Count; $routeIndex++) {
        $route = @($Manifest.routes)[$routeIndex]
        $intervalCount = @($route.waypoints).Count - 1
        $steps = [int]$route.splineSegmentCount
        if ($steps -le 0) { throw "$Label route $routeIndex has no authored segment count." }
        $expectedCount += $intervalCount * $steps
        for ($intervalIndex = 0; $intervalIndex -lt $intervalCount; $intervalIndex++) {
            $tiles = @($Trace.tiles | Where-Object {
                [int]$_.routeIndex -eq $routeIndex -and [int]$_.intervalIndex -eq $intervalIndex
            } | Sort-Object tileIndex)
            if ($tiles.Count -ne $steps) {
                throw "$Label route $routeIndex interval $intervalIndex submitted $($tiles.Count) tiles; expected $steps."
            }
            Assert-VectorClose $tiles[0].tileBegin $route.waypoints[$intervalIndex].position 0.001 "$Label route $routeIndex interval $intervalIndex start"
            Assert-VectorClose $tiles[$tiles.Count - 1].tileEnd $route.waypoints[$intervalIndex + 1].position 0.001 "$Label route $routeIndex interval $intervalIndex end"
            for ($tileIndex = 1; $tileIndex -lt $tiles.Count; $tileIndex++) {
                Assert-VectorClose $tiles[$tileIndex].tileBegin $tiles[$tileIndex - 1].tileEnd 0.001 "$Label route $routeIndex interval $intervalIndex adjacency $tileIndex"
            }
        }
    }
    if ([int]$Trace.tileCount -ne $expectedCount -or @($Trace.tiles).Count -ne $expectedCount) {
        throw "$Label trace count $($Trace.tileCount)/$(@($Trace.tiles).Count) does not equal expected $expectedCount."
    }
}

$ArtifactsRoot = [IO.Path]::GetFullPath($ArtifactsRoot)
if (Test-Path -LiteralPath $ArtifactsRoot) { throw 'Use a fresh artifact directory.' }
New-Item -ItemType Directory -Path $ArtifactsRoot -Force | Out-Null
$sourceQvm = Join-Path $GameRoot "MISSIONS/location0/level$Level/objects.qvm"
if (-not (Test-Path -LiteralPath $sourceQvm -PathType Leaf)) { throw "Missing level QVM: $sourceQvm" }
$sourceHash = Get-Sha256 $sourceQvm

if ([string]::IsNullOrWhiteSpace($RouteManifest)) {
    $qvmCopy = Join-Path $ArtifactsRoot 'objects.qvm'
    $qscPath = Join-Path $ArtifactsRoot 'objects.qsc'
    Copy-Item -LiteralPath $sourceQvm -Destination $qvmCopy
    $converterCandidates = @(
        (Join-Path $PSScriptRoot '../../assets/editor/tools/igi1conv/igi1conv.exe'),
        (Join-Path $GameRoot 'editor/tools/igi1conv/igi1conv.exe')
    )
    $converter = $converterCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $converter) { throw 'igi1conv.exe is required to decompile the current level QVM.' }
    & $converter qvm decompile $qvmCopy -o $qscPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Current Level QVM decompilation failed.' }
    $manifest = Read-RouteManifest $qscPath $sourceHash
} else {
    $manifest = Get-Content -LiteralPath ([IO.Path]::GetFullPath($RouteManifest)) -Raw | ConvertFrom-Json
    Assert-RouteManifest $manifest
    if ($manifest.sourceSha256 -and ([string]$manifest.sourceSha256).ToLowerInvariant() -ne $sourceHash) { throw 'Route manifest source hash does not match the installed QVM.' }
}
Assert-RouteManifest $manifest
$manifestPath = Join-Path $ArtifactsRoot 'route-manifest.json'
$manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

if ([string]::IsNullOrWhiteSpace($CameraPath)) { $camera = New-CameraPath $manifest }
else { $camera = Get-Content -LiteralPath ([IO.Path]::GetFullPath($CameraPath)) -Raw | ConvertFrom-Json }
if ($null -eq $camera.poses -or @($camera.poses).Count -lt 6) { throw 'Camera path must contain at least six fixed poses.' }
$cameraPathOut = Join-Path $ArtifactsRoot 'camera-path.json'
$camera | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $cameraPathOut -Encoding UTF8
$capturedPoses = [Collections.Generic.List[object]]::new()

$report = [ordered]@{
    schemaVersion = 1; level = $Level; sourcePath = $sourceQvm; sourceSha256 = $sourceHash
    routeManifest = $manifestPath; cameraPath = $cameraPathOut; status = if ($PrepareOnly) { 'PREPARED' } else { 'NOT_RUN' }
    # Camera-only records are useful for preparation but do not have trace paths.
    poses = if ($PrepareOnly) { @($camera.poses) } else { @() }
}
if ($PrepareOnly) {
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $ArtifactsRoot 'report.json') -Encoding UTF8
    Write-Output "Prepared Level $Level cable transition: $(@($manifest.routes).Count) routes, $(@($camera.poses).Count) poses."
    exit 0
}

if ([string]::IsNullOrWhiteSpace($EditorExePath)) { $EditorExePath = Join-Path $GameRoot 'igi1ed.exe' }
$EditorExePath = [IO.Path]::GetFullPath($EditorExePath)
if (-not (Test-Path -LiteralPath $EditorExePath -PathType Leaf)) { throw "Editor executable not found: $EditorExePath" }
$commandPath = Join-Path $GameRoot 'editor/tools/debug-command.txt'
$commandOriginal = if (Test-Path -LiteralPath $commandPath) { [IO.File]::ReadAllBytes($commandPath) } else { $null }
$process = $null
try {
    $wmi = [wmiclass]'\\.\root\cimv2:Win32_Process'
    $created = $wmi.Create(('"' + $EditorExePath + '" --developer-mode --game-path "' + $GameRoot + '" -level ' + $Level), $GameRoot)
    if ([int]$created.ReturnValue -ne 0) { throw "WMI editor launch failed: $($created.ReturnValue)." }
    $process = Get-Process -Id ([int]$created.ProcessId) -ErrorAction Stop
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    do {
        $process.Refresh()
        if ($process.SessionId -eq 1 -and $process.Responding -and $process.WorkingSet64 -gt 30MB -and $process.MainWindowHandle -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($process.SessionId -ne 1 -or -not $process.Responding -or $process.WorkingSet64 -le 30MB) { throw 'Editor did not become a healthy interactive Session 1 process.' }

    $captureDir = Join-Path $ArtifactsRoot 'screenshots'; New-Item -ItemType Directory -Path $captureDir -Force | Out-Null
    $traceDir = Join-Path $ArtifactsRoot 'spline-traces'; New-Item -ItemType Directory -Path $traceDir -Force | Out-Null
    foreach ($pose in @($camera.poses)) {
        $line = 'set-camera level={0} x={1} y={2} z={3} yaw={4} pitch={5}' -f $Level,
            ([double]$pose.position[0]).ToString('R',$Invariant), ([double]$pose.position[1]).ToString('R',$Invariant),
            ([double]$pose.position[2]).ToString('R',$Invariant), ([double]$pose.yaw).ToString('R',$Invariant), ([double]$pose.pitch).ToString('R',$Invariant)
        [IO.File]::WriteAllText($commandPath, $line + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
        $consumedDeadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            if ((Test-Path -LiteralPath $commandPath -PathType Leaf) -and ((Get-Item -LiteralPath $commandPath).Length -eq 0)) { break }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::UtcNow -lt $consumedDeadline)
        if (-not (Test-Path -LiteralPath $commandPath -PathType Leaf) -or (Get-Item -LiteralPath $commandPath).Length -ne 0) { throw "Editor did not consume camera pose '$($pose.name)'." }
        Start-Sleep -Milliseconds $SettleMilliseconds
        $tracePath = Join-Path $traceDir (([string]$pose.name) + '.json')
        $traceLine = 'capture-spline level={0} path={1}' -f $Level, $tracePath
        [IO.File]::WriteAllText($commandPath, $traceLine + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
        $traceConsumedDeadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            if ((Test-Path -LiteralPath $commandPath -PathType Leaf) -and ((Get-Item -LiteralPath $commandPath).Length -eq 0)) { break }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::UtcNow -lt $traceConsumedDeadline)
        if (-not (Test-Path -LiteralPath $commandPath -PathType Leaf) -or (Get-Item -LiteralPath $commandPath).Length -ne 0) { throw "Editor did not consume spline trace command for pose '$($pose.name)'." }
        $traceDeadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            if (Test-Path -LiteralPath $tracePath -PathType Leaf) {
                if ((Get-Item -LiteralPath $tracePath).Length -gt 0) { break }
            }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::UtcNow -lt $traceDeadline)
        [void](Read-SplineTrace $tracePath)
        $capturePath = Join-Path $captureDir (([string]$pose.name) + '.png')
        Write-DesktopCapture $process.Id $capturePath
        if ((Get-Item -LiteralPath $capturePath).Length -le 0) { throw "Empty screenshot for pose '$($pose.name)'." }
        if (-not $PrepareOnly) {
            [void]$capturedPoses.Add([pscustomobject]@{
                name = [string]$pose.name; screenshot = $capturePath; trace = $tracePath
                position = @($pose.position); target = @($pose.target); yaw = $pose.yaw; pitch = $pose.pitch
            })
        }
    }
    $baselineSignature = $null
    $traceLabels = [Collections.Generic.List[string]]::new()
    foreach ($poseReport in @($capturedPoses)) {
        $trace = Read-SplineTrace ([string]$poseReport.trace)
        $label = "pose '$($poseReport.name)'"
        Assert-SplineTrace $trace $manifest $label
        $signature = Get-SplineTraceSignature $trace
        if ($null -eq $baselineSignature) { $baselineSignature = $signature }
        elseif ($signature -cne $baselineSignature) { throw "Spline transforms/submissions changed between poses at $label." }
        $traceLabels.Add([string]$poseReport.name)
    }
    $report.traceAssertions = [ordered]@{
        status = 'PASS'
        posesChecked = @($traceLabels)
        transformsStableAcrossPoses = $true
        endpointAndAdjacencyChecked = $true
    }
    $report.poses = @($capturedPoses)
    if ((Get-Sha256 $sourceQvm) -ne $sourceHash) { throw 'Installed level QVM changed during the transition capture.' }
    $report.status = 'PASS'
} finally {
    if ($null -ne $process) {
        try { if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; $process.WaitForExit(3000) | Out-Null } } catch {}
    }
    if ($null -ne $commandOriginal) { [IO.File]::WriteAllBytes($commandPath, $commandOriginal) }
    elseif (Test-Path -LiteralPath $commandPath) { Remove-Item -LiteralPath $commandPath -Force }
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $ArtifactsRoot 'report.json') -Encoding UTF8
}
Write-Output "Level $Level cable transition capture PASS: $($report.poses.Count) poses."
