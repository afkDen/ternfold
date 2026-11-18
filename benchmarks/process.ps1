# Local argv runner: binary streams, explicit cwd, bounded wait, no shell.
function Invoke-M16Process {
    param([string]$Executable,[string[]]$Arguments,[string]$Output,[string]$ErrorOutput)
    $m16Info = [Diagnostics.ProcessStartInfo]::new()
    $m16Info.FileName = $Executable
    $m16Info.WorkingDirectory = (Get-Location).Path
    $m16Info.UseShellExecute = $false
    $m16Info.CreateNoWindow = $true
    $m16Info.RedirectStandardOutput = $true
    $m16Info.RedirectStandardError = $true
    $m16Info.RedirectStandardInput = $true
    foreach ($m16Arg in $Arguments) { $m16Info.ArgumentList.Add($m16Arg) }
    $m16Out = [IO.File]::Create([IO.Path]::GetFullPath($Output))
    $m16Err = [IO.File]::Create([IO.Path]::GetFullPath($ErrorOutput))
    $m16Process = [Diagnostics.Process]::new()
    $m16Process.StartInfo = $m16Info
    $m16Watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        if (-not $m16Process.Start()) { throw 'process launch failed' }
        $m16Process.StandardInput.Close()
        $m16CopyOut = $m16Process.StandardOutput.BaseStream.CopyToAsync($m16Out)
        $m16CopyErr = $m16Process.StandardError.BaseStream.CopyToAsync($m16Err)
        [long]$m16Peak = 0
        while (-not $m16Process.WaitForExit(5)) {
            if ($m16Watch.ElapsedMilliseconds -gt 60000) {
                $m16Process.Kill($true)
                throw 'process timed out after 60 seconds'
            }
            $m16Process.Refresh()
            try { $m16Peak = [Math]::Max($m16Peak,$m16Process.PeakWorkingSet64) } catch { }
        }
        $m16CopyOut.GetAwaiter().GetResult()
        $m16CopyErr.GetAwaiter().GetResult()
        $m16Watch.Stop()
        [pscustomobject]@{ ExitCode=$m16Process.ExitCode; Milliseconds=$m16Watch.Elapsed.TotalMilliseconds;
            SampledPeakWorkingSetBytes=$(if ($m16Peak) { $m16Peak } else { $null }) }
    } finally {
        $m16Out.Dispose(); $m16Err.Dispose(); $m16Process.Dispose()
    }
}
