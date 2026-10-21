#requires -Version 5.1
$ErrorActionPreference='Stop'
$source=Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'Tools/Collectors/Collect-AudioDriverPackages.ps1'
$errors=$null; $tokens=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
# Load only production function definitions. Never run live collection in tests.
foreach ($statement in $ast.EndBlock.Statements) {
    if ($statement -is [Management.Automation.Language.FunctionDefinitionAst]) { . ([scriptblock]::Create($statement.Extent.Text)) }
}
function Assert($Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Node([string]$Id,[string]$Class,[string]$Name) { [pscustomobject]@{InstanceId=$Id;Class=$Class;FriendlyName=$Name} }
function Properties([string]$Id,[string[]]$Hardware,[string]$Inf,[string]$Parent='') {
    $script:PropertyCache[$Id]=@(
        [pscustomobject]@{KeyName='DEVPKEY_Device_HardwareIds';Data=$Hardware},
        [pscustomobject]@{KeyName='DEVPKEY_Device_DriverInfPath';Data=$Inf},
        [pscustomobject]@{KeyName='DEVPKEY_Device_Parent';Data=$Parent})
}
$base=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
$temp=[IO.Path]::GetFullPath((Join-Path $base ('cirrus-collector-test-'+[guid]::NewGuid().ToString('N'))))
Assert ($temp.StartsWith($base,[StringComparison]::OrdinalIgnoreCase)) 'Unsafe temporary path'
$null=New-Item -ItemType Directory -Path $temp
try {
    $script:Root=Join-Path $temp 'report'; $null=New-Item -ItemType Directory -Path $script:Root
    $null=New-Item -ItemType Directory -Path (Join-Path $script:Root 'payload')
    $script:Status=New-Object 'System.Collections.Generic.List[object]'
    $script:PropertyCache=@{}; $script:Nodes=@{}; $script:Selected=@{}; $script:Packages=@{}
    $sound=New-IdSet
    Assert (Test-AudioSeed (Node 'ACPI\CSC3551\1' 'System' 'Amplifier') $sound) 'AMP seed missing'
    Assert (Test-AudioSeed (Node 'HDAUDIO\FUNC_01&VEN_10EC\1' 'MEDIA' 'Codec') $sound) 'Codec seed missing'
    Assert (Test-AudioSeed (Node 'SWD\MMDEVAPI\MIC' 'AudioEndpoint' 'Microphone') $sound) 'Capture endpoint missing'
    Assert (-not (Test-AudioSeed (Node 'SWC\GPU' 'SoftwareComponent' 'Realtek unrelated component') $sound)) 'Blanket SoftwareComponent selection'
    Assert (-not (Test-AudioSeed (Node 'USB\VIDEO' 'MEDIA' 'USB Video Capture') $sound)) 'Video-only device selected'
    @'
[Version]
Class=MEDIA
[Manufacturer]
%Mfg%=Devices,NTamd64.10.0
[Devices.NTamd64.10.0]
%Desc%=Install,%AmpId%
[Install.Components]
AddComponent=AudioFx,,FxInstall
[FxInstall]
ComponentIDs=SWC\VEN_TEST&COMP_AUDIO
[Strings]
Mfg="OEM"
Desc="Audio, amp; description"
AmpId="ACPI\CSC3551"
'@ | Set-Content -LiteralPath (Join-Path $temp 'oem1.inf') -Encoding Unicode
    @'
[Version]
Class=Extension
ExtensionId={11111111-1111-1111-1111-111111111111}
[Manufacturer]
%M%=Models,NTamd64
[Models.NTamd64]
%D%=Install,ACPI\CSC3551
'@ | Set-Content -LiteralPath (Join-Path $temp 'oem2.inf') -Encoding UTF8
    @'
[Version]
Class=Extension
[Manufacturer]
Cirrus=Models,NTamd64
[Models.NTamd64]
"Same vendor, unrelated device"=Install,PCI\VEN_1234&DEV_BEEF
'@ | Set-Content -LiteralPath (Join-Path $temp 'oem3.inf') -Encoding ASCII
    $docs=@(1..3 | ForEach-Object {Read-InfDocument (Join-Path $temp "oem$_.inf")})
    Assert ($docs[0].Ids.Contains('ACPI\CSC3551')) 'INF string/model decoration not decoded'
    Assert ($docs[0].Components.Contains('SWC\VEN_TEST&COMP_AUDIO')) 'ComponentIDs missing'
    $items=@((Node 'ACPI\CSC3551\1' 'System' 'Cirrus'),(Node 'SWC\FX\1' 'SoftwareComponent' 'Audio effects'),
        (Node 'SWC\UNRELATED\1' 'SoftwareComponent' 'Other'),(Node 'PCI\ROOT\0' 'System' 'PCI root'))
    foreach ($node in $items) {$script:Nodes[$node.InstanceId]=$node}
    Properties 'ACPI\CSC3551\1' @('ACPI\CSC3551') 'oem1.inf' 'PCI\ROOT\0'
    Properties 'SWC\FX\1' @('SWC\VEN_TEST&COMP_AUDIO') 'oem4.inf' 'UNRELATED_PARENT'
    Properties 'SWC\UNRELATED\1' @('SWC\GPU') 'oem99.inf' 'PCI\ROOT\0'
    Properties 'PCI\ROOT\0' @('PCI\VEN_1234&DEV_BEEF') 'oem100.inf'
    $null=Add-SelectedDevice 'ACPI\CSC3551\1' 'Audio' 'Fixture amp'
    $null=Add-SelectedDevice 'PCI\ROOT\0' 'Ancestor' 'Topology only'
    Resolve-AudioSelection $docs
    Assert ($script:Packages.ContainsKey('oem1.inf') -and $script:Packages.ContainsKey('oem2.inf') -and $script:Packages.ContainsKey('oem4.inf')) 'Audio/extension/component closure incomplete'
    Assert (-not $script:Packages.ContainsKey('oem3.inf')) 'Ancestor hardware ID selected unrelated extension'
    Assert (-not $script:Packages.ContainsKey('oem99.inf')) 'Unrelated software component exported'
    Assert (-not $script:Packages.ContainsKey('oem100.inf')) 'Ancestor chipset package exported'
    $first=Join-Path $temp 'first.bin';$duplicate=Join-Path $temp 'duplicate.bin';$different=Join-Path $temp 'different.sys'
    [IO.File]::WriteAllBytes($first,[byte[]](1,2,3));[IO.File]::WriteAllBytes($duplicate,[byte[]](1,2,3));[IO.File]::WriteAllBytes($different,[byte[]](4,5,6,7))
    $script:PayloadPlan=New-Object 'System.Collections.Generic.List[object]'
    Add-PayloadFile $first 'oem1.inf' 'first.bin';Add-PayloadFile $duplicate 'oem2.inf' 'other-name.bin';Add-PayloadFile $different 'oem1.inf' 'different.sys'
    Save-PayloadPlan 100 $true
    Assert (@(Get-ChildItem (Join-Path $script:Root 'payload') -File).Count -eq 2) 'Payload copied more than once'
    Assert ($script:PayloadPlan[0].Blob -eq $script:PayloadPlan[1].Blob) 'Duplicate references disagree'
    Assert (@($script:PayloadPlan | Where-Object State -NE 'COPIED').Count -eq 0) 'Copied status incorrect'
    $budgetRoot=Join-Path $temp 'budget';$null=New-Item -ItemType Directory -Path (Join-Path $budgetRoot 'payload') -Force
    $script:Root=$budgetRoot;$script:PayloadPlan=New-Object 'System.Collections.Generic.List[object]'
    Add-PayloadFile $first 'oem1.inf' 'first.bin';Save-PayloadPlan 2 $true
    Assert (@(Get-ChildItem (Join-Path $budgetRoot 'payload') -File).Count -eq 0) 'Budget failure copied partial payload'
    Assert ($script:PayloadPlan[0].State -eq 'OVER_BUDGET') 'Budget loss hidden'
    $log=Join-Path $temp 'setupapi.log';$extract=Join-Path $temp 'audio.log'
    @'
>>> Device Install ACPI\CSC3551\10
UNRELATED_PREFIX_INSTANCE
<<< Section end
>>>  [Device Install (Hardware initiated) - ACPI\CSC3551\1]
>>>  Section start 2026/10/17 12:00:00.000
KEEP_AUDIO_DETAIL
<<<  Section end 2026/10/17 12:00:00.100
<<<  [Exit status: SUCCESS]
>>> Device Install GPU
UNRELATED_GPU
<<< Section end
'@ | Set-Content -LiteralPath $log -Encoding Unicode
    Export-SetupApiSections $log @('ACPI\CSC3551\1') $extract
    $text=[IO.File]::ReadAllText($extract)
    Assert ($text.Contains('KEEP_AUDIO_DETAIL') -and -not $text.Contains('UNRELATED_')) 'SetupAPI exact section filter failed'
    Assert ($text.Contains('Section start') -and $text.Contains('[Exit status: SUCCESS]')) 'SetupAPI timestamps/end marker lost'
    $script:Root=Join-Path $temp 'report'
    Write-Manifest
    Write-Manifest
    $manifest=@(Import-Csv -LiteralPath (Join-Path $script:Root 'manifest-sha256.csv'))
    Assert ($manifest.Count -gt 0 -and -not @($manifest | Where-Object Path -EQ 'manifest-sha256.csv').Count) 'Manifest includes itself'
    Write-AudioZip $script:Root
    Assert (Test-Path -LiteralPath ($script:Root+'.zip')) 'Archive missing'
    $raw=[IO.File]::ReadAllText($source)
    Assert ($raw -notmatch 'ExportAllDrivers|AcpiDumpPath|EnumSystemFirmwareTables|GetSystemFirmwareTable|store-fallback|all-oem') 'Broad export or table collection returned'
    Assert ($raw -notmatch "'/export-driver'|driverquery.exe|wevtutil.exe") 'Global dump command returned'
    # Compile the P/Invoke declaration without reading firmware or changing a token.
    $code=$ast.Find({param($node) $node -is [Management.Automation.Language.StringConstantExpressionAst] -and $node.Value -like '*public static class WindowsAudioCalibrationReader*'},$true)
    Assert ($null -ne $code) 'Named calibration reader missing'
    Add-Type -TypeDefinition $code.Value
    Write-Output 'PASS Windows collector: exact device/INF closure, unrelated package exclusion, deduplication, payload budget, log filtering, ZIP and PS syntax'
} finally {
    $resolved=[IO.Path]::GetFullPath($temp)
    if ($resolved.StartsWith($base,[StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf) -like 'cirrus-collector-test-*') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
