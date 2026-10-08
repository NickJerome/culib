param([Parameter(Mandatory = $true)][string]$WintunDll)

$ErrorActionPreference = 'Stop'
$dll = (Resolve-Path -LiteralPath $WintunDll).Path
if ((Get-AuthenticodeSignature -LiteralPath $dll).Status -ne 'Valid') {
    throw 'Wintun DLL signature is invalid'
}
$root = Split-Path -Parent $PSScriptRoot
$program = Join-Path $root 'build\windows\x64\debug\example_net_virtual.exe'
if (-not (Test-Path -LiteralPath $program)) {
    throw 'Build example_net_virtual for Windows x64 debug first'
}

$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $program
$start.Arguments = '--icmp-echo'
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.CreateNoWindow = $true
$start.EnvironmentVariables['PATH'] = (Split-Path -Parent $dll) + ';' + $start.EnvironmentVariables['PATH']

$worker = New-Object System.Diagnostics.Process
$worker.StartInfo = $start
$adapter = $null
try {
    if (-not $worker.Start()) { throw 'Failed to start Wintun roundtrip probe' }
    $opening = $worker.StandardOutput.ReadLineAsync()
    if (-not $opening.Wait(10000)) { throw 'Wintun adapter did not open within 10 seconds' }
    if ($opening.Result -notmatch '^opened (.+)$') {
        throw "Wintun adapter did not open: $($opening.Result) $($worker.StandardError.ReadToEnd())"
    }
    $adapter = $Matches[1]
    Write-Output $opening.Result

    New-NetIPAddress -InterfaceAlias $adapter -IPAddress '198.51.100.1' `
        -PrefixLength 30 -AddressFamily IPv4 -PolicyStore ActiveStore | Out-Null
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        $address = Get-NetIPAddress -InterfaceAlias $adapter -IPAddress '198.51.100.1' `
            -AddressFamily IPv4 -ErrorAction SilentlyContinue
        if ($address.AddressState -eq 'Preferred') { break }
        Start-Sleep -Milliseconds 250
    }
    if ($address.AddressState -ne 'Preferred') {
        throw "Wintun test address did not become ready: $($address.AddressState)"
    }
    $reply = & ping.exe -n 1 -w 5000 198.51.100.2 2>&1
    $pingStatus = $LASTEXITCODE
    Write-Output ($reply -join "`n")
    if (-not $worker.WaitForExit(16000)) { throw 'ICMP responder did not exit' }
    $output = $worker.StandardOutput.ReadToEnd()
    $errors = $worker.StandardError.ReadToEnd()
    Write-Output $output
    if ($pingStatus -ne 0 -or $worker.ExitCode -ne 0 -or
        $output -notmatch 'ICMP echo replied') {
        throw "Wintun roundtrip failed: ping=$pingStatus process=$($worker.ExitCode) $reply $errors"
    }
    Write-Output 'Wintun IP roundtrip passed'
}
finally {
    if (-not $worker.HasExited) {
        $worker.Kill()
        $worker.WaitForExit()
    }
    $worker.Dispose()
}
