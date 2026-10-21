#requires -Version 5.1
<#
Read-only, bounded Windows audio/smart-AMP collector.
Compatible with Windows PowerShell 5.1, Windows 10/11.
No driver installation, hardware restart, raw register writes or network access.
#>
[CmdletBinding()]
param(
    [string]$OutputRoot = [Environment]::GetFolderPath('MyDocuments'),
    [ValidateRange(1,30)][int]$EventDays = 3,
    [ValidateRange(8,128)][int]$MaxMB = 24,
    [switch]$IncludeDriverFiles,
    [switch]$NoZip,
    [string]$SpeakerTestNotes = ''
)
$ErrorActionPreference = 'Stop'
$script:Status = New-Object 'System.Collections.Generic.List[object]'
$script:Budget = [long]$MaxMB * 1MB
$script:Used = [long]0
$script:Properties = @{}
$script:Nodes = @{}
$script:Selected = @{}
$script:InfNames = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$script:Services = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$script:AmpPattern = '(?i)cs35|clsa|csc35|cirrus|smart.?amp|amplifier|\bamp\b|tas2[0-9a-z]+|tas5[0-9a-z]+|max98[0-9a-z]+|max97[0-9a-z]+|rt13[0-9a-z]+|aw8[0-9a-z]+|nau83[0-9a-z]+|speaker.?id'
function Mark([string]$Task,[string]$State,[string]$Details='') {
    $script:Status.Add([pscustomobject]@{Task=$Task;State=$State;Details=$Details})
    Write-Host "[$State] $Task $Details"
}
function Write-Evidence([string]$Relative, $Data, [int]$FileCap = 524288) {
    $target = Join-Path $script:Root $Relative
    $parent = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $parent)) { $null = New-Item -ItemType Directory -Force -Path $parent }
    if ($Data -is [byte[]]) { $bytes=$Data }
    else {
        $json = if ($Data -is [string]) { $Data } else { ConvertTo-Json -InputObject $Data -Depth 12 -Compress:$false }
        $bytes = ([Text.UTF8Encoding]::new($false)).GetBytes([string]$json)
    }
    if ($bytes.Length -gt $FileCap) {
        Mark $Relative 'TRUNCATED' 'Per-file limit'
        [byte[]]$bounded = New-Object byte[] $FileCap
        [Array]::Copy($bytes,$bounded,$FileCap)
        $bytes=$bounded
    }
    if ($script:Used + $bytes.Length -gt $script:Budget) { Mark $Relative 'SKIPPED' 'Total size limit'; return $false }
    [IO.File]::WriteAllBytes($target,$bytes); $script:Used += $bytes.Length
    return $true
}
function Attempt([string]$Name,[scriptblock]$Action) {
    try { & $Action } catch { Mark $Name 'UNAVAILABLE' $_.Exception.Message }
}
function Props([string]$Id) {
    if (-not $script:Properties.ContainsKey($Id)) {
        try { $script:Properties[$Id]=@(Get-PnpDeviceProperty -InstanceId $Id -ErrorAction Stop) }
        catch { $script:Properties[$Id]=@(); Mark "properties $Id" 'UNAVAILABLE' $_.Exception.Message }
    }
    return $script:Properties[$Id]
}
function Val([string]$Id,[string]$Key) {
    foreach ($prop in @(Props $Id)) { if ($prop.KeyName -eq $Key) { return $prop.Data } }
    return $null
}
function Select-Node([string]$Id,[string]$Role) {
    if (-not $script:Nodes.ContainsKey($Id)) { return $false }
    if (-not $script:Selected.ContainsKey($Id)) { $script:Selected[$Id]=$Role; return $true }
    if ($script:Selected[$Id] -eq 'Parent' -and $Role -ne 'Parent') { $script:Selected[$Id]=$Role }
    return $false
}
function Read-RegValues([string]$Key, [string]$File) {
    # Small, selected registry metadata only. No full recursive reg export.
    $rows=New-Object 'System.Collections.Generic.List[object]'
    $stack=New-Object 'System.Collections.Generic.Stack[object]'
    $stack.Push([pscustomobject]@{Path=$Key;Depth=0})
    while ($stack.Count -and $rows.Count -lt 128) {
        $item=$stack.Pop()
        $node=Get-Item -LiteralPath $item.Path -ErrorAction SilentlyContinue
        if ($null -eq $node) { continue }
        foreach ($name in @($node.GetValueNames())) {
            $value=$node.GetValue($name)
            if ($null -eq $value) { continue }
            if ($value -is [byte[]]) { $value='byte['+$value.Length+'] SHA256='+([BitConverter]::ToString(([Security.Cryptography.SHA256]::Create()).ComputeHash($value))).Replace('-','') }
            elseif ($value -is [string[]]) { $value=($value -join '; ') }
            else { $value=[string]$value }
            if ([string]$value -match '(?i)password|secret|token|userprofile|\\users\\') { $value='[REDACTED]' }
            if ([string]$value -and ([string]$value).Length -gt 2048) { $value=([string]$value).Substring(0,2048)+' [TRUNCATED]' }
            $rows.Add([pscustomobject]@{Key=$item.Path;Name=$name;Value=$value})
            if ($rows.Count -ge 128) { break }
        }
        if ($item.Depth -lt 1) {
            foreach ($child in @(Get-ChildItem -LiteralPath $item.Path -ErrorAction SilentlyContinue)) {
                if ($stack.Count -lt 32) { $stack.Push([pscustomobject]@{Path=$child.PSPath;Depth=$item.Depth+1}) }
            }
        }
    }
    $null = Write-Evidence $File @($rows.ToArray()) 131072
}
function Copy-Small([string]$Source,[string]$Relative,[long]$Cap=262144) {
    try {
        $f=Get-Item -LiteralPath $Source -ErrorAction Stop
        if ($f.PSIsContainer -or ($f.Attributes -band [IO.FileAttributes]::ReparsePoint)) { return }
        if ($f.Length -gt $Cap) { Mark $Relative 'SKIPPED' "Source exceeds $Cap bytes"; return }
        if ($script:Used + $f.Length -gt $script:Budget) { Mark $Relative 'SKIPPED' 'Total size limit'; return }
        $dest=Join-Path $script:Root $Relative
        $null=New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest)
        Copy-Item -LiteralPath $Source -Destination $dest -ErrorAction Stop
        $script:Used += $f.Length
    } catch { Mark $Relative 'UNAVAILABLE' $_.Exception.Message }
}
function Run-Text([string]$Relative,[string]$Exe,[string[]]$Arguments,[int]$Cap=65536) {
    if (-not (Get-Command $Exe -ErrorAction SilentlyContinue)) { Mark $Relative 'UNAVAILABLE' "$Exe not available"; return }
    try {
        $old=$ErrorActionPreference
        $ErrorActionPreference='Continue'
        try { $output=@(& $Exe @Arguments 2>&1 | Out-String -Width 4096); $exit=$LASTEXITCODE }
        finally { $ErrorActionPreference=$old }
        $null=Write-Evidence $Relative ((@("Exit: $exit")+$output) -join "`r`n") $Cap
    } catch { Mark $Relative 'UNAVAILABLE' $_.Exception.Message }
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
        if (Write-Evidence 'system/CirrusSmartAmpCalibrationData.bin' $data 1048576) {
            Mark 'Cirrus EFI calibration' 'OK' "$($data.Length) bytes; exact named variable only"
        }
    } elseif ($errorCode -in @(1,203)) {
        Mark 'Cirrus EFI calibration' 'UNAVAILABLE' "Variable absent or firmware interface unavailable; Win32 $errorCode"
    } else { Mark 'Cirrus EFI calibration' 'MISSING' "Read unavailable; Win32 $errorCode" }
}


