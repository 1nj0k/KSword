#requires -Version 5.1
<#
.SYNOPSIS
    Drives the TinyCore serial console over the VMware named pipe and records a transcript.

.DESCRIPTION
    The descendant VM is configured with

        serial0.fileType      = "pipe"
        serial0.fileName      = "\\.\pipe\tinycore-console"
        serial0.pipe.endPoint = "server"

    so VMware owns the pipe and this script connects as the client. Everything read
    from the console is appended to the transcript file, so the transcript survives
    even when the guest never answers.

    The script always terminates: every read is a polled BeginRead against a wall
    clock deadline, and the deadline is enforced whether or not the guest responds.

.NOTES
    Run this inside the Windows VM that hosts the descendant, not on the physical host.
#>
param(
    [string]$PipeName = 'tinycore-console',
    [string]$TranscriptPath = 'C:\vmware\tinycore-console.txt',
    [string[]]$Command = @(),
    [int]$WakeCount = 3,
    [int]$SettleSeconds = 6,
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$transcript = New-Object System.Text.StringBuilder

function Add-Transcript {
    param([string]$Text)
    if ($Text) { [void]$transcript.Append($Text) }
}

$stream = New-Object System.IO.Pipes.NamedPipeClientStream('.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::Asynchronous)
try {
    $stream.Connect(5000)
} catch {
    "CONNECT-FAILED: $($_.Exception.Message)" | Set-Content -LiteralPath $TranscriptPath -Encoding ASCII
    exit 2
}

$buffer = New-Object byte[] 8192
$pending = $null

function Drain-Console {
    param([int]$Seconds)
    $stop = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $stop -and (Get-Date) -lt $deadline) {
        if ($null -eq $script:pending) {
            $script:pending = $stream.BeginRead($buffer, 0, $buffer.Length, $null, $null)
        }
        if ($script:pending.AsyncWaitHandle.WaitOne(200)) {
            $read = $stream.EndRead($script:pending)
            $script:pending = $null
            if ($read -gt 0) {
                Add-Transcript ([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
            }
        }
    }
}

function Send-Console {
    param([string]$Line)
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Line + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    Add-Transcript ("`n[SENT] " + $Line + "`n")
}

Add-Transcript ("[CONNECTED] " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + "`n")

# Wake the console: a bare newline asks getty or the shell to redraw its prompt.
for ($i = 0; $i -lt $WakeCount; $i++) {
    Send-Console ''
    Drain-Console -Seconds 2
}

foreach ($line in $Command) {
    if ((Get-Date) -ge $deadline) { break }
    Send-Console $line
    Drain-Console -Seconds $SettleSeconds
}

Drain-Console -Seconds 3

Add-Transcript ("`n[CLOSED] " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + "`n")
$transcript.ToString() | Set-Content -LiteralPath $TranscriptPath -Encoding ASCII
$stream.Dispose()
exit 0
