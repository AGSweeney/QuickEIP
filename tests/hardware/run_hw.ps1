# MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
# Run the hardware test stages against one device. See README.md in this folder.
#
#   .\run_hw.ps1 -HwTest <path to hw_test.exe> -Ip 172.16.82.153 -Qos 1 -Rpi 10 -Nic Ethernet -Com COM20
#   .\run_hw.ps1 -HwTest <path to hw_test.exe> -Ip 10.168.41.12 -Qos 0 -Rpi 50 -Nic "Ethernet 2"
#
# -Nic starts a tshark capture on that adapter (DSCP of each packet). -Com logs a serial console
# with serlog.py, which needs Python with pyserial. Both are optional. Results go to -Out.
param(
    [Parameter(Mandatory)][string]$HwTest,
    [Parameter(Mandatory)][string]$Ip,
    [int]$Qos = 0,
    [int]$Rpi = 10,
    [string]$Nic = '',
    [string]$Com = '',
    [string]$Out = (Join-Path $env:TEMP 'quickeip_hw'),
    [int]$RestartWait = 15,
    [string]$Tshark = 'C:\Program Files\Wireshark\tshark.exe',
    [string]$Python = 'python'
)
New-Item -ItemType Directory -Force $Out | Out-Null
$cap = Join-Path $Out 'capture.txt'; $ser = Join-Path $Out 'serial.log'; $res = Join-Path $Out 'result.txt'
Remove-Item $cap, $ser, $res -ErrorAction SilentlyContinue
$total = 75 + 2 * $RestartWait
$ts = $null; $sl = $null
if ($Nic -ne '') {
    # Columns: time, source, DSCP, TCP source port, UDP source port, TCP flags.
    $ts = Start-Process -FilePath $Tshark -ArgumentList @('-i', "`"$Nic`"", '-f', "`"host $Ip`"", '-a', "duration:$total",
        '-T', 'fields', '-e', 'frame.time_relative', '-e', 'ip.src', '-e', 'ip.dsfield.dscp', '-e', 'tcp.srcport',
        '-e', 'udp.srcport', '-e', 'tcp.flags.str') -RedirectStandardOutput $cap `
        -RedirectStandardError (Join-Path $Out 'tshark.err') -PassThru -WindowStyle Hidden
}
if ($Com -ne '') {
    $sl = Start-Process -FilePath $Python -ArgumentList @((Join-Path $PSScriptRoot 'serlog.py'), $Com, $ser, "$total") `
        -PassThru -WindowStyle Hidden
}
Start-Sleep -Seconds 3
# Warm-up with no checks. Security software can drop the first seconds of UDP of a new program,
# in both directions, and the device then times out the first connection.
& $HwTest $Ip w $Qos $Rpi *> $null
Start-Sleep -Seconds 4
& $HwTest $Ip a $Qos $Rpi *>> $res
Start-Sleep -Seconds $RestartWait
& $HwTest $Ip b $Qos $Rpi *>> $res
Start-Sleep -Seconds $RestartWait
& $HwTest $Ip c $Qos $Rpi *>> $res
Start-Sleep -Seconds 2
if ($ts -and -not $ts.HasExited) { Stop-Process -Id $ts.Id -Force }
if ($sl -and -not $sl.HasExited) { Stop-Process -Id $sl.Id -Force }
Get-Content $res
if ($Nic -ne '' -and (Test-Path $cap)) {
    'Packets from the device, by source port and DSCP:'
    Get-Content $cap | ForEach-Object {
        $f = $_ -split "`t"
        if ($f[1] -eq $Ip) { if ($f[3]) { "tcp $($f[3]) dscp $($f[2])" } else { "udp $($f[4]) dscp $($f[2])" } }
    } | Group-Object | Sort-Object Name | ForEach-Object { '  {0,-24} {1}' -f $_.Name, $_.Count }
}
