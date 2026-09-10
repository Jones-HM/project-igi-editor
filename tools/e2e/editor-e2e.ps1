#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\IGI1',
    [string]$EditorPath = '',
    [string]$ScenarioPath = '',
    [string]$ScenarioName = '',
    [string]$ArtifactsRoot = '',
    [switch]$ValidateOnly,
    [switch]$AllowGameDataMutation,
    [switch]$KeepEditorOpen
)

$ErrorActionPreference = 'Stop'
$script:SupportedSteps = @(
    'launch_editor', 'wait_for_window', 'wait_for_log', 'assert_process',
    'key', 'key_hold', 'click', 'type_text', 'wait', 'screenshot',
    'assert_screenshot_region', 'assert_screenshot_color_ratio',
    'assert_screenshot_difference', 'assert_log', 'mark_log', 'assert_file',
    'assert_path', 'assert_cursor_visible', 'assert_cursor_hidden', 'close_editor',
    'capture_window_state', 'orbit_camera', 'assert_file_hash',
    'snapshot_paths', 'restore_paths', 'assert_log_count',
    'select_graph_node', 'nudge_graph_node', 'assert_graph_edit', 'start_animation', 'pause_animation'
)
$script:OrbitAngles = @(
    'front', 'back', 'left', 'right', 'top', 'bottom',
    'front-left', 'front-right', 'back-left', 'back-right'
)
# Global constraint #19 (plan) and the design spec: a scenario cannot pass
# without BOTH a screenshot/UI oracle and a state/data oracle.
$script:StateOracleTypes = @(
    'assert_process', 'capture_window_state', 'assert_log', 'assert_log_count',
    'assert_file', 'assert_path', 'assert_file_hash', 'snapshot_paths', 'restore_paths',
    'assert_graph_edit'
)

