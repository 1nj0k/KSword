param([Parameter(Mandatory)][string]$PipeName,
      [Parameter(Mandatory)][string]$OutputPath,
      [ValidateRange(10,600)][int]$Seconds=300)
$ErrorActionPreference='Stop'
$pipe=[IO.Pipes.NamedPipeClientStream]::new('.', $PipeName, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
$output=[IO.File]::Open($OutputPath,'Create','Write','ReadWrite')
$record=[ordered]@{startUtc=[DateTime]::UtcNow.ToString('o');pipe=$PipeName;bytes=0;status='started'}
try {
    $pipe.Connect(60000)
    $clock=[Diagnostics.Stopwatch]::StartNew();$buffer=New-Object byte[] 4096
    while($clock.Elapsed.TotalSeconds -lt $Seconds) {
        $read=$pipe.ReadAsync($buffer,0,$buffer.Length)
        while(!$read.Wait(250) -and $clock.Elapsed.TotalSeconds -lt $Seconds) {}
        if(!$read.IsCompleted){break}
        $count=$read.GetAwaiter().GetResult()
        if($count -eq 0){break}
        $output.Write($buffer,0,$count);$output.Flush();$record.bytes+=$count
    }
    $record.status='complete'
} catch {$record.status='error';$record.error=$_.Exception.Message} finally {
    $pipe.Dispose();$output.Dispose();$record.endUtc=[DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json | Set-Content -LiteralPath ($OutputPath+'.collector.json') -Encoding UTF8
}
