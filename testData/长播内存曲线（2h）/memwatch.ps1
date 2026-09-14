param(
    [string]$ProcName    = "QtPlayer",
    [int]   $IntervalSec = 300,
    [int]   $DurationMin = 120,
    [string]$OutFile     = "mem_2h.csv"
)

"timestamp,elapsed_min,private_MB,workingset_MB,pid" | Out-File -FilePath $OutFile -Encoding utf8

# Capture first matched process and lock its PID
$procCandidate = Get-Process -Name $ProcName -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $procCandidate) {
    Write-Host "ERROR: Process $ProcName not found"
    exit 1
}
$targetPid = $procCandidate.Id
Write-Host "Locked target process: $ProcName , PID=$targetPid"

$start    = Get-Date
$deadline = $start.AddMinutes($DurationMin)

while ((Get-Date) -lt $deadline) {
    $p = Get-Process -Id $targetPid -ErrorAction SilentlyContinue
    if (-not $p) {
        "process exited,pid=$targetPid" | Out-File -FilePath $OutFile -Append -Encoding utf8
        Write-Host "Process PID $targetPid has exited, monitoring stopped"
        break
    }

    $now     = Get-Date
    $elapsed = [math]::Round(($now - $start).TotalMinutes, 1)
    $priv    = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
    $ws      = [math]::Round($p.WorkingSet64        / 1MB, 1)
    $line    = "$($now.ToString('HH:mm:ss')),$elapsed,$priv,$ws,$targetPid"
    $line | Out-File -FilePath $OutFile -Append -Encoding utf8
    Write-Host "$elapsed min : private $priv MB | PID $targetPid"

    Start-Sleep -Seconds $IntervalSec
}
Write-Host "Monitoring duration finished"
