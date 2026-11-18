param([ValidateSet('check','sanitize')][string]$Mode='check',[string]$Compiler='')
$ErrorActionPreference='Stop'
$m16Root=Split-Path -Parent $PSScriptRoot
Push-Location $m16Root
try {
    . ./benchmarks/process.ps1
    if (-not $Compiler) { $Compiler=(Resolve-Path '.uv-cache/native-toolchains/llvm-mingw-20261020-ucrt-x86_64/bin/clang.exe').Path }
    $env:PATH=(Split-Path -Parent $Compiler)+[IO.Path]::PathSeparator+$env:PATH
    $m16Build=Join-Path $m16Root "build/m16-$Mode"
    New-Item -ItemType Directory -Path $m16Build -Force | Out-Null
    $m16Driver=(Resolve-Path $(if ($Mode -eq 'sanitize') { 'build/ternfold-san.exe' } else { 'build/ternfold.exe' })).Path
    $m16Flags=@('-std=c17','-Wall','-Wextra','-Werror','-pedantic','-Wconversion','-Wshadow',
        '-O0','-fno-fast-math','-ffp-contract=off','-Iruntime/include','-Iinclude','-Isrc/execution')
    if ($Mode -eq 'sanitize') {
        $m16Flags+=@('-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-g')
        $env:ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'; $env:UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'
    }
    $m16Runtime=@(Get-ChildItem runtime/src -Filter '*.c' | ForEach-Object { $_.FullName })+
        @('src/checked_numeric.c','src/execution/scalars.c','src/execution/trace.c',
          'src/execution/trace_value.c','src/execution/trace_io.c','src/execution/trace_artifact.c')
    $m16Cases=@(
        @{ Source='benchmarks/scalars.tfd'; Expected="42`n-3`n-1`n2.5e0`ntrue`n" },
        @{ Source='examples/intro/counter.tfd'; Expected="4`n" },
        @{ Source='examples/features/snapshot_swap.tfd'; Expected="9`n2`n" },
        @{ Source='examples/features/dependency_order.tfd'; Expected="14`n" },
        @{ Source='examples/features/guard_short_circuit.tfd'; Expected="first reason`n0`n" },
        @{ Source='examples/features/fold_and_closure.tfd'; Expected="46`n" },
        @{ Source='tests/fixtures/differential/numeric.tfd' },
        @{ Source='tests/fixtures/types/interpreter-candidate-order.tfd'; Fault='R001' },
        @{ Source='tests/fixtures/types/interpreter-late-guard.tfd'; Fault='R002' },
        @{ Source='tests/fixtures/types/interpreter-late-emission.tfd'; Fault='R002' },
        @{ Source='tests/fixtures/types/interpreter-negative-power.tfd'; Fault='R005' },
        @{ Source='tests/fixtures/differential/backend-edge-record-div-first.tfd'; Fault='R002' },
        @{ Source='tests/fixtures/differential/backend-edge-record-overflow-first.tfd'; Fault='R001' },
        @{ Source='benchmarks/lifetimes-fixture.tfd'; Expected="owned across resumption`n1`n" }
    )
    $m16Index=0
    foreach ($m16Case in $m16Cases) {
        $m16Prefix="$m16Build/pair-$m16Index"
        $m16Ref=Invoke-M16Process $m16Driver @('run',$m16Case.Source) "$m16Prefix.ref" "$m16Prefix.ref-err"
        $m16WantExit=if ($m16Case.Fault) { 3 } else { 0 }
        if ($m16Ref.ExitCode -ne $m16WantExit) { throw "reference failed: $($m16Case.Source)" }
        if ($m16Case.ContainsKey('Expected') -and [IO.File]::ReadAllText("$m16Prefix.ref") -cne $m16Case.Expected) {
            throw "hand-derived result failed: $($m16Case.Source)"
        }
        foreach ($m16Level in 0,1) {
            $m16Stem="$m16Prefix-opt$m16Level"
            $m16Emit=Invoke-M16Process $m16Driver @('emit-c',$m16Case.Source,'--opt',"$m16Level",'-o',"$m16Stem.c") "$m16Stem.emit-log" "$m16Stem.emit-err"
            if ($m16Emit.ExitCode) { throw "emission failed: $m16Stem" }
            $m16Compile=Invoke-M16Process $Compiler ($m16Flags+@("$m16Stem.c")+$m16Runtime+@('-o',"$m16Stem.exe")) "$m16Stem.cc-log" "$m16Stem.cc-err"
            if ($m16Compile.ExitCode) { throw "compile failed: $m16Stem (see cc-err)" }
            # Trace outputs are always fresh; generated launcher refuses overwrite.
            $m16Trace="$m16Stem.trace"
            if (Test-Path -LiteralPath $m16Trace) { Remove-Item -LiteralPath $m16Trace }
            if (Test-Path -LiteralPath "$m16Trace.origins") { Remove-Item -LiteralPath "$m16Trace.origins" }
            $m16Native=Invoke-M16Process ([IO.Path]::GetFullPath("$m16Stem.exe")) @('--trace',$m16Trace) "$m16Stem.out" "$m16Stem.err"
            if ($m16Native.ExitCode -ne $m16WantExit) { throw "native exit mismatch: $m16Stem" }
            if ((Get-FileHash "$m16Stem.out").Hash -ne (Get-FileHash "$m16Prefix.ref").Hash) { throw "output mismatch: $m16Stem" }
            if ($m16Case.Fault) {
                $m16NativeError=[IO.File]::ReadAllText("$m16Stem.err")
                $m16ReferenceError=[IO.File]::ReadAllText("$m16Prefix.ref-err")
                $m16Range=[regex]::Match($m16ReferenceError,'\[bytes [0-9]+\.\.[0-9]+\)').Value
                if (-not $m16Range -or -not $m16NativeError.Contains("error[$($m16Case.Fault)]") -or
                    -not $m16NativeError.Contains($m16Range)) { throw "first fault/span mismatch: $m16Stem" }
            }
        }
        foreach ($m16Suffix in @('out','err','trace','trace.origins')) {
            if ((Get-FileHash "$m16Prefix-opt0.$m16Suffix").Hash -ne (Get-FileHash "$m16Prefix-opt1.$m16Suffix").Hash) {
                throw "native opt0/opt1 $m16Suffix mismatch: $($m16Case.Source)"
            }
        }
        if ($m16Case.Source -eq 'benchmarks/lifetimes-fixture.tfd') {
            $m16Releases = @(0,1 | ForEach-Object {
                [regex]::Matches([IO.File]::ReadAllText("$m16Prefix-opt$_.c"),'tf_release\(&v\[').Count
            })
            if ($m16Releases[1] -le $m16Releases[0]) { throw 'Opt1 did not generate additional last-use releases' }
        }
        Write-Output "PASS three-way $($m16Case.Source)"
        ++$m16Index
    }
    $m16Target="$m16Build/preserved.c"
    [IO.File]::WriteAllText($m16Target,'preserve')
    foreach ($m16Negative in @(@{Source='benchmarks/cycle.tfd';Code='E404'},@{Source='benchmarks/duplicate-writer.tfd';Code='E402'})) {
        foreach ($m16Level in 0,1) {
            $m16Reject=Invoke-M16Process $m16Driver @('emit-c',$m16Negative.Source,'--opt',"$m16Level",'-o',$m16Target) "$m16Target.out" "$m16Target.err"
            if ($m16Reject.ExitCode -ne 1 -or -not [IO.File]::ReadAllText("$m16Target.err").Contains("[$($m16Negative.Code)]") -or
                [IO.File]::ReadAllText($m16Target) -cne 'preserve') { throw "graph rejection mismatch: $($m16Negative.Source)" }
        }
    }
    Write-Output "M16 $Mode three-way: $m16Index fixtures, $($m16Index * 2) native executions, 4 preserved graph rejections"
} finally { Pop-Location }
