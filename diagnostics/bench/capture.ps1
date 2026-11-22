# Capture one benchmark run of iRacing: PresentMon frame times, Windows CPU/GPU counters and iRacing telemetry.
#
# Requires PresentMon 2.x (https://github.com/GameTechDev/PresentMon/releases): by default, the console executable
# PresentMon-2.6.0-x64.exe next to this script. PresentMon needs an elevated prompt (or the "Performance Log Users"
# group), and Python 3 for the iRacing telemetry.
#
# Usage (with iRacing running and the car on track):
#   .\capture.ps1 -Label smp_on
#   .\capture.ps1 -Label smp_off -Seconds 90 -Delay 10
#   .\capture.ps1 -Label box_3proj_r1 -Seconds 5 -PresentMon C:\Tools\PresentMon.exe
#
# Output: <OutDir>\<Label>_<timestamp>\{presentmon.csv, counters.csv, irsdk.csv, meta.json}, next to this script.

param(
    [Parameter(Mandatory)][string]$Label,
    [int]$Seconds = 60,
    [int]$Delay = 5,
    [string]$OutDir = 'runs',
    [string]$PresentMon = (Join-Path $PSScriptRoot 'PresentMon-2.6.0-x64.exe')
)

$ErrorActionPreference = 'Stop'
$exe = 'iRacingSim64DX11'
$root = $PSScriptRoot
if (-not (Test-Path $PresentMon)) { throw "PresentMon not found at '$PresentMon' (see -PresentMon)." }

$process = Get-Process $exe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $process) { throw "$exe.exe is not running. Start iRacing and get on track first." }

$runDir = Join-Path $root ("{0}\{1}_{2}" -f $OutDir, $Label, (Get-Date -Format 'yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Force $runDir | Out-Null

Write-Host "Run '$Label': starting in $Delay s, recording for $Seconds s (pid $($process.Id))..."
Start-Sleep -Seconds $Delay

# 1) PresentMon: per-frame metrics (FrameTime, CPUBusy, GPUBusy, ...).
# Start-Process joins -ArgumentList with spaces without quoting: paths with spaces must be quoted here.
$pm = Start-Process -FilePath $PresentMon -PassThru -WindowStyle Hidden -ArgumentList @(
    '--process_name', "$exe.exe",
    '--output_file', ('"{0}"' -f (Join-Path $runDir 'presentmon.csv')),
    '--timed', $Seconds, '--terminate_after_timed',
    '--stop_existing_session', '--no_console_stats')

# 2) iRacing's own telemetry (FrameRate, CpuUsageFG, GpuUsage).
$irsdk = Start-Process -FilePath 'python' -PassThru -WindowStyle Hidden -ArgumentList @(
    '-I', ('"{0}"' -f (Join-Path $root 'irsdk_logger.py')), ('"{0}"' -f (Join-Path $runDir 'irsdk.csv')), $Seconds)

# 3) Windows counters, once per second: CPU per core, iRacing's CPU, and iRacing's 3D engine usage on the GPU.
$cores = [Environment]::ProcessorCount
$pidTag = "pid_$($process.Id)_"
$rows = foreach ($i in 1..$Seconds) {
    $samples = (Get-Counter -ErrorAction SilentlyContinue -Counter @(
            '\Processor(*)\% Processor Time',
            "\Process($exe)\% Processor Time",
            '\GPU Engine(*engtype_3D)\Utilization Percentage')).CounterSamples
    $perCore = $samples | Where-Object { $_.Path -like '*\processor(*' -and $_.InstanceName -ne '_total' }
    [pscustomobject]@{
        t              = $i
        cpu_total      = [math]::Round(($samples | Where-Object { $_.Path -like '*\processor(_total)*' }).CookedValue, 1)
        cpu_max_core   = [math]::Round(($perCore | Measure-Object CookedValue -Maximum).Maximum, 1)
        iracing_cpu    = [math]::Round(($samples | Where-Object { $_.Path -like '*\process(*' }).CookedValue / $cores, 1)
        iracing_gpu_3d = [math]::Round(($samples | Where-Object { $_.InstanceName -like "$pidTag*" } | Measure-Object CookedValue -Sum).Sum, 1)
    }
}
$rows | Export-Csv -NoTypeInformation (Join-Path $runDir 'counters.csv')

$pm.WaitForExit(15000) | Out-Null
$irsdk.WaitForExit(15000) | Out-Null

@{
    label    = $Label
    seconds  = $Seconds
    date     = (Get-Date -Format o)
    cpu      = (Get-CimInstance Win32_Processor).Name
    gpu      = (Get-CimInstance Win32_VideoController | Where-Object Name -like '*Radeon*').Name
    driver   = (Get-CimInstance Win32_VideoController | Where-Object Name -like '*Radeon*').DriverVersion
} | ConvertTo-Json | Set-Content (Join-Path $runDir 'meta.json')

Write-Host "Done: $runDir"
Get-ChildItem $runDir | Select-Object Name, Length | Format-Table -AutoSize