function Fail([string]$Message) {
    throw [System.InvalidOperationException]::new($Message)
}
function Get-FullPath([string]$Path) { return [System.IO.Path]::GetFullPath($Path) }
function Assert-UnderRoot([string]$Path, [string]$Root, [string]$Label) {
    $fullPath = Get-FullPath $Path
    $fullRoot = (Get-FullPath $Root).TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "$Label - $fullPath"
    }
    return $fullPath
}
function Assert-DeclaredPathUnderRoot([string]$Path, [string]$Root, [string]$Label) {
    # Manifest-declared paths are relative to the game root.  Reject empty,
    # absolute, or rooted-escape declarations before any launch.
    if ([string]::IsNullOrWhiteSpace($Path)) { Fail "$Label requires a non-empty path." }
    if ([IO.Path]::IsPathRooted($Path)) { Fail "$Label path must be relative to the game root (got '$Path')." }
    $combined = Join-Path $Root $Path
    $resolved = Get-FullPath $combined
    $fullRoot = (Get-FullPath $Root).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "$Label path must be relative to the game root (got '$Path')."
    }
    return $resolved
}
function Get-Property($Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}
function Assert-Integer($Value, [string]$Name, [int]$Minimum, [int]$Maximum) {
    if ($null -eq $Value -or $Value -is [bool] -or [int]$Value -ne $Value) { Fail "$Name must be an integer." }
    if ([int]$Value -lt $Minimum -or [int]$Value -gt $Maximum) { Fail "$Name must be between $Minimum and $Maximum." }
}
function Assert-Region($Region, [string]$Name) {
    if ($null -eq $Region -or @($Region).Count -ne 4) { Fail "$Name must be [x,y,width,height]." }
    for ($i = 0; $i -lt 4; $i++) { if ([int]$Region[$i] -lt 0) { Fail "$Name contains a negative value." } }
    if ([int]$Region[2] -le 0 -or [int]$Region[3] -le 0) { Fail "$Name width and height must be positive." }
}
function Assert-PathArray($Paths, [string]$Name) {
    if ($null -eq $Paths -or @($Paths).Count -eq 0) { Fail "$Name requires a non-empty paths array." }
    foreach ($path in @($Paths)) {
        if ($null -eq $path -or [string]::IsNullOrWhiteSpace([string]$path)) { Fail "$Name contains an empty path." }
    }
}
function Assert-Sha256([string]$Value, [string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Value)) { Fail "$Name requires sha256." }
    if ($Value -notmatch '^[0-9a-fA-F]{64}$') { Fail "$Name sha256 must be a 64-character hexadecimal SHA-256 digest." }
}
function Get-UiOracle([string]$Type, $Step) {
    if ($Type -eq 'screenshot') { return $true }
    if ($Type -eq 'assert_screenshot_region' -or $Type -eq 'assert_screenshot_color_ratio' -or $Type -eq 'assert_screenshot_difference') { return $true }
    if ($Type -eq 'orbit_camera') {
        # An orbit without either a following explicit screenshot or an anchor
        # screenshot whose region overlaps the window center is silent; see the
        # note on Invoke-OrbitCamera. Requiring a real pixel sample before the
        # runner proves the rendered result keeps the pair rule honest.
        if ($null -ne (Get-Property $Step 'screenshotAfter')) { return $true }
    }
    return $false
}
function Assert-RequiresMutationPolicy($Scenario, [string]$Name) {
    # The reversible primitives are mandatory on any mutation (Task 2 brief);
    # they must never run inside a scenario that did not declare
    # requiresMutation=true, which itself requires -AllowGameDataMutation at
    # run time (mirroring the existing mutation gate).  Legacy mutating
    # scenarios written before these primitives still set requiresMutation=true
    # and perform their own manual reset, so no reverse requirement is imposed.
    $steps = Get-Property $Scenario 'steps'
    foreach ($step in @($steps)) {
        if (@('snapshot_paths', 'restore_paths') -contains [string](Get-Property $step 'type') -and -not [bool](Get-Property $Scenario 'requiresMutation')) {
            Fail "Scenario '$Name' uses snapshot_paths/restore_paths but does not set requiresMutation=true."
        }
    }
}
function Assert-ObserverPair($Scenario, [string]$Name) {
    $hasUi = $false; $hasState = $false
    foreach ($step in @(Get-Property $Scenario 'steps')) {
        $type = [string](Get-Property $step 'type')
        if (Get-UiOracle $type $step) { $hasUi = $true }
        if ($script:StateOracleTypes -contains $type) { $hasState = $true }
    }
    if (-not $hasUi) { Fail "Scenario '$Name' has no screenshot/UI oracle; a scenario cannot pass without a screenshot/UI oracle." }
    if (-not $hasState) { Fail "Scenario '$Name' has no state/data oracle; a scenario cannot pass without a state/data oracle." }
}
function Validate-Step($Step, [string]$ScenarioName, [int]$Index, [string]$Root) {
    $id = [string](Get-Property $Step 'id')
    $type = [string](Get-Property $Step 'type')
    if ([string]::IsNullOrWhiteSpace($id)) { Fail "Scenario '$ScenarioName' step $Index is missing id." }
    if ([string]::IsNullOrWhiteSpace($type) -or $script:SupportedSteps -notcontains $type) { Fail "Scenario '$ScenarioName' step '$id' has unsupported type '$type'." }
    switch ($type) {
        'launch_editor' {
            Assert-Integer (Get-Property $Step 'level') "$ScenarioName/$id level" 1 14
            if ($null -ne (Get-Property $Step 'drawParts')) { Assert-Integer (Get-Property $Step 'drawParts') "$ScenarioName/$id drawParts" -2 127 }
            if ($null -ne (Get-Property $Step 'developerMode') -and (Get-Property $Step 'developerMode') -isnot [bool]) { Fail "$ScenarioName/$id developerMode must be boolean." }
        }
        'wait_for_window' { if ($null -ne (Get-Property $Step 'timeoutSeconds')) { Assert-Integer (Get-Property $Step 'timeoutSeconds') "$ScenarioName/$id timeoutSeconds" 1 300 } }
        'wait_for_log' { if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'pattern'))) { Fail "$ScenarioName/$id requires pattern." } }
        'key' { if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'key'))) { Fail "$ScenarioName/$id requires key." } }
        'key_hold' {
            if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'key'))) { Fail "$ScenarioName/$id requires key." }
            if ($null -eq (Get-Property $Step 'seconds') -or [double](Get-Property $Step 'seconds') -le 0 -or [double](Get-Property $Step 'seconds') -gt 300) { Fail "$ScenarioName/$id seconds must be greater than 0 and no more than 300." }
        }
        'click' {
            Assert-Integer (Get-Property $Step 'x') "$ScenarioName/$id x" 0 10000
            Assert-Integer (Get-Property $Step 'y') "$ScenarioName/$id y" 0 10000
            $button = [string]$(if ($null -ne (Get-Property $Step 'button')) { Get-Property $Step 'button' } else { 'left' })
            if (@('left','right') -notcontains $button.ToLowerInvariant()) { Fail "$ScenarioName/$id button must be left or right." }
        }
        'select_graph_node' {
            Assert-Integer (Get-Property $Step 'x') "$ScenarioName/$id x" 0 10000
            Assert-Integer (Get-Property $Step 'y') "$ScenarioName/$id y" 0 10000
            if ($null -eq (Get-Property $Step 'nodeId')) { Fail "$ScenarioName/$id requires nodeId." }
            Assert-Integer (Get-Property $Step 'nodeId') "$ScenarioName/$id nodeId" -2147483648 2147483647
        }
        'nudge_graph_node' {
            Assert-Integer (Get-Property $Step 'x') "$ScenarioName/$id x" 0 10000
            Assert-Integer (Get-Property $Step 'y') "$ScenarioName/$id y" 0 10000
            Assert-Integer (Get-Property $Step 'nodeId') "$ScenarioName/$id nodeId" -2147483648 2147483647
            if ([string](Get-Property $Step 'field') -notin @('x','y','z')) { Fail "$ScenarioName/$id field must be x, y, or z." }
            if ($null -eq (Get-Property $Step 'delta')) { Fail "$ScenarioName/$id requires delta." }
        }
        'start_animation' {
            Assert-Integer (Get-Property $Step 'x') "$ScenarioName/$id x" 0 10000
            Assert-Integer (Get-Property $Step 'y') "$ScenarioName/$id y" 0 10000
            if ($null -ne (Get-Property $Step 'animationId')) { Assert-Integer (Get-Property $Step 'animationId') "$ScenarioName/$id animationId" -1 100000 }
        }
        'pause_animation' {
            Assert-Integer (Get-Property $Step 'x') "$ScenarioName/$id x" 0 10000
            Assert-Integer (Get-Property $Step 'y') "$ScenarioName/$id y" 0 10000
        }
        'type_text' { if ($null -eq (Get-Property $Step 'text')) { Fail "$ScenarioName/$id requires text." } }
        'wait' {
            if ($null -eq (Get-Property $Step 'seconds')) { Fail "$ScenarioName/$id requires seconds." }
            if ([double](Get-Property $Step 'seconds') -lt 0 -or [double](Get-Property $Step 'seconds') -gt 300) { Fail "$ScenarioName/$id seconds out of range." }
        }
        'screenshot' {
            if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'name'))) { Fail "$ScenarioName/$id requires name." }
            if ($null -ne (Get-Property $Step 'client') -and (Get-Property $Step 'client') -isnot [bool]) { Fail "$ScenarioName/$id client must be boolean." }
            if ((Get-Property $Step 'client') -and $null -ne (Get-Property $Step 'region')) { Fail "$ScenarioName/$id client capture cannot also declare a desktop region." }
            if ($null -ne (Get-Property $Step 'region')) { Assert-Region (Get-Property $Step 'region') "$ScenarioName/$id region" }
        }
        'assert_screenshot_region' {
            Assert-Region (Get-Property $Step 'region') "$ScenarioName/$id region"
            $hasLimit = $false
            foreach ($name in @('minNonBlackRatio','minUniqueRatio','minMeanLuma','maxMeanLuma')) {
                if ($null -ne (Get-Property $Step $name)) { $hasLimit = $true }
            }
            if (-not $hasLimit) { Fail "$ScenarioName/$id requires an image metric." }
        }
        'assert_screenshot_color_ratio' {
            Assert-Region (Get-Property $Step 'region') "$ScenarioName/$id region"
            $rgb = Get-Property $Step 'rgb'
            if ($null -eq $rgb -or @($rgb).Count -ne 3) { Fail "$ScenarioName/$id rgb must be [r,g,b]." }
            foreach ($channel in @($rgb)) { Assert-Integer $channel "$ScenarioName/$id rgb" 0 255 }
            if ($null -eq (Get-Property $Step 'minRatio') -and $null -eq (Get-Property $Step 'maxRatio')) { Fail "$ScenarioName/$id requires minRatio or maxRatio." }
            if ($null -ne (Get-Property $Step 'tolerance')) { Assert-Integer (Get-Property $Step 'tolerance') "$ScenarioName/$id tolerance" 0 255 }
        }
        'assert_screenshot_difference' {
            if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'from')) -or [string]::IsNullOrWhiteSpace([string](Get-Property $Step 'to'))) { Fail "$ScenarioName/$id requires from and to screenshot names." }
            Assert-Region (Get-Property $Step 'region') "$ScenarioName/$id region"
            $hasLimit = $false
            foreach ($name in @('minChangedRatio','maxChangedRatio','minMeanAbsDifference','maxMeanAbsDifference')) {
                if ($null -ne (Get-Property $Step $name)) { $hasLimit = $true }
            }
            if (-not $hasLimit) { Fail "$ScenarioName/$id requires an image difference metric." }
            if ($null -ne (Get-Property $Step 'changedPixelThreshold')) { Assert-Integer (Get-Property $Step 'changedPixelThreshold') "$ScenarioName/$id changedPixelThreshold" 0 255 }
        }
        'assert_log' {
            if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'pattern'))) { Fail "$ScenarioName/$id requires pattern." }
            if ($null -ne (Get-Property $Step 'timeoutSeconds')) { Assert-Integer (Get-Property $Step 'timeoutSeconds') "$ScenarioName/$id timeoutSeconds" 1 300 }
        }
        'assert_file' {
            if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'path'))) { Fail "$ScenarioName/$id requires path." }
            if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'pattern'))) { Fail "$ScenarioName/$id requires pattern." }
            if ($null -ne (Get-Property $Step 'timeoutSeconds')) { Assert-Integer (Get-Property $Step 'timeoutSeconds') "$ScenarioName/$id timeoutSeconds" 1 300 }
        }
        'assert_path' {
            if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'path'))) { Fail "$ScenarioName/$id requires path." }
            $kind = [string]$(if ($null -ne (Get-Property $Step 'kind')) { Get-Property $Step 'kind' } else { 'file' })
            if (@('file','directory') -notcontains $kind.ToLowerInvariant()) { Fail "$ScenarioName/$id kind must be file or directory." }
            if ($null -ne (Get-Property $Step 'minBytes')) { Assert-Integer (Get-Property $Step 'minBytes') "$ScenarioName/$id minBytes" 0 2147483647 }
            if ($null -ne (Get-Property $Step 'timeoutSeconds')) { Assert-Integer (Get-Property $Step 'timeoutSeconds') "$ScenarioName/$id timeoutSeconds" 1 300 }
        }
        'assert_cursor_visible' { }
        'assert_cursor_hidden' { }
        'orbit_camera' {
            $angle = [string](Get-Property $Step 'angle')
            if ($script:OrbitAngles -notcontains $angle) {
                Fail "$ScenarioName/$id angle must be one of: $($script:OrbitAngles -join ', ') (got '$angle')."
            }
            if ($null -eq (Get-Property $Step 'distance')) { Fail "$ScenarioName/$id requires distance." }
            if ([double](Get-Property $Step 'distance') -le 0 -or [double](Get-Property $Step 'distance') -gt 100000) { Fail "$ScenarioName/$id distance must be greater than 0 and no more than 100000." }
            if ($null -eq (Get-Property $Step 'pixels')) { Fail "$ScenarioName/$id requires pixels." }
            Assert-Integer (Get-Property $Step 'pixels') "$ScenarioName/$id pixels" 1 5000
        }
        'capture_window_state' { }
        'assert_file_hash' {
            if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'path'))) { Fail "$ScenarioName/$id requires path." }
            [void](Assert-DeclaredPathUnderRoot ([string](Get-Property $Step 'path')) $Root "$ScenarioName/$id")
            Assert-Sha256 ([string](Get-Property $Step 'sha256')) "$ScenarioName/$id"
            if ($null -ne (Get-Property $Step 'timeoutSeconds')) { Assert-Integer (Get-Property $Step 'timeoutSeconds') "$ScenarioName/$id timeoutSeconds" 1 300 }
        }
        'snapshot_paths' {
            Assert-PathArray (Get-Property $Step 'paths') "$ScenarioName/$id"
            foreach ($path in @(Get-Property $Step 'paths')) { [void](Assert-DeclaredPathUnderRoot ([string]$path) $Root "$ScenarioName/$id") }
        }
        'restore_paths' {
            if ($null -ne (Get-Property $Step 'paths')) {
                Assert-PathArray (Get-Property $Step 'paths') "$ScenarioName/$id"
                foreach ($path in @(Get-Property $Step 'paths')) { [void](Assert-DeclaredPathUnderRoot ([string]$path) $Root "$ScenarioName/$id") }
            }
        }
        'assert_log_count' {
            if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'pattern'))) { Fail "$ScenarioName/$id requires pattern." }
            if ($null -eq (Get-Property $Step 'min')) { Fail "$ScenarioName/$id requires min." }
            Assert-Integer (Get-Property $Step 'min') "$ScenarioName/$id min" 0 2147483647
            if ($null -ne (Get-Property $Step 'max')) { Assert-Integer (Get-Property $Step 'max') "$ScenarioName/$id max" 0 2147483647 }
            if ($null -ne (Get-Property $Step 'timeoutSeconds')) { Assert-Integer (Get-Property $Step 'timeoutSeconds') "$ScenarioName/$id timeoutSeconds" 1 300 }
        }
        'assert_graph_edit' {
            if ([string]::IsNullOrWhiteSpace([string](Get-Property $Step 'path'))) { Fail "$ScenarioName/$id requires path." }
            [void](Assert-DeclaredPathUnderRoot ([string](Get-Property $Step 'path')) $Root "$ScenarioName/$id")
            if ($null -eq (Get-Property $Step 'nodeId')) { Fail "$ScenarioName/$id requires nodeId." }
            Assert-Integer (Get-Property $Step 'nodeId') "$ScenarioName/$id nodeId" -2147483648 2147483647
            $field = [string](Get-Property $Step 'field')
            if (@('x','y','z','gamma','radius','material','criteria') -notcontains $field) { Fail "$ScenarioName/$id field must be x, y, z, gamma, radius, material, or criteria." }
            if ($field -ne 'criteria' -and $null -eq (Get-Property $Step 'delta')) { Fail "$ScenarioName/$id numeric graph edit requires delta." }
        }
    }
}
function Assert-RestoreHasSnapshot($Scenario, [string]$Name) {
    # restore_paths may only restore what a preceding snapshot_paths captured;
    # a bare restore (no snapshot earlier in the same scenario) has nothing to
    # verify against and is rejected before launch.
    $steps = @(Get-Property $Scenario 'steps')
    $sawSnapshot = $false
    foreach ($step in $steps) {
        $type = [string](Get-Property $step 'type')
        if ($type -eq 'snapshot_paths') { $sawSnapshot = $true }
        if ($type -eq 'restore_paths' -and -not $sawSnapshot) {
            Fail "Scenario '$Name' restore_paths has no preceding snapshot_paths; restoration cannot be verified."
        }
    }
}
function Assert-ActionResultDependencies($Scenario, [string]$Name) {
    # An action-result oracle must explicitly identify an earlier action.  This
    # keeps a screenshot or process-liveness assertion from being relabelled as
    # proof that a later edit persisted.
    $seen = @{}
    foreach ($step in @(Get-Property $Scenario 'steps')) {
        $id = [string](Get-Property $step 'id')
        $role = Get-Property $step 'oracleRole'
        if ($null -ne $role -and [string]$role -ne 'action-result') {
            Fail "Scenario '$Name' step '$id' has unsupported oracleRole '$role'."
        }
        if ([string]$role -eq 'action-result') {
            $dependency = [string](Get-Property $step 'dependsOnStepId')
            if ([string]::IsNullOrWhiteSpace($dependency) -or -not $seen.ContainsKey($dependency)) {
                Fail "Scenario '$Name' action-result oracle '$id' must reference an earlier step."
            }
        } elseif ($null -ne (Get-Property $step 'dependsOnStepId')) {
            Fail "Scenario '$Name' step '$id' declares dependsOnStepId without oracleRole=action-result."
        }
        $seen[$id] = $true
    }
}
function Validate-Scenario($Scenario, [string]$Root) {
    $name = [string](Get-Property $Scenario 'name')
    Assert-RequiresMutationPolicy $Scenario $name
    Assert-ObserverPair $Scenario $name
    Assert-RestoreHasSnapshot $Scenario $name
    Assert-ActionResultDependencies $Scenario $name
}
function Validate-Manifest($Manifest, [string]$Root) {
    if ($null -eq $Manifest) { Fail 'Manifest is empty.' }
    $scenarios = Get-Property $Manifest 'scenarios'
    if ($null -eq $scenarios -or @($scenarios).Count -eq 0) { Fail 'Manifest requires a non-empty scenarios array.' }
    $scenarioNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($scenario in @($scenarios)) {
        $name = [string](Get-Property $scenario 'name')
        if ([string]::IsNullOrWhiteSpace($name)) { Fail 'Every scenario requires name.' }
        if (-not $scenarioNames.Add($name)) { Fail "Duplicate scenario name '$name'." }
        Assert-Integer (Get-Property $scenario 'level') "$name level" 1 14
        if ($null -ne (Get-Property $scenario 'inputScale')) {
            $inputScale = [double](Get-Property $scenario 'inputScale')
            if ($inputScale -le 0 -or $inputScale -gt 2) { Fail "Scenario '$name' inputScale must be greater than 0 and no more than 2." }
        }
        $requiresMutation = [bool](Get-Property $scenario 'requiresMutation')
        $steps = Get-Property $scenario 'steps'
        if ($null -eq $steps -or @($steps).Count -eq 0) { Fail "Scenario '$name' requires steps." }
        $stepIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        $index = 0
        foreach ($step in @($steps)) {
            $id = [string](Get-Property $step 'id')
            if ([string]::IsNullOrWhiteSpace($id)) { Fail "Scenario '$name' step $index is missing id." }
            if (-not $stepIds.Add($id)) { Fail "Scenario '$name' has duplicate step id '$id'." }
            Validate-Step $step $name $index $Root
            $index++
        }
        Validate-Scenario $scenario $Root
    }
    return @($scenarios)
}

