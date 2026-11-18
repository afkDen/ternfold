param(
    [Parameter(Mandatory)][ValidateSet('source','parser','types','execution','native')][string]$Stage,
    [Parameter(Mandatory)][string]$Compiler
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cc = (Get-Command $Compiler -ErrorAction Stop).Source
$stages = @{
    source = @{ checkpoint='030c92d'; test='test_source_manager.c' }
    parser = @{ checkpoint='7d8e974'; test='test_parser.c' }
    types = @{ checkpoint='6a07965'; test='test_check.c' }
    execution = @{ checkpoint='66b3717'; test='test_interpreter_core.c' }
    native = @{ checkpoint='1213c3d'; test='test_c_emit.c' }
}
$chosen = $stages[$Stage]
$manifest = Get-Content -Raw -LiteralPath (Join-Path $root 'docs/history/checkpoints.json') | ConvertFrom-Json
$steps = @($manifest.checkpoints | Where-Object {
    $_.group -ceq 'conformance' -and $_.source_checkpoint.StartsWith($chosen.checkpoint,[StringComparison]::Ordinal)
})
if ($steps.Count -ne 1 -or $steps[0].commit -cnotmatch '^[0-9a-f]{40}$') { throw 'Missing historical conformance snapshot' }
$commit = $steps[0].commit
$workspace = Join-Path ([IO.Path]::GetTempPath()) ('tf-history-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $workspace | Out-Null
$archive = Join-Path $workspace 'source.zip'
$snapshot = Join-Path $workspace 'source'
git -C $root archive --format=zip --output=$archive $commit
if ($LASTEXITCODE -ne 0) { throw 'Fetch full history before running the stage check' }
Expand-Archive -LiteralPath $archive -DestinationPath $snapshot
New-Item -ItemType Directory -Path (Join-Path $snapshot 'build') | Out-Null
$sources = @(Get-ChildItem -LiteralPath (Join-Path $snapshot 'src') -Recurse -Filter '*.c' |
    Where-Object Name -NotIn @('main.c','compiler_main.c') | ForEach-Object FullName)
$test = Join-Path $snapshot ('tests/' + $chosen.test)
$binary = Join-Path $snapshot 'build/history-test.exe'
$flags = @('-std=c17','-Wall','-Wextra','-Werror','-pedantic','-Wconversion','-Wshadow',
    '-fno-fast-math','-ffp-contract=off','-Iinclude','-Isrc/execution','-Iruntime/include')
$oldPath = $env:PATH
Push-Location $snapshot
try {
    $env:PATH = (Split-Path -Parent $cc) + [IO.Path]::PathSeparator + $oldPath
    & $cc @flags @sources $test -o $binary
    if ($LASTEXITCODE -ne 0) { throw "Historical $Stage test did not compile" }
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "Historical $Stage test failed" }
    Write-Output "PASS historical $Stage at $commit; focused strict C17 test $($chosen.test), executed now"
} finally { Pop-Location; $env:PATH = $oldPath }
