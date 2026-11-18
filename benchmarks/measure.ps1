param([string]$Compiler='', [ValidateRange(1,20)][int]$Repeats=3)
$ErrorActionPreference='Stop'
$m16Root=Split-Path -Parent $PSScriptRoot
Push-Location $m16Root
try {
    . ./benchmarks/process.ps1
    if (-not $Compiler) { $Compiler=(Resolve-Path '.uv-cache/native-toolchains/llvm-mingw-20261020-ucrt-x86_64/bin/clang.exe').Path }
    $env:PATH=(Split-Path -Parent $Compiler)+[IO.Path]::PathSeparator+$env:PATH
    $m16Build=Join-Path $m16Root 'build/m16-measure'
    New-Item -ItemType Directory -Path $m16Build -Force | Out-Null
    $m16Driver=(Resolve-Path build/ternfold.exe).Path
    $m16Flags=@('-std=c17','-Wall','-Wextra','-Werror','-pedantic','-Wconversion','-Wshadow',
        '-O0','-fno-fast-math','-ffp-contract=off','-Iruntime/include','-Iinclude','-Isrc/execution')
    $m16Runtime=@(Get-ChildItem runtime/src -Filter '*.c' | ForEach-Object { $_.FullName })+
        @('src/checked_numeric.c','src/execution/scalars.c','src/execution/trace.c',
          'src/execution/trace_value.c','src/execution/trace_io.c','src/execution/trace_artifact.c')
    $m16Rows=[Collections.Generic.List[object]]::new()
    for ($m16Repeat=0;$m16Repeat -lt $Repeats;++$m16Repeat) {
        $m16Order=if ($m16Repeat % 2) { @(1,0) } else { @(0,1) }
        foreach ($m16Level in $m16Order) {
            $m16Stem="$m16Build/repeat$m16Repeat-opt$m16Level"
            $m16Emit=Invoke-M16Process $m16Driver @('emit-c','benchmarks/workload.tfd','--opt',"$m16Level",'-o',"$m16Stem.c") "$m16Stem.emit-log" "$m16Stem.emit-err"
            if ($m16Emit.ExitCode) { throw "emission failed: $m16Stem" }
            $m16Compile=Invoke-M16Process $Compiler ($m16Flags+@("$m16Stem.c")+$m16Runtime+@('-o',"$m16Stem.exe")) "$m16Stem.cc-log" "$m16Stem.cc-err"
            if ($m16Compile.ExitCode) { throw "native compilation failed: $m16Stem" }
            # One unmeasured warmup, followed by one measured process per pair.
            $m16Warm=Invoke-M16Process ([IO.Path]::GetFullPath("$m16Stem.exe")) @() "$m16Stem.warm" "$m16Stem.warm-err"
            $m16Run=Invoke-M16Process ([IO.Path]::GetFullPath("$m16Stem.exe")) @() "$m16Stem.out" "$m16Stem.err"
            if ($m16Warm.ExitCode -or $m16Run.ExitCode -or [IO.File]::ReadAllText("$m16Stem.out") -cne "35001400000`n") {
                throw "workload result failed: $m16Stem"
            }
            $m16Rows.Add([pscustomobject]@{ Repeat=$m16Repeat; CoreOptLevel=$m16Level;
                FrontendOptimizeEmitMilliseconds=$m16Emit.Milliseconds; NativeCompileMilliseconds=$m16Compile.Milliseconds;
                NativeRunMilliseconds=$m16Run.Milliseconds; SampledPeakWorkingSetBytes=$m16Run.SampledPeakWorkingSetBytes;
                GeneratedCBytes=(Get-Item "$m16Stem.c").Length })
            Write-Output "measured repeat $m16Repeat opt$m16Level"
        }
    }
    $m16SourceHashes=[ordered]@{}
    foreach ($m16File in @('src/core/optimize.c','include/ternfold/core_optimize.h',
        'src/backend/lifetimes.c','src/backend/lifetimes.h','tests/test_optimization.c',
        'tests/test_lifetimes.c','src/backend/c_emit.c','src/compiler_main.c','benchmarks/measure.ps1','benchmarks/process.ps1')) {
        $m16SourceHashes[$m16File]=(Get-FileHash $m16File).Hash
    }
    $m16Metadata=[ordered]@{
        UTC=[DateTime]::UtcNow.ToString('o'); Platform=[Environment]::OSVersion.VersionString;
        Architecture=[Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString(); Compiler=(& $Compiler --version | Out-String).Trim();
        WorkloadSHA256=(Get-FileHash benchmarks/workload.tfd).Hash; CompilerSHA256=(Get-FileHash $Compiler).Hash;
        DriverSHA256=(Get-FileHash $m16Driver).Hash; ImplementationSHA256=$m16SourceHashes; Flags=$m16Flags; Repeats=$Repeats;
        Method='Production CLI; alternating Core level order; native -O0; one unmeasured warmup per binary; wall time includes process launch and wait; peak working set sampled every 5ms (OS process peak observed while alive); null means unavailable.';
        Samples=$m16Rows
    }
    $m16Metadata | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 "$m16Build/results-windows.json"
    $m16Rows | Format-Table -AutoSize
} finally { Pop-Location }