if ($env:OS -ne 'Windows_NT') { throw 'Windows required.' }
$base=[IO.Path]::GetFullPath($OutputRoot)
if (-not (Test-Path -LiteralPath $base -PathType Container)) { $null=New-Item -ItemType Directory -Force -Path $base }
$script:Root=Join-Path $base ('AUDIO_AMP_WINDOWS_'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'_'+[guid]::NewGuid().ToString('N').Substring(0,6))
$null=New-Item -ItemType Directory -Path $script:Root
try {
    $admin=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { Mark 'Permissions' 'LIMITED' 'Administrator privileges recommended for registry/driver metadata.' }
    Attempt 'Computer/OS' {
        $os=Get-CimInstance Win32_OperatingSystem
        $system=Get-CimInstance Win32_ComputerSystem
        $bios=Get-CimInstance Win32_BIOS
        $null=Write-Evidence 'system/identity.json' ([ordered]@{
            Timestamp=(Get-Date).ToUniversalTime().ToString('o'); Windows=($os | Select-Object Caption,Version,BuildNumber,OSArchitecture)
            Hardware=($system | Select-Object Manufacturer,Model,SystemSKUNumber)
            BIOS=($bios | Select-Object Manufacturer,SMBIOSBIOSVersion); Elevated=$admin;SpeakerTestNotes=$SpeakerTestNotes
        }) 16384
    }
    $sound=@(Get-CimInstance Win32_SoundDevice -ErrorAction SilentlyContinue)
    $null=Write-Evidence 'system/sound-devices.json' @($sound | Select-Object Name,DeviceID,Manufacturer,Status,ConfigManagerErrorCode) 65536
    $soundIDs=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($s in $sound) { $null=$soundIDs.Add([string]$s.DeviceID) }
    foreach ($node in @(Get-PnpDevice -PresentOnly -ErrorAction Stop)) { $script:Nodes[[string]$node.InstanceId]=$node }
    foreach ($node in @($script:Nodes.Values)) {
        $id=[string]$node.InstanceId; $name=[string]$node.FriendlyName; $cl=[string]$node.Class
        $isAmp = ($name+' '+$id) -match $script:AmpPattern
        $isAudio = $soundIDs.Contains($id) -or $cl -in @('MEDIA','AudioEndpoint','AudioProcessingObject') -or
            $id -match '^(HDAUDIO|INTELAUDIO|SWD\\MMDEVAPI|SWD\\DRIVERENUM)\\' -or
            ($cl -eq 'SoftwareComponent' -and $name -match '(?i)audio|sound|speaker|codec|effect')
        if ($isAmp -or $isAudio) { $null=Select-Node $id $(if($isAmp){'AMP'}else{'Audio'}) }
    }
    # Parent topology explains AMP bus/HDA routing but is never used as a seed for driver package copying.
    foreach ($id in @($script:Selected.Keys)) {
        $cursor=$id
        for ($depth=0;$depth -lt 8;$depth++) {
            $parent=[string](Val $cursor 'DEVPKEY_Device_Parent')
            if (-not $parent -or $parent -eq $cursor -or -not $script:Nodes.ContainsKey($parent)) { break }
            $null=Select-Node $parent 'Parent'
            $cursor=$parent
        }
    }
    # Capture immediate software components linked to selected audio devices.
    foreach ($node in @($script:Nodes.Values)) {
        if ($node.Class -notin @('SoftwareComponent','AudioProcessingObject')) { continue }
        $parent=[string](Val $node.InstanceId 'DEVPKEY_Device_Parent')
        if ($parent -and $script:Selected.ContainsKey($parent) -and $script:Selected[$parent] -ne 'Parent') {
            $null=Select-Node $node.InstanceId 'Component'
        }
    }
    # Compact ACPI inventory helps identify vendor-specific AMP HIDs when no audio class/driver is exposed.
    $unclassified=New-Object 'System.Collections.Generic.List[object]'
    foreach ($node in @($script:Nodes.Values | Sort-Object InstanceId)) {
        if ($unclassified.Count -ge 160) { break }
        if ($node.InstanceId -notmatch '^ACPI\\' -or $script:Selected.ContainsKey([string]$node.InstanceId)) { continue }
        if ($node.Class -notin @('System','Unknown','MEDIA','SoftwareComponent')) { continue }
        $unclassified.Add([pscustomobject]@{InstanceId=$node.InstanceId;Name=$node.FriendlyName;Class=$node.Class;Status=$node.Status})
    }
    $null=Write-Evidence 'devices/other-acpi-devices.json' @($unclassified.ToArray()) 65536
    $selected=@($script:Selected.GetEnumerator() | Sort-Object Key)
    if ($selected.Count -gt 96) { Mark 'Device selection' 'LIMITED' '96-device cap; extra devices excluded';$selected=@($selected | Select-Object -First 96) }
    if (-not $selected.Count) { Mark 'Device discovery' 'UNAVAILABLE' 'No active audio devices reported.' }
    $summary=New-Object 'System.Collections.Generic.List[object]'
    $idx=0
    foreach ($entry in $selected) {
        $idx++;$id=[string]$entry.Key; $node=$script:Nodes[$id]; $role=[string]$entry.Value
        $p=@(Props $id)
        $keys=@('DEVPKEY_Device_HardwareIds','DEVPKEY_Device_CompatibleIds','DEVPKEY_Device_DriverInfPath',
            'DEVPKEY_Device_Driver','DEVPKEY_Device_Service','DEVPKEY_Device_Parent','DEVPKEY_Device_BusReportedDeviceDesc',
            'DEVPKEY_Device_DriverVersion','DEVPKEY_Device_DriverProvider','DEVPKEY_Device_Manufacturer',
            'DEVPKEY_Device_LocationPaths','DEVPKEY_Device_ProblemCode')
        $fields=[ordered]@{}
        foreach ($key in $keys) {
            $match=@($p | Where-Object KeyName -EQ $key | Select-Object -First 1)
            if ($match.Count) { $fields[$key]=$match[0].Data }
        }
        $summary.Add([pscustomobject]@{InstanceId=$id;Role=$role;Name=$node.FriendlyName;Class=$node.Class;Status=$node.Status;Fields=$fields})
        if ($role -eq 'Parent') { continue }
        $inf=[string]$fields['DEVPKEY_Device_DriverInfPath']
        if ($inf -match '^oem\d+\.inf$') { $null=$script:InfNames.Add($inf) }
        $service=[string]$fields['DEVPKEY_Device_Service']
        if ($service -and $service -match '^[\w.-]+$') { $null=$script:Services.Add($service) }
        $safe='{0:D3}' -f $idx
        if ($admin) {
            $driver=[string]$fields['DEVPKEY_Device_Driver']
            if ($driver -match '^\{[0-9a-fA-F-]{36}\}\\\d{4}$') {
                Attempt "registry class $safe" { Read-RegValues ("Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\"+$driver) "registry/$safe-class.json" }
            }
            Attempt "registry device $safe" { Read-RegValues ("Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\"+$id) "registry/$safe-device.json" }
        }
        # Keep device-specific driver candidates and resource metadata, not whole PnP inventory.
        if ([int]([Environment]::OSVersion.Version.Build) -ge 19041) {
            Run-Text "devices/$safe-drivers.txt" 'pnputil.exe' @('/enum-devices','/instanceid',$id,'/drivers') 32768
        }
        if ([int]([Environment]::OSVersion.Version.Build) -ge 22621) {
            Run-Text "devices/$safe-resources.txt" 'pnputil.exe' @('/enum-devices','/instanceid',$id,'/resources') 32768
        }
    }
    $null=Write-Evidence 'devices/selection.json' @($summary.ToArray()) 1048576
    if ($admin -and @($summary | Where-Object { ($_.InstanceId+' '+$_.Name) -match '(?i)cs35|csc35|clsa|cirrus' }).Count) {
        Attempt 'Cirrus EFI calibration' { Read-CirrusCalibration }
    }
    # Copy installed audio INF; related extensions are candidates, not proof of runtime activation.
    $installedIds=New-Object 'System.Collections.Generic.List[string]'
    foreach ($entry in $summary) {
        if ($entry.Role -eq 'Parent') { continue }
        foreach ($hw in @($entry.Fields['DEVPKEY_Device_HardwareIds'])) {
            if ($hw -and ([string]$hw).Length -gt 7) { $installedIds.Add([string]$hw) }
        }
    }
    $extCandidates=New-Object 'System.Collections.Generic.List[object]'
    Attempt 'INF extension matching' {
        foreach ($inf in @(Get-ChildItem -LiteralPath (Join-Path $env:windir 'INF') -Filter 'oem*.inf' -File)) {
            if ($inf.Length -gt 512KB) { continue }
            $content=[IO.File]::ReadAllText($inf.FullName,[Text.Encoding]::Default)
            if ($content -notmatch '(?im)^\s*Class\s*=\s*Extension\b') { continue }
            foreach ($hw in @($installedIds | Select-Object -Unique)) {
                if ($content.IndexOf($hw,[StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    $extCandidates.Add([pscustomobject]@{Inf=$inf.Name;MatchedHardwareId=$hw;Note='Candidate; does not prove extension was active'})
                    $null=$script:InfNames.Add($inf.Name); break
                }
            }
        }
    }
    $null=Write-Evidence 'drivers/extension-candidates.json' @($extCandidates.ToArray()) 131072
    foreach ($inf in @($script:InfNames | Sort-Object)) {
        Copy-Small (Join-Path $env:windir ('INF\'+$inf)) ('drivers/inf/'+$inf) 524288
    }
    $null=Write-Evidence 'drivers/selected-inf.json' @($script:InfNames | Sort-Object) 16384
    # DriverStore binary files are opt-in: only the exact selected OEM packages; cap each payload.
    if ($IncludeDriverFiles) {
        Attempt 'Selected DriverStore metadata' {
            if (-not $admin) { throw 'Administrator required for mapping DriverStore files.' }
            $drivers=@(Get-WindowsDriver -Online -All -ErrorAction Stop)
            $prefix=[IO.Path]::GetFullPath((Join-Path $env:windir 'System32\DriverStore\FileRepository')).TrimEnd('\')+'\'
            $seen=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
            foreach ($inf in @($script:InfNames)) {
                $originals=@($drivers | Where-Object Driver -EQ $inf | Select-Object -ExpandProperty OriginalFileName -Unique)
                if ($originals.Count -ne 1) { Mark "DriverStore $inf" 'UNAVAILABLE' 'Exact mapping not found';continue }
                $orig=[IO.Path]::GetFullPath([string]$originals[0]);$dir=Split-Path -Parent $orig
                if (-not ($orig.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase))) { continue }
                foreach ($f in @(Get-ChildItem -LiteralPath $dir -File -Recurse -ErrorAction SilentlyContinue)) {
                    if ($f.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
                    if ($f.Extension -notin @('.sys','.dll','.bin','.wmfw','.bfb','.cfg','.dat')) { continue }
                    if (-not $seen.Add($f.FullName)) { continue }
                    $rel=$f.FullName.Substring($dir.Length+1).Replace('\','/')
                    Copy-Small $f.FullName ('drivers/binaries/'+$inf+'/'+$rel) (2MB)
                }
            }
        }
    }
    # Only services belonging to selected audio hardware / components.
    $services=New-Object 'System.Collections.Generic.List[object]'
    foreach ($service in @($script:Services | Sort-Object)) {
        Attempt "service $service" {
            $s=Get-CimInstance Win32_Service -Filter "Name='$service'" -ErrorAction SilentlyContinue
            if ($s) { $services.Add(($s | Select-Object Name,State,StartMode,PathName)) }
            if ($admin) { Read-RegValues ("Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\"+$service) ('registry/service-'+$service+'.json') }
        }
    }
    $null=Write-Evidence 'drivers/services.json' @($services.ToArray()) 131072
    # Only relevant recent kernel-PnP/audio events, bounded to 160 total entries.
    Attempt 'Audio event logs' {
        $rows=New-Object 'System.Collections.Generic.List[object]'
        $providers=@('Microsoft-Windows-Kernel-PnP','Microsoft-Windows-Audio','Microsoft-Windows-AudioEndpointBuilder','Microsoft-Windows-DriverFrameworks-UserMode')
        foreach ($provider in $providers) {
            foreach ($log in @('System','Application')) {
                $events=@(Get-WinEvent -FilterHashtable @{LogName=$log;ProviderName=$provider;StartTime=(Get-Date).AddDays(-$EventDays)} -MaxEvents 300 -ErrorAction SilentlyContinue)
                foreach ($ev in $events) {
                    $message=[string]$ev.Message
                    if ($provider -match 'Audio' -or $message -match $script:AmpPattern -or $message -match '(?i)HDAUDIO|INTELAUDIO|sound|codec|speaker') {
                        if ($message.Length -gt 1500) { $message=$message.Substring(0,1500)+' [TRUNCATED]' }
                        $rows.Add([pscustomobject]@{Time=$ev.TimeCreated;Provider=$provider;Id=$ev.Id;Level=$ev.LevelDisplayName;Message=$message})
                        if ($rows.Count -ge 160) { break }
                    }
                }
                if ($rows.Count -ge 160) { break }
            }
            if ($rows.Count -ge 160) { break }
        }
        $null=Write-Evidence 'events/audio-events.json' @($rows.ToArray()) 262144
    }
    $readme = @'
Windows audio / AMP evidence, selected from active PnP devices (not HP-specific).
Default: device graph, audio identities, installed INF plus matching extension
candidates, small selected registry values, service and bounded recent event data.
Extension candidates do NOT prove runtime activation. Driver binaries optional.
Read-only: no writes to device, service, firmware, driver or registry; no upload.
Hidden AMP and bus register state may require Linux ACPI/regmap collection.
This Windows collector does not claim to capture DSDT/SSDT or active DSP state.
Review serials, device IDs, driver packages and logs before sharing.
'@
    $null=Write-Evidence 'README.txt' $readme 4096
} catch { Mark 'Collector' 'FAILED' $_.Exception.ToString() }
finally {
    $null=Write-Evidence 'status.json' @($script:Status.ToArray()) 131072
    $checks=New-Object 'System.Collections.Generic.List[string]'
    foreach ($f in @(Get-ChildItem -LiteralPath $script:Root -File -Recurse | Sort-Object FullName)) {
        $stream=[IO.File]::OpenRead($f.FullName)
        $alg=[Security.Cryptography.SHA256]::Create()
        try { $hex=([BitConverter]::ToString($alg.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
        finally { $stream.Dispose();$alg.Dispose() }
        $checks.Add($hex+'  '+$f.FullName.Substring($script:Root.Length+1).Replace('\','/'))
    }
    $checks | Set-Content -LiteralPath (Join-Path $script:Root 'SHA256SUMS') -Encoding UTF8
    Write-Host "OUTPUT: $script:Root ($script:Used bytes)"
    if (-not $NoZip) {
        try {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $zip=$script:Root+'.zip'
            [IO.Compression.ZipFile]::CreateFromDirectory($script:Root,$zip,[IO.Compression.CompressionLevel]::Optimal,$true)
            Write-Host "ZIP: $zip"
        } catch { Mark 'ZIP' 'UNAVAILABLE' $_.Exception.Message }
    }
}
