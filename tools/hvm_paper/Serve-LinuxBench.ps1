param([string]$Directory='C:\ksword\paper',[int]$Port=18080,[int]$Seconds=180)
$ErrorActionPreference='Stop'
# Temporary lab transfer only, restricted to the VMware NAT-side address and
# two public benchmark files. This server does not expose arbitrary paths.
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Parse('192.168.61.1'),$Port)
$watch=[Diagnostics.Stopwatch]::StartNew()
$listener.Start()
try {
    while($watch.Elapsed.TotalSeconds -lt $Seconds) {
        if(!$listener.Pending()){Start-Sleep -Milliseconds 100;continue}
        $client=$listener.AcceptTcpClient()
        try {
            $client.ReceiveTimeout=5000;$client.SendTimeout=5000
            $stream=$client.GetStream();$reader=[IO.StreamReader]::new($stream,[Text.Encoding]::ASCII,$false,1024,$true)
            $request=$reader.ReadLine();$headerBytes=$request.Length
            do {$line=$reader.ReadLine();$headerBytes+=$line.Length;if($headerBytes -gt 8192){throw 'Oversized request header'}}while($line)
            $name=if($request -match '^GET /(microbench-linux|Run-LinuxBenchmarks.sh) HTTP/1\.[01]$'){$Matches[1]}else{$null}
            if($name){$body=[IO.File]::ReadAllBytes((Join-Path $Directory $name));$status='200 OK'}
            else{$body=[Text.Encoding]::ASCII.GetBytes('Not found');$status='404 Not Found'}
            $header=[Text.Encoding]::ASCII.GetBytes("HTTP/1.1 $status`r`nContent-Length: $($body.Length)`r`nConnection: close`r`nContent-Type: application/octet-stream`r`n`r`n")
            $stream.Write($header,0,$header.Length);$stream.Write($body,0,$body.Length);$stream.Flush()
            Write-Output ([ordered]@{utc=[DateTime]::UtcNow.ToString('o');request=$request;status=$status;bytes=$body.Length} | ConvertTo-Json -Compress)
        }finally{$client.Dispose()}
    }
}finally{$listener.Stop()}