if (-not ('EditorE2E_Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class EditorE2E_Native {
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)] public struct CursorInfo { public int cbSize; public uint flags; public IntPtr hCursor; public Point ptScreenPos; }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out Rect lpRect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref Point lpPoint);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern short VkKeyScan(char ch);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CursorInfo info);
    public const uint LeftDown = 0x0002;
    public const uint LeftUp = 0x0004;
    public const uint RightDown = 0x0008;
    public const uint RightUp = 0x0010;
    public const uint KeyUp = 0x0002;
    public const uint WheelScroll = 0x0800;
    public const int ShowNormal = 5;
    public static void Click(int x, int y, bool right) {
        SetCursorPos(x, y);
        mouse_event(right ? RightDown : LeftDown, 0, 0, 0, UIntPtr.Zero);
        mouse_event(right ? RightUp : LeftUp, 0, 0, 0, UIntPtr.Zero);
    }
    public static void Drag(int x1, int y1, int x2, int y2, bool right) {
        SetCursorPos(x1, y1);
        mouse_event(right ? RightDown : LeftDown, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        int steps = 12;
        for (int i = 1; i <= steps; i++) {
            int x = x1 + ((x2 - x1) * i) / steps;
            int y = y1 + ((y2 - y1) * i) / steps;
            SetCursorPos(x, y);
            System.Threading.Thread.Sleep(16);
        }
        mouse_event(right ? RightUp : LeftUp, 0, 0, 0, UIntPtr.Zero);
    }
    public static void ScrollWheel(int notches) {
        mouse_event(WheelScroll, 0, 0, (uint)notches, UIntPtr.Zero);
    }
    public static void Key(byte key, bool up) { keybd_event(key, 0, up ? KeyUp : 0, UIntPtr.Zero); }
    public static bool CursorVisible() { var info = new CursorInfo(); info.cbSize = Marshal.SizeOf(typeof(CursorInfo)); return GetCursorInfo(ref info) && (info.flags & 1) != 0; }
    public static int[] ClientRect(IntPtr hWnd) {
        Rect r; int cx = 0; int cy = 0;
        if (hWnd != IntPtr.Zero && GetClientRect(hWnd, out r)) {
            Point topLeft = new Point(); topLeft.X = r.Left; topLeft.Y = r.Top;
            ClientToScreen(hWnd, ref topLeft);
            cx = topLeft.X + (r.Right - r.Left) / 2;
            cy = topLeft.Y + (r.Bottom - r.Top) / 2;
            return new int[] { r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top, topLeft.X, topLeft.Y, cx, cy };
        }
        return new int[] { 0, 0, 0, 0, 0, 0, cx, cy };
    }
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    public const uint WM_CLOSE = 0x0010;
    private static IntPtr _enumFoundHwnd = IntPtr.Zero;
    private static uint _enumTargetPid = 0;
    private static bool EnumWindowCallback(IntPtr hWnd, IntPtr lParam) {
        uint pid;
        GetWindowThreadProcessId(hWnd, out pid);
        if (pid == _enumTargetPid && IsWindowVisible(hWnd)) {
            Rect r;
            if (GetClientRect(hWnd, out r)) {
                int w = r.Right - r.Left;
                int h = r.Bottom - r.Top;
                if (w > 100 && h > 100) {
                    _enumFoundHwnd = hWnd;
                    return false;
                }
            }
        }
        return true;
    }
    public static IntPtr FindWindowForProcess(uint processId) {
        _enumFoundHwnd = IntPtr.Zero;
        _enumTargetPid = processId;
        EnumWindows(EnumWindowCallback, IntPtr.Zero);
        return _enumFoundHwnd;
    }
}
'@
}
function Get-EditorWindowHandle($ProcessOrId) {
    $pidToFind = if ($ProcessOrId -is [System.Diagnostics.Process]) { $ProcessOrId.Id } else { [int]$ProcessOrId }
    $hWnd = [EditorE2E_Native]::FindWindowForProcess([uint32]$pidToFind)
    if ($hWnd -ne [IntPtr]::Zero) { return $hWnd }
    $p = if ($ProcessOrId -is [System.Diagnostics.Process]) { $ProcessOrId } else { Get-Process -Id $pidToFind -ErrorAction SilentlyContinue }
    if ($null -ne $p -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
        return $p.MainWindowHandle
    }
    return [IntPtr]::Zero
}
function Get-VirtualKey([string]$Name) {
    $name = $Name.ToUpperInvariant()
    $named = @{
        'ESC' = 0x1B; 'ENTER' = 0x0D; 'TAB' = 0x09; 'SPACE' = 0x20; 'BACKSPACE' = 0x08
        'DELETE' = 0x2E; 'UP' = 0x26; 'DOWN' = 0x28; 'LEFT' = 0x25; 'RIGHT' = 0x27
        'PAGEUP' = 0x21; 'PAGEDOWN' = 0x22; 'F2' = 0x71; 'F3' = 0x72; 'F4' = 0x73
        'F5' = 0x74; 'F6' = 0x75; 'F7' = 0x76; 'F8' = 0x77; 'F11' = 0x7A
        'CTRL' = 0x11; 'ALT' = 0x12; 'SHIFT' = 0x10
    }
    if ($named.ContainsKey($name)) { return [byte]$named[$name] }
    if ($name.Length -eq 1) {
        $code = [int][char]$name[0]
        if (($code -ge 0x30 -and $code -le 0x39) -or ($code -ge 0x41 -and $code -le 0x5A)) { return [byte]$code }
    }
    Fail "Unsupported virtual key '$Name'."
}
function Send-Key([string]$KeyName) {
    $parts = $KeyName.ToUpperInvariant().Split('+')
    $modifiers = @()
    for ($i = 0; $i -lt $parts.Length - 1; $i++) { $modifiers += Get-VirtualKey $parts[$i] }
    $key = Get-VirtualKey $parts[$parts.Length - 1]
    foreach ($modifier in $modifiers) { [EditorE2E_Native]::Key($modifier, $false) }
    [EditorE2E_Native]::Key($key, $false)
    # Keep the chord down long enough for GLUT's callback and the editor's
    # GetAsyncKeyState-based configurable binding check to observe it.
    Start-Sleep -Milliseconds 80
    [EditorE2E_Native]::Key($key, $true)
    for ($i = $modifiers.Length - 1; $i -ge 0; $i--) { [EditorE2E_Native]::Key($modifiers[$i], $true) }
}
function Send-KeyForDuration([string]$KeyName, [double]$Seconds) {
    $parts = $KeyName.ToUpperInvariant().Split('+')
    $modifiers = @()
    for ($i = 0; $i -lt $parts.Length - 1; $i++) { $modifiers += Get-VirtualKey $parts[$i] }
    $key = Get-VirtualKey $parts[$parts.Length - 1]
    foreach ($modifier in $modifiers) { [EditorE2E_Native]::Key($modifier, $false) }
    [EditorE2E_Native]::Key($key, $false)
    Start-Sleep -Milliseconds ([int]($Seconds * 1000))
    [EditorE2E_Native]::Key($key, $true)
    for ($i = $modifiers.Length - 1; $i -ge 0; $i--) { [EditorE2E_Native]::Key($modifiers[$i], $true) }
}
function Send-Text([string]$Text) {
    foreach ($character in $Text.ToCharArray()) {
        $encoded = [EditorE2E_Native]::VkKeyScan($character)
        if ($encoded -eq -1) { Fail "Cannot inject character '$character'." }
        $virtualKey = [byte]($encoded -band 0xFF)
        $shiftState = [byte](($encoded -shr 8) -band 0xFF)
        $held = @()
        if (($shiftState -band 1) -ne 0) { $held += [byte]0x10 }
        if (($shiftState -band 2) -ne 0) { $held += [byte]0x11 }
        if (($shiftState -band 4) -ne 0) { $held += [byte]0x12 }
        foreach ($modifier in $held) { [EditorE2E_Native]::Key($modifier, $false) }
        $scan = [byte]([EditorE2E_Native]::MapVirtualKey($virtualKey, 0) -band 0xFF)
        [EditorE2E_Native]::keybd_event($virtualKey, $scan, 0, [UIntPtr]::Zero)
        [EditorE2E_Native]::keybd_event($virtualKey, $scan, [EditorE2E_Native]::KeyUp, [UIntPtr]::Zero)
        for ($i = $held.Count - 1; $i -ge 0; $i--) { [EditorE2E_Native]::Key($held[$i], $true) }
        Start-Sleep -Milliseconds 20
    }
}
function Wait-ForEditor([int]$ProcessId, [int]$TimeoutSeconds = 30) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
        if ($null -ne $process) {
            try { $process.Refresh() } catch {}
            $hWnd = Get-EditorWindowHandle $ProcessId
            if ($hWnd -ne [IntPtr]::Zero) {
                if ($process.SessionId -ne 1) { Fail "Editor PID $ProcessId is not on interactive Session 1." }
                return $process
            }
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    Fail "Editor PID $ProcessId did not expose a responsive window within $TimeoutSeconds seconds."
}
function Focus-Editor($Process) {
    $hWnd = Get-EditorWindowHandle $Process
    if ($hWnd -ne [IntPtr]::Zero) {
        [void][EditorE2E_Native]::ShowWindow($hWnd, [EditorE2E_Native]::ShowNormal)
        [void][EditorE2E_Native]::BringWindowToTop($hWnd)
        [void][EditorE2E_Native]::SetForegroundWindow($hWnd)
        [void][EditorE2E_Native]::SetFocus($hWnd)
        Start-Sleep -Milliseconds 120
    }
}
function Capture-Screenshot([string]$Path, $Region, [IntPtr]$ClientWindow = [IntPtr]::Zero) {
    Add-Type -AssemblyName System.Drawing
    Add-Type -AssemblyName System.Windows.Forms
    $previousDpi = [IntPtr]::Zero
    if ($ClientWindow -ne [IntPtr]::Zero) {
        $previousDpi = [EditorE2E_Native]::SetThreadDpiAwarenessContext([IntPtr](-4))
        if ($previousDpi -eq [IntPtr]::Zero) { Fail 'Cannot enable physical-pixel client capture.' }
    }
    try {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $x = 0; $y = 0; $width = $bounds.Width; $height = $bounds.Height
    if ($ClientWindow -ne [IntPtr]::Zero) {
        $client = [EditorE2E_Native]::ClientRect($ClientWindow)
        $x=$client[4]; $y=$client[5]; $width=$client[2]; $height=$client[3]
        if ($width -le 0 -or $height -le 0) { Fail 'Cannot resolve client capture bounds.' }
    }
    if ($null -ne $Region) { $x = [int]$Region[0]; $y = [int]$Region[1]; $width = [int]$Region[2]; $height = [int]$Region[3] }
    $bitmap = [System.Drawing.Bitmap]::new($width, $height)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.CopyFromScreen($x, $y, 0, 0, [System.Drawing.Size]::new($width, $height)) } finally { $graphics.Dispose() }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
    return [pscustomobject]@{ path = $Path; x = $x; y = $y; width = $width; height = $height }
    } finally {
        if ($previousDpi -ne [IntPtr]::Zero) { [void][EditorE2E_Native]::SetThreadDpiAwarenessContext($previousDpi) }
    }
}
function Get-ImageMetrics([string]$Path, $Region) {
    Add-Type -AssemblyName System.Drawing
    $image = [System.Drawing.Bitmap]::new($Path)
    try {
        $x = [int]$Region[0]; $y = [int]$Region[1]; $w = [int]$Region[2]; $h = [int]$Region[3]
        if ($x + $w -gt $image.Width -or $y + $h -gt $image.Height) { Fail "Image region exceeds screenshot bounds: $Path" }
        $step = [Math]::Max(1, [int]([Math]::Max($w,$h) / 120))
        $samples = 0; $nonBlack = 0; $lumaSum = 0.0
        $colors = [System.Collections.Generic.HashSet[string]]::new()
        for ($py = $y; $py -lt $y + $h; $py += $step) {
            for ($px = $x; $px -lt $x + $w; $px += $step) {
                $pixel = $image.GetPixel($px,$py)
                $luma = (0.2126 * $pixel.R) + (0.7152 * $pixel.G) + (0.0722 * $pixel.B)
                $samples++; $lumaSum += $luma
                if ($luma -gt 18) { $nonBlack++ }
                [void]$colors.Add(('{0:X2}{1:X2}{2:X2}' -f ($pixel.R -shr 4),($pixel.G -shr 4),($pixel.B -shr 4)))
            }
        }
        return [pscustomobject]@{ samples=$samples; nonBlackRatio=$nonBlack/[double]$samples; uniqueRatio=$colors.Count/[double]$samples; meanLuma=$lumaSum/[double]$samples }
    } finally { $image.Dispose() }
}
function Get-ImageColorRatio([string]$Path, $Region, $Rgb, [int]$Tolerance) {
    Add-Type -AssemblyName System.Drawing
    $image = [System.Drawing.Bitmap]::new($Path)
    try {
        $x = [int]$Region[0]; $y = [int]$Region[1]; $w = [int]$Region[2]; $h = [int]$Region[3]
        if ($x + $w -gt $image.Width -or $y + $h -gt $image.Height) { Fail "Image region exceeds screenshot bounds: $Path" }
        $step = [Math]::Max(1, [int]([Math]::Max($w,$h) / 160))
        $samples = 0; $matching = 0
        for ($py = $y; $py -lt $y + $h; $py += $step) {
            for ($px = $x; $px -lt $x + $w; $px += $step) {
                $pixel = $image.GetPixel($px,$py); $samples++
                if ([Math]::Abs($pixel.R - [int]$Rgb[0]) -le $Tolerance -and
                    [Math]::Abs($pixel.G - [int]$Rgb[1]) -le $Tolerance -and
                    [Math]::Abs($pixel.B - [int]$Rgb[2]) -le $Tolerance) { $matching++ }
            }
        }
        return [pscustomobject]@{ samples=$samples; matching=$matching; ratio=$matching/[double]$samples; rgb=@([int]$Rgb[0],[int]$Rgb[1],[int]$Rgb[2]); tolerance=$Tolerance }
    } finally { $image.Dispose() }
}
function Get-ImageDifferenceMetrics([string]$FromPath, [string]$ToPath, $Region, [int]$ChangedPixelThreshold) {
    Add-Type -AssemblyName System.Drawing
    $from = [System.Drawing.Bitmap]::new($FromPath)
    $to = [System.Drawing.Bitmap]::new($ToPath)
    try {
        if ($from.Width -ne $to.Width -or $from.Height -ne $to.Height) { Fail "Compared screenshots have different dimensions: $FromPath, $ToPath" }
        $x = [int]$Region[0]; $y = [int]$Region[1]; $w = [int]$Region[2]; $h = [int]$Region[3]
        if ($x + $w -gt $from.Width -or $y + $h -gt $from.Height) { Fail "Image region exceeds compared screenshot bounds." }
        $step = [Math]::Max(1, [int]([Math]::Max($w,$h) / 160))
        $samples = 0; $changed = 0; $differenceSum = 0.0
        for ($py = $y; $py -lt $y + $h; $py += $step) {
            for ($px = $x; $px -lt $x + $w; $px += $step) {
                $a = $from.GetPixel($px,$py); $b = $to.GetPixel($px,$py)
                $difference = ([Math]::Abs($a.R-$b.R) + [Math]::Abs($a.G-$b.G) + [Math]::Abs($a.B-$b.B)) / 3.0
                $samples++; $differenceSum += $difference
                if ($difference -ge $ChangedPixelThreshold) { $changed++ }
            }
        }
        return [pscustomobject]@{ samples=$samples; changed=$changed; changedRatio=$changed/[double]$samples; meanAbsDifference=$differenceSum/[double]$samples; threshold=$ChangedPixelThreshold }
    } finally { $from.Dispose(); $to.Dispose() }
}
function Get-AppendedLog([string]$LogPath, [long]$Offset) {
    if (-not (Test-Path -LiteralPath $LogPath)) { return '' }
    for ($attempt = 0; $attempt -lt 5; $attempt++) {
        try {
            $stream = [System.IO.FileStream]::new($LogPath, [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            try {
                $length = [int]$stream.Length
                $start = [Math]::Min([Math]::Max(0,[int]$Offset), $length)
                if ($start -ge $length) { return '' }
                $stream.Position = $start
                $bytes = New-Object byte[] ($length - $start)
                [void]$stream.Read($bytes, 0, $bytes.Length)
                return [System.Text.Encoding]::UTF8.GetString($bytes)
            } finally { $stream.Dispose() }
        } catch [System.IO.IOException] {
            Start-Sleep -Milliseconds 100
        }
    }
    return ''
}
function Wait-ForPattern([string]$LogPath, [long]$Offset, [string]$Pattern, [int]$TimeoutSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $text = Get-AppendedLog $LogPath $Offset
        if ($text -match $Pattern) { return $text }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    return $null
}
function Wait-ForFilePattern([string]$Path, [string]$Pattern, [bool]$MustNotMatch, [int]$TimeoutSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $text = if (Test-Path -LiteralPath $Path) { Get-Content -LiteralPath $Path -Raw } else { '' }
        $matches = $text -match $Pattern
        if (($MustNotMatch -and -not $matches) -or (-not $MustNotMatch -and $matches)) { return $true }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}
function Close-Editor($Process, [bool]$Force) {
    if ($null -eq $Process) { return $null }
    $current = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
    if ($null -eq $current) { return [pscustomobject]@{ exited=$true; exitCode=$null; forced=$Force } }
    $hWnd = Get-EditorWindowHandle $current.Id
    if ($hWnd -ne [IntPtr]::Zero) {
        [void][EditorE2E_Native]::PostMessage($hWnd, [EditorE2E_Native]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    } else {
        & taskkill.exe /PID $current.Id 2>$null | Out-Null
    }
    if ($current.WaitForExit(4000)) {
        Start-Sleep -Milliseconds 400
        return [pscustomobject]@{ exited=$true; exitCode=$current.ExitCode; forced=$false }
    }
    if ($Force) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 400
        return [pscustomobject]@{ exited=$true; exitCode=$null; forced=$true }
    }
    return [pscustomobject]@{ exited=$false; exitCode=$null; forced=$false }
}
function Get-Sha256([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-GraphExport($Path, [string]$ScratchRoot) {
    $repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSCommandPath))
    $converter = Join-Path $repoRoot 'assets\editor\tools\igi1conv\igi1conv.exe'
    if (-not (Test-Path -LiteralPath $converter -PathType Leaf)) { Fail "Graph assertion converter is missing: $converter" }
    $jsonPath = Join-Path $ScratchRoot ('graph-' + [guid]::NewGuid().ToString('N') + '.json')
    try {
        $output = @(& $converter graph export $Path -o $jsonPath 2>&1)
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
            Fail "igi1conv graph export failed for '$Path': $($output -join [Environment]::NewLine)"
        }
        return (Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json)
    } finally {
        Remove-Item -LiteralPath $jsonPath -Force -ErrorAction SilentlyContinue
    }
}
function Assert-GraphEdit($Step, [string]$Root, [string]$SnapshotStaging, $Record) {
    if ([string]::IsNullOrWhiteSpace($SnapshotStaging)) { Fail 'assert_graph_edit requires a preceding snapshot_paths step.' }
    $manifestPath = Join-Path $SnapshotStaging 'snapshot.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { Fail 'assert_graph_edit could not find the graph snapshot manifest.' }
    $relative = [string]$Step.path
    $snapshot = @(Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json | Where-Object { [string]$_.path -eq $relative })
    if ($snapshot.Count -ne 1) { Fail "assert_graph_edit path '$relative' was not captured by snapshot_paths." }
    $baselinePath = Join-Path $SnapshotStaging (Get-StagingFileName $relative)
    $currentPath = Assert-UnderRoot (Join-Path $Root $relative) $Root 'assert_graph_edit path'
    $scratch = Join-Path $SnapshotStaging ('graph-assert-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    try {
        $before = Get-GraphExport $baselinePath $scratch
        $after = Get-GraphExport $currentPath $scratch
        $beforeNodes = @($before.nodes); $afterNodes = @($after.nodes)
        if ($beforeNodes.Count -ne $afterNodes.Count -or [int]$before.edge_count -ne [int]$after.edge_count) { Fail 'Graph edit changed graph topology counts.' }
        $targetId = [int]$Step.nodeId
        $beforeTarget = @($beforeNodes | Where-Object { [int]$_.id -eq $targetId })
        $afterTarget = @($afterNodes | Where-Object { [int]$_.id -eq $targetId })
        if ($beforeTarget.Count -ne 1 -or $afterTarget.Count -ne 1) { Fail "Graph edit node $targetId was not present exactly once before and after the edit." }
        foreach ($node in $beforeNodes) {
            $other = @($afterNodes | Where-Object { [int]$_.id -eq [int]$node.id })
            if ($other.Count -ne 1) { Fail "Graph edit removed or duplicated node $($node.id)." }
            $fields = @('x','y','z','gamma','radius','material','criteria','link1','link2')
            foreach ($field in $fields) {
                if ([int]$node.id -eq $targetId -and $field -eq [string]$Step.field) { continue }
                if ([string]$node.$field -ne [string]$other[0].$field) { Fail "Graph edit changed node $($node.id) field '$field' outside the declared edit." }
            }
        }
        $beforeEdges = @($before.edges | ForEach-Object { "$($_.from)|$($_.to)|$($_.link_type)" } | Sort-Object)
        $afterEdges = @($after.edges | ForEach-Object { "$($_.from)|$($_.to)|$($_.link_type)" } | Sort-Object)
        if (($beforeEdges -join "`n") -ne ($afterEdges -join "`n")) { Fail 'Graph edit changed edge data outside the declared node edit.' }
        $beforeValue = $beforeTarget[0].([string]$Step.field)
        $afterValue = $afterTarget[0].([string]$Step.field)
        if ([string]$beforeValue -eq [string]$afterValue) { Fail "Graph edit did not change node $targetId field '$($Step.field)'." }
        if ($null -ne $Step.delta -and $Step.field -ne 'criteria') {
            $actualDelta = [double]$afterValue - [double]$beforeValue
            if ([Math]::Abs($actualDelta - [double]$Step.delta) -gt 0.001) { Fail "Graph edit delta for node $targetId field '$($Step.field)' was $actualDelta, expected $($Step.delta)." }
        }
        $Record.nodeId = $targetId; $Record.field = [string]$Step.field; $Record.before = $beforeValue; $Record.after = $afterValue
    } finally {
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    }
}
function New-WorkingCopyDirectory([string]$Root) {
    # The deployed retail root may allow file replacement but deny creating
    # sibling directories. Keep snapshot bytes in the user's temp directory;
    # only the declared game files are ever written back during restoration.
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ('igi-editor-e2e-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return $path
}
function Get-OrbitDelta([string]$Angle, [int]$Pixels) {
    if ($Pixels -le 0) { Fail 'orbit_camera pixels must be greater than 0.' }
    # Baseline deltas are calibrated for the default pixel budget of 12 (a
    # 180-degree yaw/pitch turn maps to a 900px drag across a typical viewport,
    # diagonals to half that).  The declared pixels value scales the drag so the
    # sequence is deterministic and honors the bound.
    $scale = $Pixels / 12.0
    switch ($Angle) {
        'front'  { return @{ yawPixels = 0; pitchPixels = 0 } }
        'back'   { return @{ yawPixels = [int][Math]::Round(-900 * $scale); pitchPixels = 0 } }
        'left'   { return @{ yawPixels = [int][Math]::Round(450 * $scale); pitchPixels = 0 } }
        'right'  { return @{ yawPixels = [int][Math]::Round(-450 * $scale); pitchPixels = 0 } }
        'top'    { return @{ yawPixels = 0; pitchPixels = [int][Math]::Round(900 * $scale) } }
        'bottom' { return @{ yawPixels = 0; pitchPixels = [int][Math]::Round(-900 * $scale) } }
        'front-left'  { return @{ yawPixels = [int][Math]::Round(225 * $scale); pitchPixels = 0 } }
        'front-right' { return @{ yawPixels = [int][Math]::Round(-225 * $scale); pitchPixels = 0 } }
        'back-left'   { return @{ yawPixels = [int][Math]::Round(675 * $scale); pitchPixels = 0 } }
        'back-right'  { return @{ yawPixels = [int][Math]::Round(-675 * $scale); pitchPixels = 0 } }
        default { Fail "Unsupported orbit angle '$Angle'." }
    }
}
function Invoke-OrbitCamera($Process, [string]$Angle, [int]$Pixels, [double]$Distance) {
    # Deterministic, data-driven camera sequence.  It snaps the camera to the
    # selection (F11 -> object, SHIFT+F11 -> object at configured radius),
    # holds ALT (the installed editor binding for camera mode), drags the mouse
    # across the viewport by the fixed pixel delta mapped from the requested
    # view, and then zooms toward the orbit target with ALT+SPACE wheel
    # increments scaled from the requested distance.  The intended angle and
    # distance are recorded verbatim in the step record; asserting the rendered
    # pixels for each angle is a later task that composes screenshot oracles on
    # top of this primitive.
    Focus-Editor $process
    Send-Key 'F11'
    Start-Sleep -Milliseconds 350
    $hWnd = Get-EditorWindowHandle $process
    $geometry = [EditorE2E_Native]::ClientRect($hWnd)
    $centerX = $geometry[6]; $centerY = $geometry[7]
    if ($centerX -le 0 -or $centerY -le 0) { Fail 'orbit_camera could not resolve a viewport center from the editor window.' }
    [EditorE2E_Native]::Key([byte]0x12, $false)  # ALT down (CameraEnable)
    Start-Sleep -Milliseconds 150
    $delta = Get-OrbitDelta $Angle $Pixels
    $targetX = $centerX + $delta.yawPixels
    $targetY = $centerY + $delta.pitchPixels
    [EditorE2E_Native]::Drag($centerX, $centerY, $targetX, $targetY, $false)
    Start-Sleep -Milliseconds 120
    # distance scale: 40 world units per notch, 120 notches max (modifier held)
    $notches = [Math]::Max(1, [Math]::Min(120, [int][Math]::Floor($Distance / 40.0)))
    [EditorE2E_Native]::Key([byte]0x20, $false)  # SPACE down (CameraAdjustRadius)
    Start-Sleep -Milliseconds 120
    for ($i = 0; $i -lt $notches; $i++) {
        [EditorE2E_Native]::ScrollWheel(120)
        Start-Sleep -Milliseconds 20
    }
    [EditorE2E_Native]::Key([byte]0x20, $true)
    Start-Sleep -Milliseconds 80
    [EditorE2E_Native]::Key([byte]0x12, $true)   # ALT up
    Start-Sleep -Milliseconds 200
    return [pscustomobject]@{ angle=$Angle; distance=$Distance; pixels=$Pixels; notches=$notches; centerX=$centerX; centerY=$centerY; sequence='F11+ALT+drag+ALT+SPACE+wheel' }
}
function Invoke-CaptureWindowState($Process) {
    if ($null -eq $Process) { Fail 'capture_window_state requires a live editor process (run launch_editor first).' }
    $current = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
    if ($null -eq $current) { Fail 'capture_window_state requires a live editor process (run launch_editor first).' }
    $current.Refresh()
    $hWnd = Get-EditorWindowHandle $current
    $geometry = [EditorE2E_Native]::ClientRect($hWnd)
    if ($hWnd -eq [IntPtr]::Zero -or $geometry[2] -le 0 -or $geometry[3] -le 0) {
        Fail 'capture_window_state could not resolve the editor window client bounds.'
    }
    $state = [ordered]@{
        pid = $current.Id
        windowHandle = ('0x{0:X}' -f $hWnd.ToInt64())
        sessionId = $current.SessionId
        responding = $current.Responding
        workingSetMb = [Math]::Round($current.WorkingSet64 / 1MB, 2)
        clientBounds = [ordered]@{ left=$geometry[0]; top=$geometry[1]; width=$geometry[2]; height=$geometry[3]; screenX=$geometry[4]; screenY=$geometry[5]; centerX=$geometry[6]; centerY=$geometry[7] }
    }
    if ($state.sessionId -ne 1) { Fail "Editor PID $($current.Id) is not on interactive Session 1." }
    if (-not $state.responding) { Fail "Editor PID $($current.Id) is not responding." }
    return $state
}
function Get-HashWaitResult([string]$Path, [string]$ExpectedHash, [int]$TimeoutSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            $actual = Get-Sha256 $Path
            if ($actual -eq $ExpectedHash.ToLowerInvariant()) { return [pscustomobject]@{ matched=$true; hash=$actual; present=$true } }
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    $final = if (Test-Path -LiteralPath $Path -PathType Leaf) { Get-Sha256 $Path } else { '' }
    return [pscustomobject]@{ matched=$false; hash=$final; present=(Test-Path -LiteralPath $Path -PathType Leaf) }
}
function Resolve-DeclaredPaths($Paths, [string]$Root) {
    $resolved = @()
    foreach ($entry in @($Paths)) {
        $combined = Join-Path $Root ([string]$entry)
        $resolved += (Assert-UnderRoot $combined $Root 'declared path')
    }
    return @($resolved)
}
function Get-RelativeToRoot([string]$Path, [string]$Root) {
    $fullRoot = (Get-FullPath $Root).TrimEnd('\') + '\'
    $fullPath = Get-FullPath $Path
    return $fullPath.Substring($fullRoot.Length).TrimStart('\')
}
function Get-StagingFileName([string]$RelativePath) {
    # Map a game-root-relative path onto a flat staging directory without
    # basename collisions: two files named e.g. a\qedconfig.qsc and
    # b\qedconfig.qsc must not overwrite each other.
    return ([string]$RelativePath -replace '[\\/]', '__')
}
function Invoke-RestorePaths($Record, [string]$Root, [string]$ScenarioDir, [string]$StagingPath, $Process) {
    # Restoration on any mutation: write every snapshot back from the staging
    # copy, then prove each restored file's SHA-256 equals its snapshot hash.
    # Failures surface as step records plus a failure screenshot when a window
    # or screen is available.
    $manifestPath = Join-Path $StagingPath 'snapshot.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        $restoreFailure = $null
        try { if ($null -ne $Process) { $restoreFailure = Capture-Screenshot (Join-Path $ScenarioDir 'failure.png') $null } } catch {}
        $failureMessage = 'restore_paths has no snapshot to restore; run snapshot_paths on the same paths first.'
        if ($null -ne $restoreFailure) { $failureMessage += " (failure screenshot: $($restoreFailure.path))" }
        throw [System.InvalidOperationException]::new($failureMessage)
    }
    $snapshotManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifestEntries = @($snapshotManifest)
    if ($manifestEntries.Count -eq 0) { throw 'restore_paths snapshot is empty.' }
    $records = New-Object System.Collections.Generic.List[object]
    foreach ($snapshot in $manifestEntries) {
        $relative = [string](Get-Property $snapshot 'path')
        $snapshotHash = [string](Get-Property $snapshot 'sha256')
        $dest = Assert-UnderRoot (Join-Path $Root $relative) $Root 'restore destination'
        $copied = $false; $restoreError = $null
        try {
            if (-not (Test-Path -LiteralPath $dest)) {
                $parent = Split-Path -Parent $dest
                if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
            }
            Copy-Item -LiteralPath (Join-Path $StagingPath (Get-StagingFileName $relative)) -Destination $dest -Force
            $copied = $true
        } catch { $restoreError = $_.Exception.Message }
        $actual = if ($copied -and (Test-Path -LiteralPath $dest -PathType Leaf)) { Get-Sha256 $dest } else { '' }
        $verified = ($copied -and $actual -eq $snapshotHash)
        $records.Add([pscustomobject]@{ path=$relative; restored=$copied; snapshotHash=$snapshotHash; restoredHash=$actual; verified=$verified; error=$restoreError })
        if (-not $verified) {
            $failureScreenshot = $null
            try { if ($null -ne $Process) { $failureScreenshot = Capture-Screenshot (Join-Path $ScenarioDir 'failure.png') $null } } catch {}
            $message = "Restore of '$relative' failed (snapshot=$snapshotHash restored=$actual)."
            if ($null -ne $failureScreenshot) { $message += " Failure screenshot: $($failureScreenshot.path)" }
            throw [System.InvalidOperationException]::new($message)
        }
    }
    $Record.restoreRecords = @($records.ToArray())
    $Record.restoreSummary = [ordered]@{ restored=@($records | Where-Object { $_.verified }).Count; total=$records.Count }
}
function Invoke-Scenario($Scenario, [string]$Root, [string]$Editor, [string]$OutputRoot) {
    if ($null -eq $Scenario -or $Scenario -is [string] -or $null -eq (Get-Property $Scenario 'name')) {
        Fail 'Runner encountered a non-scenario entry; manifest validation leaked an object into the scenario list.'
    }
    $scenarioDir = Join-Path $OutputRoot ([string]$Scenario.name)
    New-Item -ItemType Directory -Path $scenarioDir -Force | Out-Null
    $logPath = Join-Path $Root 'igi1ed.log'
    $logOffset = if (Test-Path -LiteralPath $logPath) { (Get-Item -LiteralPath $logPath).Length } else { 0 }
    $process = $null; $latestScreenshot = $null; $screenshots = @{}; $steps = @()
    # Reversible-state working copy (snapshot bytes) plus the snapshot manifest.
    $workingCopyRoot = $null; $snapshotStaging = $null
    $scenarioStarted = [DateTime]::UtcNow.ToString('o')
    $inputScale = if ($null -ne $Scenario.inputScale) { [double]$Scenario.inputScale } else { 1.0 }
    $scenarioResult = 'PASS'; $scenarioFailure = $null
    $needsStaging = $false
    foreach ($step in @($Scenario.steps)) {
        if (@('snapshot_paths', 'restore_paths') -contains [string](Get-Property $step 'type')) { $needsStaging = $true }
    }
    if ($needsStaging) {
        $workingCopyRoot = New-WorkingCopyDirectory $Root
        $snapshotStaging = Join-Path $workingCopyRoot 'snapshots'
        New-Item -ItemType Directory -Path $snapshotStaging -Force | Out-Null
    }
    try {
        foreach ($step in @($Scenario.steps)) {
            $stepStart = [DateTime]::UtcNow
            $record = [ordered]@{ id=[string]$step.id; type=[string]$step.type; status='PASS'; started=$stepStart.ToString('o') }
            try {
                switch ([string]$step.type) {
                    'launch_editor' {
                        $level = [int]$step.level
                        $logOffset = if (Test-Path -LiteralPath $logPath) { (Get-Item -LiteralPath $logPath).Length } else { 0 }
                        $wmi = [wmiclass]'\\.\root\cimv2:Win32_Process'
                        $command = '"' + $Editor + '" --game-path "' + $Root + '" -level ' + $level
                        if ($null -ne $step.drawParts) { $command += ' -draw_parts ' + [int]$step.drawParts }
                        # Diagnostic sessions need load-time log evidence: developer
                        # mode forces INFO logging in-memory only, without
                        # touching the authored qedconfig.
                        if ($step.developerMode -eq $true) { $command += ' --developer-mode' }
                        $created = $wmi.Create($command, $Root)
                        if ([int]$created.ReturnValue -ne 0) { Fail "WMI launch failed with return code $($created.ReturnValue)." }
                        $process = Wait-ForEditor ([int]$created.ProcessId) 45
                        $record.pid = [int]$created.ProcessId; $record.command = $command
                    }
                    'wait_for_window' { $process = Wait-ForEditor $process.Id ([int]$(if ($step.timeoutSeconds) { $step.timeoutSeconds } else { 45 })) }
                    'assert_process' {
                        $process = Wait-ForEditor $process.Id 5
                        $record.sessionId = $process.SessionId
                        $record.workingSetMb = [Math]::Round($process.WorkingSet64 / 1MB, 2)
                        if ($record.workingSetMb -lt 30) { Fail "Editor working set is only $($record.workingSetMb) MB." }
                        for ($i = 0; $i -lt 10; $i++) {
                            try { $process.Refresh() } catch {}
                            if ($process.Responding) { break }
                            Start-Sleep -Milliseconds 500
                        }
                        $record.responding = $process.Responding
                        if (-not $process.Responding) { Fail "Editor PID $($process.Id) is not responding." }
                    }
                    'key' { Focus-Editor $process; Send-Key ([string]$step.key) }
                    'key_hold' { Focus-Editor $process; Send-KeyForDuration ([string]$step.key) ([double]$step.seconds) }
                    'click' { Focus-Editor $process; $right = ([string]$(if ($null -ne $step.button) { $step.button } else { 'left' })).ToLowerInvariant() -eq 'right'; [EditorE2E_Native]::Click([int]([double]$step.x * $inputScale),[int]([double]$step.y * $inputScale),$right) }
                    'select_graph_node' {
                        Focus-Editor $process
                        [EditorE2E_Native]::Click([int]([double]$step.x * $inputScale), [int]([double]$step.y * $inputScale), $false)
                        $record.nodeId = [int]$step.nodeId; $record.x = [int]$step.x; $record.y = [int]$step.y
                    }
                    'nudge_graph_node' {
                        Focus-Editor $process
                        [EditorE2E_Native]::Click([int]([double]$step.x * $inputScale), [int]([double]$step.y * $inputScale), $false)
                        $record.nodeId = [int]$step.nodeId; $record.field = [string]$step.field; $record.delta = [double]$step.delta
                    }
                    'start_animation' {
                        Focus-Editor $process
                        [EditorE2E_Native]::Click([int]([double]$step.x * $inputScale), [int]([double]$step.y * $inputScale), $false)
                        $record.animationId = if ($null -ne $step.animationId) { [int]$step.animationId } else { $null }
                        $record.x = [int]$step.x; $record.y = [int]$step.y
                    }
                    'pause_animation' {
                        Focus-Editor $process
                        [EditorE2E_Native]::Click([int]([double]$step.x * $inputScale), [int]([double]$step.y * $inputScale), $false)
                        $record.x = [int]$step.x; $record.y = [int]$step.y
                    }
                    'type_text' { Focus-Editor $process; Send-Text ([string]$step.text) }
                    'wait' { Start-Sleep -Milliseconds ([int]([double]$step.seconds * 1000)) }
                    'wait_for_log' {
                        $found = Wait-ForPattern $logPath $logOffset ([string]$step.pattern) ([int]$(if ($step.timeoutSeconds) { $step.timeoutSeconds } else { 30 }))
                        if ($null -eq $found) { Fail "Pattern not found in appended log: $($step.pattern)" }
                    }
                    'assert_log' {
                        $mustNotMatch = [bool]$(if ($null -ne $step.mustNotMatch) { $step.mustNotMatch } else { $false })
                        $timeout = [int]$(if ($step.timeoutSeconds) { $step.timeoutSeconds } else { 5 })
                        $deadline = [DateTime]::UtcNow.AddSeconds($timeout)
                        $matched = $false
                        do {
                            $text = Get-AppendedLog $logPath $logOffset
                            $matches = $text -match ([string]$step.pattern)
                            if (($mustNotMatch -and -not $matches) -or (-not $mustNotMatch -and $matches)) { $matched = $true; break }
                            Start-Sleep -Milliseconds 250
                        } while ([DateTime]::UtcNow -lt $deadline)
                        if (-not $matched) { Fail "Log assertion failed for pattern '$($step.pattern)'." }
                    }
                    'mark_log' {
                        $logOffset = if (Test-Path -LiteralPath $logPath) { (Get-Item -LiteralPath $logPath).Length } else { 0 }
                        $record.logOffset = $logOffset
                    }
                    'assert_file' {
                        $filePath = Assert-UnderRoot (Join-Path $Root ([string]$step.path)) $Root 'assert_file path'
                        $mustNotMatch = [bool]$(if ($null -ne $step.mustNotMatch) { $step.mustNotMatch } else { $false })
                        $timeout = [int]$(if ($step.timeoutSeconds) { $step.timeoutSeconds } else { 5 })
                        if (-not (Wait-ForFilePattern $filePath ([string]$step.pattern) $mustNotMatch $timeout)) { Fail "File assertion failed for '$($step.path)'." }
                    }
                    'assert_path' {
                        $path = Assert-UnderRoot (Join-Path $Root ([string]$step.path)) $Root 'assert_path path'
                        $kind = [string]$(if ($null -ne $step.kind) { $step.kind } else { 'file' })
                        $minBytes = [int]$(if ($null -ne $step.minBytes) { $step.minBytes } else { 0 })
                        $timeout = [int]$(if ($step.timeoutSeconds) { $step.timeoutSeconds } else { 5 })
                        $deadline = [DateTime]::UtcNow.AddSeconds($timeout); $valid = $false
                        do {
                            if ($kind.ToLowerInvariant() -eq 'directory') {
                                $valid = Test-Path -LiteralPath $path -PathType Container
                            } elseif (Test-Path -LiteralPath $path -PathType Leaf) {
                                $valid = ((Get-Item -LiteralPath $path).Length -ge $minBytes)
                            }
                            if (-not $valid) { Start-Sleep -Milliseconds 100 }
                        } while (-not $valid -and [DateTime]::UtcNow -lt $deadline)
                        if (-not $valid) { Fail "Path assertion failed for '$($step.path)' (kind=$kind, minBytes=$minBytes)." }
                        $record.path = $path; $record.kind = $kind; $record.bytes = if ($kind.ToLowerInvariant() -eq 'file') { (Get-Item -LiteralPath $path).Length } else { $null }
                    }
                    'assert_cursor_visible' {
                        if (-not [EditorE2E_Native]::CursorVisible()) { Fail 'Mouse cursor is not visible.' }
                        $record.cursorVisible = $true
                    }
                    'assert_cursor_hidden' {
                        if ([EditorE2E_Native]::CursorVisible()) { Fail 'Native mouse cursor is still visible.' }
                        $record.cursorVisible = $false
                    }
                    'orbit_camera' {
                        if ($null -eq $process) { Fail 'orbit_camera requires a live editor process (run launch_editor first).' }
                        $angle = [string]$step.angle; $distance = [double]$step.distance
                        $pixels = [int]$(if ($null -ne $step.pixels) { $step.pixels } else { 12 })
                        $orbit = Invoke-OrbitCamera $process $angle $pixels $distance
                        $record.angle = $angle; $record.distance = $distance
                        $record.sequence = $orbit.sequence; $record.centerX = $orbit.centerX; $record.centerY = $orbit.centerY
                        if ($null -ne $step.screenshotAfter) {
                            Focus-Editor $process
                            $file = Join-Path $scenarioDir (([string]$step.screenshotAfter) + '.png')
                            $latestScreenshot = Capture-Screenshot $file $step.region
                            $screenshots[[string]$step.screenshotAfter] = $latestScreenshot
                            $record.screenshot = $file
                        }
                    }
                    'capture_window_state' {
                        $state = Invoke-CaptureWindowState $process
                        $record.pid = $state.pid; $record.sessionId = $state.sessionId
                        $record.responding = $state.responding; $record.workingSetMb = $state.workingSetMb
                        $record.clientBounds = $state.clientBounds
                    }
                    'assert_file_hash' {
                        $filePath = Assert-UnderRoot (Join-Path $Root ([string]$step.path)) $Root 'assert_file_hash path'
                        $expected = ([string]$step.sha256).ToLowerInvariant()
                        $timeout = [int]$(if ($step.timeoutSeconds) { $step.timeoutSeconds } else { 5 })
                        $waited = Get-HashWaitResult $filePath $expected $timeout
                        $record.path = $filePath; $record.expected = $expected; $record.actual = $waited.hash; $record.present = $waited.present
                        if (-not $waited.matched) { Fail "File hash assertion failed for '$($step.path)' (expected $expected, actual $($waited.hash))." }
                    }
                    'assert_log_count' {
                        $timeout = [int]$(if ($step.timeoutSeconds) { $step.timeoutSeconds } else { 5 })
                        $deadline = [DateTime]::UtcNow.AddSeconds($timeout)
                        $count = -1
                        do {
                            $text = Get-AppendedLog $logPath $logOffset
                            $count = @([regex]::Matches($text, ([string]$step.pattern))).Count
                            if ($count -ge [int]$step.min) { break }
                            Start-Sleep -Milliseconds 250
                        } while ([DateTime]::UtcNow -lt $deadline)
                        $record.count = $count
                        if ($count -lt [int]$step.min) { Fail "Log count for '$($step.pattern)' was $count, below min $($step.min)." }
                        if ($null -ne $step.max -and $count -gt [int]$step.max) { Fail "Log count for '$($step.pattern)' was $count, above max $($step.max)." }
                    }
                    'assert_graph_edit' {
                        Assert-GraphEdit $step $Root $snapshotStaging $record
                    }
                    'snapshot_paths' {
                        $paths = Resolve-DeclaredPaths (Get-Property $step 'paths') $Root
                        $snapshots = @()
                        $missing = @()
                        foreach ($resolved in $paths) {
                            if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
                                $missing += $resolved
                                continue
                            }
                            $sha = Get-Sha256 $resolved
                            $relative = Get-RelativeToRoot $resolved $Root
                            $staged = Join-Path $snapshotStaging (Get-StagingFileName $relative)
                            Copy-Item -LiteralPath $resolved -Destination $staged -Force
                            $snapshots += [ordered]@{ path=$relative; sha256=$sha }
                        }
                        if ($missing.Count -gt 0) { Fail "snapshot_paths could not hash declared file(s): $($missing -join ', ')" }
                        if ($snapshots.Count -eq 0) { Fail 'snapshot_paths found no existing declared file to snapshot.' }
                        @($snapshots) | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $snapshotStaging 'snapshot.json') -Encoding UTF8
                        $record.snapshotCount = @($snapshots).Count
                        $record.snapshots = @($snapshots)
                    }
                    'restore_paths' {
                        if ($null -eq $snapshotStaging -or -not (Test-Path -LiteralPath (Join-Path $snapshotStaging 'snapshot.json') -PathType Leaf)) {
                            Fail 'restore_paths has no snapshot to restore; run snapshot_paths on the same paths first.'
                        }
                        if ($null -ne $step.paths) {
                            # Optional declared subset must match the snapshot set exactly.
                            $declared = Resolve-DeclaredPaths (Get-Property $step 'paths') $Root
                            $snapshotManifest = Get-Content -LiteralPath (Join-Path $snapshotStaging 'snapshot.json') -Raw | ConvertFrom-Json
                            foreach ($snapshot in @($snapshotManifest)) {
                                $snapshotResolved = Assert-UnderRoot (Join-Path $Root ([string]$snapshot.path)) $Root 'restore subset path'
                                $matches = @($declared | Where-Object { $_.TrimEnd('\') -eq $snapshotResolved.TrimEnd('\') })
                                if ($matches.Count -ne 1) { Fail "restore_paths declared subset must cover exactly the snapshot paths; '$($snapshot.path)' was not matched once." }
                            }
                        }
                        Invoke-RestorePaths $record $Root $scenarioDir $snapshotStaging $process
                    }
                    'screenshot' {
                        Focus-Editor $process
                        $file = Join-Path $scenarioDir (([string]$step.name) + '.png')
                        $captureWindow = if ($step.client) { Get-EditorWindowHandle $process } else { [IntPtr]::Zero }
                        $latestScreenshot = Capture-Screenshot $file $step.region $captureWindow
                        $screenshots[[string]$step.name] = $latestScreenshot
                        $record.screenshot = $file
                        $record.captureBounds = $latestScreenshot
                    }
                    'assert_screenshot_region' {
                        if ($null -eq $latestScreenshot) { Fail 'Image assertion requires a preceding screenshot step.' }
                        $metrics = Get-ImageMetrics $latestScreenshot.path $step.region; $record.metrics = $metrics
                        if ($null -ne $step.minNonBlackRatio -and $metrics.nonBlackRatio -lt [double]$step.minNonBlackRatio) { Fail "nonBlackRatio $($metrics.nonBlackRatio) < $($step.minNonBlackRatio)." }
                        if ($null -ne $step.minUniqueRatio -and $metrics.uniqueRatio -lt [double]$step.minUniqueRatio) { Fail "uniqueRatio $($metrics.uniqueRatio) < $($step.minUniqueRatio)." }
                        if ($null -ne $step.minMeanLuma -and $metrics.meanLuma -lt [double]$step.minMeanLuma) { Fail "meanLuma $($metrics.meanLuma) < $($step.minMeanLuma)." }
                        if ($null -ne $step.maxMeanLuma -and $metrics.meanLuma -gt [double]$step.maxMeanLuma) { Fail "meanLuma $($metrics.meanLuma) > $($step.maxMeanLuma)." }
                    }
                    'assert_screenshot_color_ratio' {
                        if ($null -eq $latestScreenshot) { Fail 'Color assertion requires a preceding screenshot step.' }
                        $tolerance = [int]$(if ($null -ne $step.tolerance) { $step.tolerance } else { 12 })
                        $metrics = Get-ImageColorRatio $latestScreenshot.path $step.region $step.rgb $tolerance
                        $record.metrics = $metrics
                        if ($null -ne $step.minRatio -and $metrics.ratio -lt [double]$step.minRatio) { Fail "color ratio $($metrics.ratio) < $($step.minRatio)." }
                        if ($null -ne $step.maxRatio -and $metrics.ratio -gt [double]$step.maxRatio) { Fail "color ratio $($metrics.ratio) > $($step.maxRatio)." }
                    }
                    'assert_screenshot_difference' {
                        $fromName = [string]$step.from; $toName = [string]$step.to
                        if (-not $screenshots.ContainsKey($fromName) -or -not $screenshots.ContainsKey($toName)) { Fail "Difference assertion references unknown screenshots '$fromName' or '$toName'." }
                        $threshold = [int]$(if ($null -ne $step.changedPixelThreshold) { $step.changedPixelThreshold } else { 12 })
                        $metrics = Get-ImageDifferenceMetrics $screenshots[$fromName].path $screenshots[$toName].path $step.region $threshold
                        $record.metrics = $metrics
                        if ($null -ne $step.minChangedRatio -and $metrics.changedRatio -lt [double]$step.minChangedRatio) { Fail "changedRatio $($metrics.changedRatio) < $($step.minChangedRatio)." }
                        if ($null -ne $step.maxChangedRatio -and $metrics.changedRatio -gt [double]$step.maxChangedRatio) { Fail "changedRatio $($metrics.changedRatio) > $($step.maxChangedRatio)." }
                        if ($null -ne $step.minMeanAbsDifference -and $metrics.meanAbsDifference -lt [double]$step.minMeanAbsDifference) { Fail "meanAbsDifference $($metrics.meanAbsDifference) < $($step.minMeanAbsDifference)." }
                        if ($null -ne $step.maxMeanAbsDifference -and $metrics.meanAbsDifference -gt [double]$step.maxMeanAbsDifference) { Fail "meanAbsDifference $($metrics.meanAbsDifference) > $($step.maxMeanAbsDifference)." }
                    }
                    'close_editor' {
                        $force = [bool]$(if ($null -ne $step.force) { $step.force } else { $false })
                        $closed = Close-Editor $process $force
                        if ($null -ne $closed) {
                            $record.exited = $closed.exited; $record.exitCode = $closed.exitCode; $record.forced = $closed.forced
                            if (-not $force -and $closed.exited -and $null -ne $closed.exitCode -and $closed.exitCode -ne 0) { Fail "Editor exited with code $($closed.exitCode) during graceful close." }
                        }
                        $process = $null
                    }
                }
            } catch {
                $record.status='FAIL'; $record.error=$_.Exception.Message; throw
            } finally {
                $record.finished=[DateTime]::UtcNow.ToString('o'); $record.durationMs=[int](([DateTime]::UtcNow-$stepStart).TotalMilliseconds); $steps += [pscustomobject]$record
            }
        }
    } catch {
        $scenarioResult='FAIL'; $scenarioFailure=$_.Exception.Message
        if ($null -ne $process) {
            try { Focus-Editor $process; $latestScreenshot = Capture-Screenshot (Join-Path $scenarioDir 'failure.png') $null } catch {}
        } else {
            try { $latestScreenshot = Capture-Screenshot (Join-Path $scenarioDir 'failure.png') $null } catch {}
        }
    } finally {
        if (-not $KeepEditorOpen) { $discardedCloseResult = Close-Editor $process $true }
        # Mandatory restoration: even when a later step failed, restore declared
        # files byte-for-byte and verify each restored SHA-256 against its
        # snapshot; a mismatch or missing snapshot is recorded on the run.
        $restoreRan = $false; $restoreError = $null
        if ($null -ne $snapshotStaging -and (Test-Path -LiteralPath (Join-Path $snapshotStaging 'snapshot.json') -PathType Leaf)) {
            try {
                $restoreRecord = [ordered]@{ id='__auto_restore'; type='restore_paths'; status='PASS'; started=[DateTime]::UtcNow.ToString('o'); restoreRecords=@() }
                Invoke-RestorePaths $restoreRecord $Root $scenarioDir $snapshotStaging $process
                $restoreRecord.finished=[DateTime]::UtcNow.ToString('o')
                $steps += [pscustomobject]$restoreRecord
                $restoreRan = $true
            } catch {
                $restoreError = $_.Exception.Message
                $steps += [pscustomobject]@{ id='__auto_restore'; type='restore_paths'; status='FAIL'; error=$restoreError; started=[DateTime]::UtcNow.ToString('o'); finished=[DateTime]::UtcNow.ToString('o') }
            }
        }
        if ($null -ne $workingCopyRoot) { Remove-Item -LiteralPath $workingCopyRoot -Recurse -Force -ErrorAction SilentlyContinue }
        if ($restoreError) {
            $scenarioResult = 'FAIL'
            if ($scenarioFailure) { $scenarioFailure += '; ' }
            $scenarioFailure = [string]$scenarioFailure + "Restoration failed: $restoreError"
        }
    }
    $result = [ordered]@{ name=[string]$Scenario.name; level=[int]$Scenario.level; status=$scenarioResult; started=$scenarioStarted; finished=[DateTime]::UtcNow.ToString('o'); logPath=$logPath; logOffset=$logOffset; failure=$scenarioFailure; restored=$restoreRan; steps=$steps }
    $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $scenarioDir 'scenario.json') -Encoding UTF8
    return [pscustomobject]$result
}
try {
    $runStarted = [DateTime]::UtcNow.ToString('o')
    if ([string]::IsNullOrWhiteSpace($ScenarioPath)) { $ScenarioPath = Join-Path $PSScriptRoot 'scenarios\editor-regression.json' }
    if ([string]::IsNullOrWhiteSpace($ArtifactsRoot)) { $ArtifactsRoot = Join-Path (Join-Path $PSScriptRoot '..\..\\artifacts') ('e2e\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
    $ScenarioPath = Get-FullPath $ScenarioPath
    if (-not (Test-Path -LiteralPath $ScenarioPath)) { Fail "Scenario manifest not found: $ScenarioPath" }
    $manifest = Get-Content -LiteralPath $ScenarioPath -Raw | ConvertFrom-Json
    $GameRoot = Get-FullPath $GameRoot
    if (-not (Test-Path -LiteralPath $GameRoot)) { Fail "Game root not found: $GameRoot" }
    $scenarios = Validate-Manifest $manifest $GameRoot
    $declaredCount = @($manifest.scenarios).Count
    if (@($scenarios).Count -ne $declaredCount) {
        Fail "Manifest validation returned $(@($scenarios).Count) scenarios but the manifest declares $declaredCount; a validation step leaked a value into the scenario list."
    }
    if ($ValidateOnly) { Write-Host "Validated $(@($scenarios).Count) editor E2E scenarios."; exit 0 }
    if (-not [string]::IsNullOrWhiteSpace($ScenarioName)) {
        $scenarios = @($scenarios | Where-Object { $_.name -like $ScenarioName })
        if ($scenarios.Count -eq 0) { Fail "No scenario matched '$ScenarioName'." }
    }
    foreach ($scenario in @($scenarios)) {
        if ([bool](Get-Property $scenario 'requiresMutation') -and -not $AllowGameDataMutation) {
            Fail "Scenario '$($scenario.name)' requires -AllowGameDataMutation."
        }
    }
    if ([string]::IsNullOrWhiteSpace($EditorPath)) { $EditorPath = Join-Path $GameRoot 'igi1ed.exe' }
    $EditorPath = Assert-UnderRoot $EditorPath $GameRoot 'Editor path'
    if (-not (Test-Path -LiteralPath $EditorPath)) { Fail "Editor executable not found: $EditorPath" }
    $ArtifactsRoot = Get-FullPath $ArtifactsRoot
    New-Item -ItemType Directory -Path $ArtifactsRoot -Force | Out-Null
    Copy-Item -LiteralPath $ScenarioPath -Destination (Join-Path $ArtifactsRoot 'scenario-manifest.json') -Force
    $results = @()
    foreach ($scenario in @($scenarios)) { Write-Host "Running $($scenario.name) (Level $($scenario.level))..."; $results += Invoke-Scenario $scenario $GameRoot $EditorPath $ArtifactsRoot }
    $report = [ordered]@{ manifest=$ScenarioPath; gameRoot=$GameRoot; editorPath=$EditorPath; started=$runStarted; finished=[DateTime]::UtcNow.ToString('o'); results=$results; passed=@($results|Where-Object status -eq 'PASS').Count; failed=@($results|Where-Object status -eq 'FAIL').Count }
    $report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $ArtifactsRoot 'run.json') -Encoding UTF8
    $lines = @('# Editor E2E report','',"- Game root: $GameRoot","- Editor: $EditorPath",'','| Scenario | Level | Status | Failure |','|---|---:|---|---|')
    foreach ($item in $results) { $lines += "| $($item.name) | $($item.level) | $($item.status) | $([string]$item.failure -replace '\|','/') |" }
    $lines | Set-Content -LiteralPath (Join-Path $ArtifactsRoot 'run.md') -Encoding UTF8
    if ($report.failed -gt 0) { exit 1 }
    exit 0
} catch { Write-Error $_.Exception.Message; exit 1 }
