#requires -Version 5.1
<#
Windows audio evidence collector. The historical filename is retained for callers;
selection is based on installed devices and INF relationships, not a laptop model.
Reads devices, packages and registry; writes only its output. No ACPI table dump,
driver installation, device restart, registry modification or network upload.
#>
[CmdletBinding()]
param(
    [string]$OutputRoot = "$env:PUBLIC\Documents",
    [ValidateRange(1,30)][int]$EventDays = 3,
    [ValidateRange(64,8192)][int]$MaxPayloadMB = 512,
    [string]$SpeakerTestNotes = 'NOT PROVIDED',
    [switch]$PlanOnly,
    [switch]$NoZip
)
$ErrorActionPreference = 'Stop'

function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
function Write-Manifest {
    $destination = Join-Path $script:Root 'manifest-sha256.csv'
    # Materialize before opening the output, so the manifest never hashes itself.
    $files = @(Get-ChildItem -LiteralPath $script:Root -File -Recurse | Where-Object FullName -NE $destination)
    $files | ForEach-Object {
        [pscustomobject]@{Path=$_.FullName.Substring($script:Root.Length+1);Bytes=$_.Length;SHA256=(Get-Sha256 $_.FullName)}
    } | Export-Csv -LiteralPath $destination -NoTypeInformation -Encoding UTF8
}
function New-IdSet {
    return ,(New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase))
}
function Add-Status([string]$Task,[string]$State,[string]$Detail) {
    $script:Status.Add([pscustomobject]@{Task=$Task;State=$State;Detail=$Detail})
    Write-Host "[$State] $Task $Detail"
}
function Write-Json($Value,[string]$Relative) {
    ConvertTo-Json -InputObject $Value -Depth 16 | Set-Content -LiteralPath (Join-Path $script:Root $Relative) -Encoding UTF8
}
function Invoke-Step([string]$Name,[scriptblock]$Action) {
    try { & $Action; Add-Status $Name 'OK' '' }
    catch { Add-Status $Name 'FAILED' $_.Exception.Message }
}
function Invoke-Native([string]$Exe,[string[]]$Arguments,[string]$Relative) {
    $old = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $Exe @Arguments 2>&1)
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    @("Command: $Exe $($Arguments -join ' ')","Exit: $code") + $output |
        Out-File -LiteralPath (Join-Path $script:Root $Relative) -Encoding UTF8 -Width 4096
    if ($code -ne 0) { throw "Exit $code; see $Relative" }
}
function Get-NodeProperties([string]$Id) {
    if (-not $script:PropertyCache.ContainsKey($Id)) {
        try { $script:PropertyCache[$Id] = @(Get-PnpDeviceProperty -InstanceId $Id -ErrorAction Stop) }
        catch { $script:PropertyCache[$Id] = @(); Add-Status 'PnP properties' 'MISSING' "$Id : $($_.Exception.Message)" }
    }
    return $script:PropertyCache[$Id]
}
function Get-NodeValue([string]$Id,[string]$Key) {
    foreach ($property in @(Get-NodeProperties $Id)) {
        if ($property.KeyName -eq $Key) { return $property.Data }
    }
}
function Test-AudioSeed($Device,$SoundIds) {
    $id = [string]$Device.InstanceId
    return $SoundIds.Contains($id) -or $Device.Class -eq 'AudioEndpoint' -or
        $id -match '^(HDAUDIO|INTELAUDIO)\\' -or
        $id -match '^ACPI\\(?:VEN_CSC&DEV_35|CSC35|CLSA010)' -or
        ($Device.Class -eq 'MEDIA' -and $Device.FriendlyName -match 'Audio|Sound|Codec|Cirrus|CS35')
}
function Add-SelectedDevice([string]$Id,[string]$Role,[string]$Reason) {
    if (-not $script:Nodes.ContainsKey($Id)) { return $false }
    if (-not $script:Selected.ContainsKey($Id)) {
        $script:Selected[$Id] = [pscustomobject]@{InstanceId=$Id;Role=$Role;Reasons=(New-IdSet)}
    } elseif ($script:Selected[$Id].Role -eq 'Ancestor' -and $Role -ne 'Ancestor') {
        $script:Selected[$Id].Role = $Role
    }
    $null = $script:Selected[$Id].Reasons.Add($Reason)
    return $true
}
function Split-InfValues([string]$Text) {
    # INF quoted commas and quoted semicolons must not become separators.
    $parts = [regex]::Split($Text, ',(?=(?:[^"]*"[^"]*")*[^"]*$)')
    foreach ($part in $parts) { $part.Trim().Trim('"') }
}
function Expand-InfValue([string]$Text,$Strings) {
    for ($iteration=0; $iteration -lt 4; $iteration++) {
        $before = $Text
        foreach ($match in @([regex]::Matches($Text,'%([^%]+)%'))) {
            if ($Strings.ContainsKey($match.Groups[1].Value)) {
                $Text = $Text.Replace($match.Value,[string]$Strings[$match.Groups[1].Value])
            }
        }
        if ($before -eq $Text) { break }
    }
    return $Text
}
function Read-InfDocument([string]$Path) {
    $sections = @{}; $section = ''; $pending = ''
    foreach ($raw in [IO.File]::ReadAllLines($Path,[Text.Encoding]::Default)) {
        $line = [regex]::Replace($raw,';(?=(?:[^"]*"[^"]*")*[^"]*$).*$', '').Trim()
        if (-not $line) { continue }
        if ($line.EndsWith('\')) { $pending += $line.Substring(0,$line.Length-1); continue }
        $line = $pending + $line; $pending = ''
        if ($line -match '^\[([^\]]+)\]$') {
            $section = $Matches[1]
            if (-not $sections.ContainsKey($section)) { $sections[$section] = New-Object 'System.Collections.Generic.List[string]' }
        } elseif ($section) { $sections[$section].Add($line) }
    }
    $strings = @{}
    if ($sections.ContainsKey('Strings')) {
        foreach ($line in $sections['Strings']) {
            if ($line -match '^([^=]+)=(.*)$') { $strings[$Matches[1].Trim()] = $Matches[2].Trim().Trim('"') }
        }
    }
    $ids = New-IdSet; $components = New-IdSet; $includes = New-IdSet
    $modelSections = New-IdSet; $class = ''; $extension = ''; $version = ''
    if ($sections.ContainsKey('Manufacturer')) {
        foreach ($line in $sections['Manufacturer']) {
            if ($line -notmatch '^[^=]+=(.*)$') { continue }
            $parts = @(Split-InfValues (Expand-InfValue $Matches[1] $strings))
            if (-not $parts.Count) { continue }
            $null = $modelSections.Add($parts[0])
            foreach ($decoration in @($parts | Select-Object -Skip 1)) { $null = $modelSections.Add($parts[0]+'.'+$decoration) }
        }
    }
    foreach ($name in $sections.Keys) {
        foreach ($line in $sections[$name]) {
            if ($line -notmatch '^([^=]+)=(.*)$') { continue }
            $key = $Matches[1].Trim(); $value = Expand-InfValue $Matches[2].Trim() $strings
            if ($name -eq 'Version') {
                if ($key -eq 'Class') { $class = $value.Trim('"') }
                if ($key -eq 'ExtensionId') { $extension = $value.Trim('"') }
                if ($key -eq 'DriverVer') { $version = $value }
            }
            if ($modelSections.Contains($name)) {
                foreach ($id in @(Split-InfValues $value | Select-Object -Skip 1)) {
                    if ($id -match '^[A-Z][A-Z0-9_]*\\[^%]+$') { $null = $ids.Add($id) }
                }
            }
            if ($key -eq 'ComponentIDs') {
                foreach ($id in @(Split-InfValues $value)) { $null = $components.Add($id) }
            }
            if ($key -eq 'Include') {
                foreach ($inf in @(Split-InfValues $value)) {
                    if ($inf -match '^oem\d+\.inf$') { $null = $includes.Add($inf) }
                }
            }
        }
    }
    return [pscustomobject]@{Name=[IO.Path]::GetFileName($Path);Path=$Path;Class=$class;ExtensionId=$extension;
        DriverVer=$version;Ids=$ids;Components=$components;Includes=$includes}
}
function Find-IdIntersection($Left,$Right) {
    foreach ($id in $Left) { if ($Right.Contains([string]$id)) { return [string]$id } }
    return $null
}
function Add-Package([string]$Inf,[string]$Reason) {
    $Inf = [IO.Path]::GetFileName($Inf)
    if ($Inf -notmatch '^oem\d+\.inf$') { return $false }
    $added = -not $script:Packages.ContainsKey($Inf)
    if ($added) { $script:Packages[$Inf] = [pscustomobject]@{Inf=$Inf;Reasons=(New-IdSet)} }
    $null = $script:Packages[$Inf].Reasons.Add($Reason)
    return $added
}
function Get-SelectedHardwareIds {
    $result = New-IdSet
    foreach ($entry in $script:Selected.Values) {
        if ($entry.Role -eq 'Ancestor') { continue }
        foreach ($id in @(Get-NodeValue $entry.InstanceId 'DEVPKEY_Device_HardwareIds')) {
            if ($id -and $id -notmatch '\\(?:CC_|CLASS_|COMPOSITE$|GENERIC$)') { $null = $result.Add([string]$id) }
        }
    }
    return ,$result
}
function Resolve-AudioSelection($Documents) {
    # Only live components linked by parent or AddComponent/ComponentIDs enter
    # the graph. A vendor name or SoftwareComponent class alone is insufficient.
    for ($pass=0; $pass -lt 16; $pass++) {
        $before = $script:Selected.Count + $script:Packages.Count
        foreach ($node in $script:Nodes.Values) {
            if ($node.Class -notin @('SoftwareComponent','AudioProcessingObject')) { continue }
            $parent = [string](Get-NodeValue $node.InstanceId 'DEVPKEY_Device_Parent')
            if ($parent -and $script:Selected.ContainsKey($parent) -and $script:Selected[$parent].Role -ne 'Ancestor') {
                $null = Add-SelectedDevice $node.InstanceId 'Component' "Parent: $parent"
            }
        }
        foreach ($entry in @($script:Selected.Values)) {
            if ($entry.Role -ne 'Ancestor') {
                $inf = [string](Get-NodeValue $entry.InstanceId 'DEVPKEY_Device_DriverInfPath')
                if ($inf) { $null = Add-Package $inf "Installed on $($entry.InstanceId)" }
            }
        }
        $hardwareIds = Get-SelectedHardwareIds
        foreach ($doc in $Documents) {
            if ($doc.Class -ne 'Extension') { continue }
            $match = Find-IdIntersection $doc.Ids $hardwareIds
            if ($match) { $null = Add-Package $doc.Name "Installed extension candidate matching hardware ID: $match" }
        }
        $componentIds = New-IdSet
        foreach ($doc in $Documents) {
            if (-not $script:Packages.ContainsKey($doc.Name)) { continue }
            foreach ($id in $doc.Components) { $null = $componentIds.Add($id) }
            foreach ($inf in $doc.Includes) { $null = Add-Package $inf "Include dependency of $($doc.Name)" }
        }
        foreach ($node in $script:Nodes.Values) {
            if ($node.Class -notin @('SoftwareComponent','AudioProcessingObject')) { continue }
            $match = Find-IdIntersection @(Get-NodeValue $node.InstanceId 'DEVPKEY_Device_HardwareIds') $componentIds
            if ($match) { $null = Add-SelectedDevice $node.InstanceId 'Component' "Audio package ComponentIDs: $match" }
        }
        if ($before -eq ($script:Selected.Count + $script:Packages.Count)) { return }
    }
    throw 'Device/package relationship graph exceeded 16 passes; selection is incomplete.'
}
function Get-PackageFiles([string]$Directory) {
    # Never follow a junction/symlink out of a selected package.
    $pending = New-Object 'System.Collections.Generic.Stack[string]'; $pending.Push($Directory)
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force -ErrorAction Stop) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point in package: $($item.FullName)" }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) } else { $item }
        }
    }
}
function Add-PayloadFile([string]$Path,[string]$Owner,[string]$Relative) {
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Not a regular file: $Path" }
    $script:PayloadPlan.Add([pscustomobject]@{Owner=$Owner;RelativePath=$Relative;Source=$file.FullName;Bytes=[long]$file.Length;
        SHA256=$null;Blob=$null;State='PLANNED'})
}
function Save-PayloadPlan([long]$Budget,[bool]$Copy) {
    $blobs = @{}; [long]$uniqueBytes = 0
    foreach ($entry in $script:PayloadPlan) {
        try {
            $before = Get-Item -LiteralPath $entry.Source -ErrorAction Stop
            $hash = Get-Sha256 $entry.Source
            $after = Get-Item -LiteralPath $entry.Source -ErrorAction Stop
            if ($before.Length -ne $after.Length -or $before.LastWriteTimeUtc -ne $after.LastWriteTimeUtc) { throw 'Source changed during hashing; retry collection.' }
            $entry.Bytes = [long]$after.Length
            $entry.SHA256 = $hash
            if (-not $blobs.ContainsKey($hash)) {
                $entry.Blob = 'payload/' + $hash + [IO.Path]::GetExtension($entry.Source).ToLowerInvariant()
                $blobs[$hash] = $entry; $uniqueBytes += $entry.Bytes
            } else { $entry.Blob = $blobs[$hash].Blob }
        } catch { $entry.State='READ_FAILED'; Add-Status 'Payload hash' 'MISSING' "$($entry.Source): $($_.Exception.Message)" }
    }
    Write-Json ([ordered]@{Files=$script:PayloadPlan.Count;UniqueFiles=$blobs.Count;UniqueBytes=$uniqueBytes;BudgetBytes=$Budget}) 'payload-size.json'
    Write-Host ('Selected payload: {0:N1} MiB in {1} unique files ({2} package/file references)' -f ($uniqueBytes/1MB),$blobs.Count,$script:PayloadPlan.Count)
    if ($uniqueBytes -gt $Budget) {
        foreach ($entry in $script:PayloadPlan) { if ($entry.State -eq 'PLANNED') { $entry.State='OVER_BUDGET' } }
        Add-Status 'Payload budget' 'MISSING' 'No binary payload copied: selected unique bytes exceed MaxPayloadMB. Inspect payload-plan.csv; rerun with an explicit larger budget if needed.'
    } elseif ($Copy) {
        foreach ($hash in $blobs.Keys) {
            $entry = $blobs[$hash]; $destination = Join-Path $script:Root $entry.Blob
            try {
                Copy-Item -LiteralPath $entry.Source -Destination $destination -ErrorAction Stop
                if ((Get-Sha256 $destination) -ne $hash) {
                    Remove-Item -LiteralPath $destination
                    throw 'Source changed while being copied; retry collection.'
                }
                foreach ($reference in $script:PayloadPlan) { if ($reference.SHA256 -eq $hash) { $reference.State='COPIED' } }
            } catch {
                foreach ($reference in $script:PayloadPlan) { if ($reference.SHA256 -eq $hash) { $reference.State='COPY_FAILED' } }
                Add-Status 'Payload copy' 'MISSING' "$($entry.Source): $($_.Exception.Message)"
            }
        }
    }
    $script:PayloadPlan | Export-Csv -LiteralPath (Join-Path $script:Root 'payload-plan.csv') -NoTypeInformation -Encoding UTF8
}
function Export-RegistryOnce([string]$Key) {
    if (-not $script:RegistryKeys.Add($Key)) { return }
    $label = 'key-{0:D3}' -f $script:RegistryKeys.Count
    $script:RegistryIndex.Add([pscustomobject]@{Key=$Key;File="registry/$label.reg"})
    Invoke-Step "Registry $Key" { Invoke-Native 'reg.exe' @('export',$Key,(Join-Path $script:Root "registry/$label.reg"),'/y') "registry/$label.log" }
}
function Resolve-BinaryPath([string]$Text) {
    $value = [Environment]::ExpandEnvironmentVariables($Text).Trim()
    $value = $value -replace '^\\\?\?\\','' -replace '^\\SystemRoot\\',($env:windir+'\')
    if ($value -match '^"([^"]+\.(?:sys|dll|exe))"' -or $value -match '^(.+?\.(?:sys|dll|exe))(?:\s|$)') { $value=$Matches[1] }
    if ($value -match '^System32\\') { $value=Join-Path $env:windir $value }
    if ($value -notmatch '^[A-Z]:\\') { return $null }
    if (Test-Path -LiteralPath $value -PathType Leaf) { return (Get-Item -LiteralPath $value).FullName }
    return $null
}
function Get-EvidencePattern([string[]]$Terms) {
    $escaped=@($Terms | Where-Object { $_ } | ForEach-Object { [regex]::Escape($_) })
    if (-not $escaped.Count) { return '(?!)' }
    return '(?i)(?<![a-z0-9_])(?:'+($escaped -join '|')+')(?![a-z0-9_&\\])'
}
function Export-SetupApiSections([string]$Path,[string[]]$Terms,[string]$Destination) {
    $pattern = Get-EvidencePattern $Terms
    $stream = New-Object IO.FileStream($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $reader = New-Object IO.StreamReader($stream)
    $writer = New-Object IO.StreamWriter($Destination,$false,(New-Object Text.UTF8Encoding($false)))
    $block = New-Object 'System.Collections.Generic.List[string]'; $matched=$false; [long]$written=0
    $truncated=$false; [long]$blockBytes=0
    try {
        while ($null -ne ($line=$reader.ReadLine())) {
            if ($line.StartsWith('>>>')) { $block.Clear(); $matched=$false; $truncated=$false; $blockBytes=0 }
            $blockBytes += $line.Length*2+2
            if ($block.Count -lt 10000 -and $blockBytes -le 2MB) { $block.Add($line) }
            else { $truncated=$true }
            if ($line -match $pattern) { $matched=$true }
            if ($line.StartsWith('<<<')) {
                if ($matched) {
                    if ($truncated) { throw 'A relevant SetupAPI section exceeded 10000 lines/2 MiB; extract is incomplete.' }
                    foreach ($item in $block) { $writer.WriteLine($item); $written += $item.Length*2+2 }
                    if ($written -gt 20MB) { throw 'SetupAPI extract reached 20 MiB; extract is incomplete.' }
                }
                $block.Clear(); $matched=$false; $truncated=$false; $blockBytes=0
            }
        }
        if ($matched) {
            if ($truncated) { throw 'Unfinished relevant SetupAPI section exceeded limits.' }
            foreach ($item in $block) { $writer.WriteLine($item) }
        }
    } finally { $writer.Dispose(); $reader.Dispose(); $stream.Dispose() }
}
function Write-AudioZip([string]$Directory) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    # CreateNew and a fresh per-run directory prevent overwriting old reports.
    $stream = New-Object IO.FileStream(($Directory+'.zip'),[IO.FileMode]::CreateNew)
    $archive = New-Object IO.Compression.ZipArchive($stream,[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $Directory -File -Recurse) {
            $relative = $file.FullName.Substring($Directory.Length+1).Replace('\','/')
            $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.FullName,$relative,[IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose(); $stream.Dispose() }
}
function Read-CirrusCalibration {
    # One named audio variable, never a firmware/ACPI table or variable dump.
    if (-not ('WindowsAudioCalibrationReader' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WindowsAudioCalibrationReader {
 [StructLayout(LayoutKind.Sequential)] struct Luid { public uint Low; public int High; }
 [StructLayout(LayoutKind.Sequential)] struct Privileges { public uint Count; public Luid Id; public uint Attributes; }
 [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
 [DllImport("advapi32.dll",SetLastError=true)] static extern bool OpenProcessToken(IntPtr process,uint access,out IntPtr token);
 [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool LookupPrivilegeValue(string system,string name,out Luid id);
 [DllImport("advapi32.dll",SetLastError=true)] static extern bool AdjustTokenPrivileges(IntPtr token,bool disable,ref Privileges requested,uint bytes,out Privileges previous,out uint returned);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern uint GetFirmwareEnvironmentVariable(string name,string guid,byte[] data,uint bytes);
 public static byte[] Read(out int error) {
  error=0;IntPtr token;
  if(!OpenProcessToken(GetCurrentProcess(),0x28,out token)){error=Marshal.GetLastWin32Error();return null;}
  Privileges previous=new Privileges();bool changed=false;
  try {
   Luid id;if(!LookupPrivilegeValue(null,"SeSystemEnvironmentPrivilege",out id)){error=Marshal.GetLastWin32Error();return null;}
   Privileges request=new Privileges{Count=1,Id=id,Attributes=2};uint returned;
   if(!AdjustTokenPrivileges(token,false,ref request,(uint)Marshal.SizeOf(typeof(Privileges)),out previous,out returned)){error=Marshal.GetLastWin32Error();return null;}
   changed=true;error=Marshal.GetLastWin32Error();if(error!=0)return null;
   for(int length=4096;length<=1048576;length*=2){
    byte[] data=new byte[length];
    uint got=GetFirmwareEnvironmentVariable("CirrusSmartAmpCalibrationData","{02f9af02-7734-4233-b43d-93fe5aa35db3}",data,(uint)data.Length);
    if(got!=0){Array.Resize(ref data,(int)got);error=0;return data;}
    error=Marshal.GetLastWin32Error();if(error!=122)return null;
   }
   return null;
  } finally {
   if(changed){Privileges ignored;uint returned;AdjustTokenPrivileges(token,false,ref previous,(uint)Marshal.SizeOf(typeof(Privileges)),out ignored,out returned);}
   CloseHandle(token);
  }
 }
}
'@
    }
    $errorCode=0
    $data=[WindowsAudioCalibrationReader]::Read([ref]$errorCode)
    if ($null -ne $data) {
        $path=Join-Path $script:Root 'system/CirrusSmartAmpCalibrationData.bin'
        [IO.File]::WriteAllBytes($path,$data)
        Add-Status 'Cirrus EFI calibration' 'OK' "$($data.Length) bytes; exact named variable only"
    } elseif ($errorCode -in @(1,203)) {
        Add-Status 'Cirrus EFI calibration' 'UNAVAILABLE' "Variable absent or firmware interface unavailable; Win32 $errorCode"
    } else { Add-Status 'Cirrus EFI calibration' 'MISSING' "Read unavailable; Win32 $errorCode" }
}

if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitProcess) { throw 'Run in 64-bit Windows PowerShell.' }
$script:Root = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('Windows-Audio-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$null = New-Item -ItemType Directory -Path $script:Root
foreach ($name in @('system','devices','packages','registry','events','payload')) { $null=New-Item -ItemType Directory -Path (Join-Path $script:Root $name) }
$script:Status=New-Object 'System.Collections.Generic.List[object]'
$script:PropertyCache=@{}; $script:Nodes=@{}; $script:Selected=@{}; $script:Packages=@{}
$script:PayloadPlan=New-Object 'System.Collections.Generic.List[object]'
$script:RegistryKeys=New-IdSet; $script:RegistryIndex=New-Object 'System.Collections.Generic.List[object]'
$services=New-IdSet; $null=$services.Add('Audiosrv'); $null=$services.Add('AudioEndpointBuilder')
$transcript=$false
try {
    Start-Transcript -Path (Join-Path $script:Root 'transcript.txt') | Out-Null; $transcript=$true
    $admin=(New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { Add-Status 'Privileges' 'MISSING' 'Not elevated: driver-store metadata, registry or events may be incomplete. Run as administrator for complete collection.' }
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    Invoke-Step 'System/audio identity' {
        Write-Json ([ordered]@{CollectedAt=(Get-Date).ToString('o');PowerShell=$PSVersionTable.PSVersion.ToString();Elevated=$admin;
            OS=$os | Select-Object Caption,Version,BuildNumber,OSArchitecture;
            Computer=Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer,Model,SystemSKUNumber;
            BIOS=Get-CimInstance Win32_BIOS | Select-Object Manufacturer,SMBIOSBIOSVersion,ReleaseDate;
            SpeakerTestNotes=$SpeakerTestNotes;PlanOnly=[bool]$PlanOnly}) 'system/identity.json'
    }
    $sound=@(Get-CimInstance Win32_SoundDevice -ErrorAction Stop)
    Write-Json @($sound | Select-Object Name,DeviceID,Manufacturer,Status,ConfigManagerErrorCode) 'system/sound-devices.json'
    $soundIds=New-IdSet; foreach ($item in $sound) { $null=$soundIds.Add([string]$item.DeviceID) }
    foreach ($device in @(Get-PnpDevice -PresentOnly -ErrorAction Stop)) { $script:Nodes[$device.InstanceId]=$device }
    foreach ($device in $script:Nodes.Values) {
        if (Test-AudioSeed $device $soundIds) { $null=Add-SelectedDevice $device.InstanceId 'Audio' 'Present audio endpoint, sound device or Cirrus AMP' }
    }
    # Ancestors explain topology but never seed a chipset/GPU package export.
    foreach ($id in @($script:Selected.Keys)) {
        $current=$id
        for ($depth=0; $depth -lt 12; $depth++) {
            $parent=[string](Get-NodeValue $current 'DEVPKEY_Device_Parent')
            if (-not $parent -or -not $script:Nodes.ContainsKey($parent) -or $parent -eq $current) { break }
            $role='Ancestor'; $node=$script:Nodes[$parent]
            if ($node.FriendlyName -match 'Serial IO.*(SPI|I2C|GPIO)|\b(SPI|I2C|GPIO)\b.*Controller|Smart Sound|Audio.*Controller|High Definition Audio Controller') { $role='Controller' }
            $null=Add-SelectedDevice $parent $role "Parent of $current"
            $current=$parent
        }
    }
    $documents=New-Object 'System.Collections.Generic.List[object]'
    Invoke-Step 'Published INF relationships' {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $env:windir 'INF') -Filter 'oem*.inf' -File) {
            try { $documents.Add((Read-InfDocument $file.FullName)) }
            catch { Add-Status 'INF parsing' 'MISSING' "$($file.Name): $($_.Exception.Message)" }
        }
    }
    Invoke-Step 'Audio device/package graph' { Resolve-AudioSelection @($documents.ToArray()) }
    $selection=@($script:Selected.Values | Sort-Object InstanceId | ForEach-Object {
        [pscustomobject]@{InstanceId=$_.InstanceId;Class=$script:Nodes[$_.InstanceId].Class;
            Name=$script:Nodes[$_.InstanceId].FriendlyName;Role=$_.Role;Reasons=@($_.Reasons | Sort-Object)} })
    Write-Json $selection 'devices/selection.json'
    $selection | Format-Table Role,Class,Name -AutoSize | Out-Host
    Write-Json @($script:Packages.Values | Sort-Object Inf | ForEach-Object {
        [pscustomobject]@{Inf=$_.Inf;Reasons=@($_.Reasons | Sort-Object)} }) 'packages/selection.json'
    if (-not $selection.Count) { Add-Status 'Audio devices' 'MISSING' 'No present audio/AMP devices found.' }
    if (-not @($selection | Where-Object InstanceId -Match '^ACPI\\(?:VEN_CSC&DEV_35|CSC35|CLSA010)').Count) {
        Add-Status 'Cirrus AMP' 'MISSING' 'No present Cirrus AMP identity found. Audio-only evidence is still collected.'
    }
    $storeRecords=@()
    if ($admin) { Invoke-Step 'Installed driver-store map' { $script:StoreRecords=@(Get-WindowsDriver -Online -All -ErrorAction Stop) } }
    if ($script:StoreRecords) { $storeRecords=$script:StoreRecords }
    $storePrefix=[IO.Path]::GetFullPath((Join-Path $env:windir 'System32/DriverStore/FileRepository')).TrimEnd('\')+'\'
    foreach ($package in @($script:Packages.Values | Sort-Object Inf)) {
        Invoke-Step "Package $($package.Inf)" {
            $published=Join-Path $env:windir ('INF/'+$package.Inf)
            if (Test-Path -LiteralPath $published) { Add-PayloadFile $published $package.Inf $package.Inf }
            $originals=@($storeRecords | Where-Object Driver -EQ $package.Inf | Select-Object -ExpandProperty OriginalFileName -Unique)
            if ($originals.Count -ne 1) { throw 'Exact DriverStore mapping unavailable. No vendor-wide fallback copy is performed.' }
            $original=[IO.Path]::GetFullPath([string]$originals[0])
            if (-not $original.StartsWith($storePrefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'Original INF is outside DriverStore.' }
            $folder=Split-Path -Parent $original
            foreach ($file in @(Get-PackageFiles $folder)) {
                Add-PayloadFile $file.FullName $package.Inf $file.FullName.Substring($folder.Length+1)
            }
        }
    }
    $index=0
    foreach ($entry in @($script:Selected.Values | Sort-Object InstanceId)) {
        $index++; $label='{0:D3}' -f $index; $id=$entry.InstanceId
        $properties=@(Get-NodeProperties $id)
        Write-Json ([ordered]@{InstanceId=$id;Role=$entry.Role;Properties=$properties}) "devices/$label.json"
        if ($PlanOnly) { continue }
        if ($entry.Role -ne 'Ancestor') {
            Export-RegistryOnce "HKLM\SYSTEM\CurrentControlSet\Enum\$id"
            $driver=[string](Get-NodeValue $id 'DEVPKEY_Device_Driver')
            if ($driver -match '^\{[0-9A-F-]{36}\}\\\d{4}$') { Export-RegistryOnce "HKLM\SYSTEM\CurrentControlSet\Control\Class\$driver" }
            $service=[string](Get-NodeValue $id 'DEVPKEY_Device_Service'); if ($service) { $null=$services.Add($service) }
            if ([int]$os.BuildNumber -ge 22621) {
                Invoke-Step "Device resources $label" { Invoke-Native 'pnputil.exe' @('/enum-devices','/instanceid',$id,'/resources') "devices/$label-resources.txt" }
            } else { Add-Status "Device resources $label" 'SKIPPED' 'This Windows build lacks pnputil /resources; using scoped CIM resources.' }
            if ([int]$os.BuildNumber -ge 19041) {
                Invoke-Step "Driver candidates $label" { Invoke-Native 'pnputil.exe' @('/enum-devices','/instanceid',$id,'/drivers') "devices/$label-drivers.txt" }
            } else { Add-Status "Driver candidates $label" 'SKIPPED' 'This Windows build lacks pnputil /drivers; selected INF metadata is retained.' }
            Invoke-Step "CIM resources $label" {
                $escaped=$id.Replace('\','\\').Replace("'","\'")
                $entity=Get-CimInstance Win32_PnPEntity -Filter "DeviceID='$escaped'" -ErrorAction Stop
                if ($entity) { Write-Json @(Get-CimAssociatedInstance -InputObject $entity -Association Win32_PnPAllocatedResource -ErrorAction Stop) "devices/$label-allocated-resources.json" }
            }
        }
    }
    if (-not $PlanOnly) {
        Invoke-Step 'Named audio calibration' { Read-CirrusCalibration }
        foreach ($service in $services) {
            Export-RegistryOnce "HKLM\SYSTEM\CurrentControlSet\Services\$service"
            Invoke-Step "Service binary $service" {
                $key=Get-ItemProperty -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\$service"
                $binary=Resolve-BinaryPath ([string]$key.ImagePath)
                if ($binary) { Add-PayloadFile $binary ('service:'+ $service) ([IO.Path]::GetFileName($binary)) }
                $parameters=Get-ItemProperty -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\$service\Parameters" -ErrorAction SilentlyContinue
                if ($parameters -and $parameters.ServiceDll) {
                    $binary=Resolve-BinaryPath ([string]$parameters.ServiceDll)
                    if ($binary) { Add-PayloadFile $binary ('service:'+ $service) ([IO.Path]::GetFileName($binary)) }
                }
            }
        }
        Export-RegistryOnce 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio'
        # Follow effect CLSIDs referenced by the selected class/endpoint data.
        $clsids=New-IdSet
        foreach ($key in @($script:RegistryKeys)) {
            if ($key -notmatch '\\Control\\Class\\|\\MMDevices\\Audio$') { continue }
            $providerPath='Registry::'+$key.Replace('HKLM\','HKEY_LOCAL_MACHINE\')
            foreach ($item in @((Get-Item -LiteralPath $providerPath -ErrorAction SilentlyContinue)) + @(Get-ChildItem -LiteralPath $providerPath -Recurse -ErrorAction SilentlyContinue)) {
                if (-not $item) { continue }
                foreach ($name in $item.GetValueNames()) {
                    $value=$item.GetValue($name)
                    if ($value -is [string] -or $value -is [string[]]) {
                        foreach ($match in [regex]::Matches(($value -join ' '),'\{[0-9a-fA-F-]{36}\}')) { $null=$clsids.Add($match.Value) }
                    }
                }
            }
        }
        foreach ($clsid in $clsids) {
            $key="Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
            if (-not (Test-Path -LiteralPath $key)) { continue }
            Export-RegistryOnce "HKLM\SOFTWARE\Classes\CLSID\$clsid"
            Invoke-Step "Effect binary $clsid" {
                $binary=Resolve-BinaryPath ([string](Get-Item -LiteralPath $key).GetValue(''))
                if ($binary) { Add-PayloadFile $binary ('effect:'+ $clsid) ([IO.Path]::GetFileName($binary)) }
                else { throw 'Referenced in-process server binary is unavailable.' }
            }
        }
        Invoke-Step 'Audio services' {
            Write-Json @(Get-CimInstance Win32_Service | Where-Object { $services.Contains($_.Name) } | Select-Object Name,State,StartMode,PathName) 'system/audio-services.json'
        }
        Invoke-Step 'Audio installation log sections' {
            $logIds=@($script:Selected.Values | Where-Object Role -NE 'Ancestor' | ForEach-Object {$_.InstanceId})
            Export-SetupApiSections (Join-Path $env:windir 'INF/setupapi.dev.log') @($logIds+@($script:Packages.Keys)) (Join-Path $script:Root 'events/setupapi-audio.txt')
        }
        Invoke-Step 'Recent audio events' {
            $rows=New-Object 'System.Collections.Generic.List[object]'
            $terms=@($script:Selected.Values | Where-Object Role -NE 'Ancestor' | ForEach-Object { $_.InstanceId })
            $pattern=Get-EvidencePattern $terms
            foreach ($log in @('System','Application')) {
                foreach ($provider in @('Microsoft-Windows-Audio','Microsoft-Windows-AudioEndpointBuilder','Microsoft-Windows-AudioSrv','Microsoft-Windows-Kernel-PnP','Microsoft-Windows-DriverFrameworks-UserMode')) {
                    $events=@(Get-WinEvent -FilterHashtable @{LogName=$log;ProviderName=$provider;StartTime=(Get-Date).AddDays(-$EventDays)} -MaxEvents 1000 -ErrorAction SilentlyContinue)
                    if ($events.Count -eq 1000) { Add-Status "Events $provider" 'LIMITED' 'Candidate query reached 1000 events; older matching events may be omitted.' }
                    foreach ($event in $events) {
                        if ($provider -match 'Audio' -or ($terms.Count -and $event.Message -match $pattern)) {
                            $rows.Add([pscustomobject]@{Time=$event.TimeCreated;Log=$log;Provider=$provider;Id=$event.Id;Level=$event.LevelDisplayName;Message=$event.Message})
                        }
                    }
                }
            }
            $rows | Export-Csv -LiteralPath (Join-Path $script:Root 'events/audio-events.csv') -NoTypeInformation -Encoding UTF8
        }
    }
    Save-PayloadPlan ([long]$MaxPayloadMB * 1MB) (-not $PlanOnly)
    Write-Json @($script:RegistryIndex.ToArray()) 'registry/index.json'
    @'
Windows audio evidence. Read devices/selection.json and packages/selection.json
for every selection reason. Extension candidates match exact hardware IDs;
their presence does not prove which extension was active at runtime.
payload-plan.csv maps each original package path/name to its hash-named payload.
Identical files are stored once. No broad OEM export or vendor fallback exists.
No ACPI/DSDT/SSDT, whole-system events or whole SetupAPI log are collected.
status.csv records missing data, permissions, unsupported commands and limits.
PlanOnly intentionally omits payload/registry/events. Budget failures copy no
binary payload; inspect the plan and explicitly increase MaxPayloadMB if needed.
This does not capture live AMP registers, bus traffic, active DSP memory or
speaker-ID GPIO state. Firmware selected at runtime still needs separate proof.
Registry/device paths and event messages may identify the machine or user.
Review before sharing. Nothing is uploaded or changed on the audio device.
'@ | Set-Content -LiteralPath (Join-Path $script:Root 'READ-ME.txt') -Encoding UTF8
} catch { Add-Status 'Collector' 'FAILED' $_.Exception.ToString() }
finally {
    $script:Status | Export-Csv -LiteralPath (Join-Path $script:Root 'status.csv') -NoTypeInformation -Encoding UTF8
    if ($transcript) { Stop-Transcript | Out-Null }
}
Write-Manifest
Write-Host "OUTPUT: $script:Root" -ForegroundColor Green
if (-not $NoZip) {
    try { Write-AudioZip $script:Root; Write-Host "ZIP: $script:Root.zip" -ForegroundColor Green }
    catch { Write-Warning "Archive failed: $($_.Exception.Message). Keep the output directory."; exit 2 }
}
if (@($script:Status | Where-Object { $_.State -in @('MISSING','FAILED','LIMITED') }).Count) { exit 2 }
exit 0
