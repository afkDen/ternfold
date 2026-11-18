param(
    [Parameter(Mandatory=$true)][string]$Compiler,
    [switch]$Sanitize,
    [string]$Ternfold = ''
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Push-Location $root
try {
    $cc = (Resolve-Path -LiteralPath $Compiler).Path
    $env:PATH = (Split-Path -Parent $cc) + [IO.Path]::PathSeparator + $env:PATH
    $mode = if ($Sanitize) { 'sanitize' } else { 'strict' }
    $dir = Join-Path $PSScriptRoot ".build/$mode"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if (-not $Ternfold) {
        $Ternfold = if ($Sanitize) { 'build/ternfold-san.exe' } else { 'build/ternfold.exe' }
    }
    $tf = (Resolve-Path -LiteralPath $Ternfold).Path
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    function Invoke-Captured([string]$Exe, [string[]]$Arguments, [string]$Name) {
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = $Exe
        $info.WorkingDirectory = $root
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardInput = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        foreach ($arg in $Arguments) { $info.ArgumentList.Add($arg) }
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $info
        $stdout = [IO.MemoryStream]::new()
        $stderr = [IO.MemoryStream]::new()
        try {
            if (-not $process.Start()) { throw "Cannot launch $Exe" }
            $process.StandardInput.Close()
            $copyOut = $process.StandardOutput.BaseStream.CopyToAsync($stdout)
            $copyErr = $process.StandardError.BaseStream.CopyToAsync($stderr)
            if (-not $process.WaitForExit(60000)) {
                $process.Kill($true)
                $process.WaitForExit()
                throw "Timeout: $Name"
            }
            $copyOut.GetAwaiter().GetResult()
            $copyErr.GetAwaiter().GetResult()
            $outBytes = $stdout.ToArray()
            $errBytes = $stderr.ToArray()
            [IO.File]::WriteAllBytes((Join-Path $dir "$Name.out"), $outBytes)
            [IO.File]::WriteAllBytes((Join-Path $dir "$Name.err"), $errBytes)
            return @{ Exit=$process.ExitCode; Out=$outBytes; Err=$utf8.GetString($errBytes) }
        } finally { $stdout.Dispose(); $stderr.Dispose(); $process.Dispose() }
    }
    function Require([bool]$Condition, [string]$Message) {
        if (-not $Condition) { throw $Message }
    }
    function Exact([byte[]]$Actual, [byte[]]$Expected, [string]$Name) {
        Require ($Actual.Length -eq $Expected.Length) "$Name output length $($Actual.Length), expected $($Expected.Length)"
        for ($i=0; $i -lt $Actual.Length; ++$i) {
            Require ($Actual[$i] -eq $Expected[$i]) "$Name differs at byte $i"
        }
    }
    $prototype = Join-Path $dir 'test-composition.exe'
    $flags = @('-std=c17','-Wall','-Wextra','-Werror','-pedantic','-Wconversion','-Wshadow','-g')
    if ($Sanitize) {
        $flags += @('-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer')
    }
    $compiled = Invoke-Captured $cc ($flags + @('experiments/composition/prototype.c',
        'tests/experiments/test_composition.c','-o',$prototype)) 'prototype-compile'
    Require ($compiled.Exit -eq 0) "Prototype compile failed: $($compiled.Err)"
    $tested = Invoke-Captured $prototype @() 'prototype-tests'
    Require ($tested.Exit -eq 0 -and $tested.Err -eq '') "Prototype failed: $($tested.Err)"
    Write-Output $utf8.GetString($tested.Out)
    $oracle = Invoke-Captured $prototype @('--observations') 'bounded-oracle'
    Require ($oracle.Exit -eq 0 -and $oracle.Err -eq '') 'Bounded oracle failed'
    # The C prototype writes CRLF through the Windows CRT. These are expected
    # bytes for Ternfold's LF-only output, not normalization of an executor result.
    $expectedBounded = $utf8.GetBytes($utf8.GetString($oracle.Out).Replace("`r`n", "`n"))
    $cases = @(
        @{Name='bounded'; Expected=$expectedBounded},
        @{Name='partial-accept'; Text="R 122 1 0 0 0`nouter reject 0`nold 0 0`n"},
        @{Name='emission-order'; Text="A 0 0 1 2 7=1;7=1;7=2;7=2;`n7=2;7=2;7=1;7=1;`n"},
        @{Name='dependency'; Text="2`n1`n"},
        @{Name='assertion'; Text="7=1;`n7=1;`n7=1;`n7=1;`nrelationship reject 0 0`n"},
        @{Name='emission-before-candidate'; Fault='R002'; Anchor='1 div delta'; Offset=2; Width=3},
        @{Name='predicate-before-candidate'; Fault='R002'; Anchor='1 div delta'; Offset=2; Width=3},
        @{Name='reason-before-candidate'; Fault='R002'; Anchor='1 div delta'; Offset=2; Width=3},
        @{Name='late-emission'; Fault='R002'; Anchor='1 div delta'; Offset=2; Width=3},
        @{Name='reject-late-emission'; Fault='R002'; Anchor='1 div delta'; Offset=2; Width=3},
        @{Name='flattened-emission'; Fault='R001'; Anchor='9223372036854775807 + 1'; Offset=20; Width=1},
        @{Name='flattened-predicate'; Fault='R001'; Anchor='9223372036854775807 + 1'; Offset=20; Width=1},
        @{Name='flattened-reason'; Fault='R001'; Anchor='9223372036854775807 + 1'; Offset=20; Width=1},
        @{Name='flattened-reject'; Text="reject`n"}
    )
    foreach ($case in $cases) {
        $source = "tests/experiments/$($case.Name).tfd"
        # .NET relative paths use the process directory, which can differ
        # from PowerShell's location when this script is invoked by the verifier.
        $origin = [IO.Path]::GetFullPath((Join-Path $root $source))
        $reference = Invoke-Captured $tf @('run',$source,'--root','.', '--diagnostics-json') "$($case.Name)-reference"
        if ($case.Fault) {
            Require ($reference.Exit -eq 3) "$($case.Name) reference exit $($reference.Exit): $($reference.Err)"
            Exact $reference.Out @() "$($case.Name) reference fault"
            $json = $reference.Err | ConvertFrom-Json
            Require ($json.diagnostics.Count -eq 1 -and $json.diagnostics[0].code -eq $case.Fault) "$($case.Name) reference code"
            $primary = @($json.diagnostics[0].labels | Where-Object primary)
            Require ($primary.Count -eq 1) "$($case.Name) primary count"
            $text = [IO.File]::ReadAllText($origin, $utf8)
            $index = $text.IndexOf($case.Anchor, [StringComparison]::Ordinal)
            Require ($index -ge 0 -and $index -eq $text.LastIndexOf($case.Anchor, [StringComparison]::Ordinal)) 'Unique fault anchor required'
            $start = $utf8.GetByteCount($text.Substring(0,$index)) + $case.Offset
            $end = $start + $case.Width
            $span = $primary[0].span
            $spelling = if ($case.Fault -eq 'R002') { 'div' } else { '+' }
            Require ($span.start_byte -eq $start -and $span.end_byte -eq $end -and $span.source_text -eq $spelling) "$($case.Name) reference span"
            Require ([IO.Path]::GetFullPath($span.source) -eq $origin) "$($case.Name) reference origin"
        } else {
            Require ($reference.Exit -eq 0) "$($case.Name) reference failed: $($reference.Err)"
            $json = $reference.Err | ConvertFrom-Json
            Require (-not $json.has_errors -and $json.diagnostics.Count -eq 0) "$($case.Name) unexpected diagnostics"
            $expected = if ($case.ContainsKey('Expected')) { $case.Expected } else { $utf8.GetBytes($case.Text) }
            Exact $reference.Out $expected "$($case.Name) reference"
        }
        $binary = Join-Path $dir "$($case.Name).exe"
        $buildArgs = @('build',$source,'--root','.','-o',$binary,'--cc',$cc,'--runtime-root',$root)
        if ($Sanitize) { $buildArgs += '--sanitize' }
        $built = Invoke-Captured $tf $buildArgs "$($case.Name)-build"
        Require ($built.Exit -eq 0) "$($case.Name) native build failed: $($built.Err)"
        $native = Invoke-Captured $binary @() "$($case.Name)-native"
        Require ($native.Exit -eq $reference.Exit) "$($case.Name) native exit $($native.Exit)"
        Exact $native.Out $reference.Out "$($case.Name) native/reference"
        if ($case.Fault) {
            $lines = $native.Err.Split("`n")
            $message = if ($case.Fault -eq 'R002') { 'division or remainder by zero' } else { 'arithmetic overflow or nonfinite result' }
            Require ($lines.Length -ge 2 -and $lines[0] -eq "error[$($case.Fault)]: $message") "$($case.Name) native fault code"
            # Native diagnostics escape backslashes as hex bytes.
            $quoted = '"' + $origin.Replace('\','\x5c') + '"'
            Require ($lines[1].Contains($quoted) -and $lines[1].EndsWith("[bytes $start..$end)")) "$($case.Name) native source span: $($lines[1])"
            Require (-not $native.Err.Contains('AddressSanitizer') -and -not $native.Err.Contains('runtime error:')) 'Sanitizer report'
        } else { Require ($native.Err -eq '') "$($case.Name) native stderr: $($native.Err)" }
        Write-Output "PASS paired $($case.Name)"
    }
    $cycle = Invoke-Captured $tf @('check','tests/experiments/cycle.tfd','--diagnostics-json') 'cycle-check'
    Require ($cycle.Exit -eq 1 -and $cycle.Out.Length -eq 0) 'Cycle must reject statically'
    $diag = $cycle.Err | ConvertFrom-Json
    Require (@($diag.diagnostics | Where-Object code -eq 'E404').Count -ge 1) 'Expected E404 for lexical dead-branch cycle'
    $sentinel = Join-Path $dir 'cycle.exe'
    $sentinelBytes = $utf8.GetBytes('existing output')
    [IO.File]::WriteAllBytes($sentinel,$sentinelBytes)
    $rejected = Invoke-Captured $tf @('build','tests/experiments/cycle.tfd','-o',$sentinel,'--cc',$cc) 'cycle-build'
    Require ($rejected.Exit -eq 1 -and $rejected.Out.Length -eq 0) 'Cyclic native build must reject'
    Exact ([IO.File]::ReadAllBytes($sentinel)) $sentinelBytes 'Cycle preserves output'
    Write-Output "PASS ${mode}: 225 bounded observations, $($cases.Count) source/native pairs, static cycle and preserved output; no skips"
} finally { Pop-Location }
