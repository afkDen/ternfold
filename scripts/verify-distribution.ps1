param([ValidatePattern('^(HEAD|[0-9a-f]{40})$')][string]$Revision='HEAD')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tree = @(git -C $root -c core.quotepath=false ls-tree -r --name-only $Revision)
if ($LASTEXITCODE -ne 0 -or -not $tree.Count) { throw 'Cannot read the tagged tree' }
$denied = @($tree | Where-Object {
    $_ -match '(^|/)(\.agents|\.codex|\.uv-cache|\.venv|\.env(\.[^/]+)?|AGENTS\.md|MASTER_IMPLEMENTATION\.md)(/|$)' -or
    $_ -match '^(build|dist|output|tmp|submission|docs/(proposal|verification))/' -or
    $_ -match '\.(pdf|docx|zip|exe|dll|pdb|pem|key)$'
})
if ($denied.Count) { throw ('Files outside the release boundary: ' + ($denied -join ', ')) }
foreach ($file in @('README.md','docs/language-spec.md','include/ternfold/lexer.h',
                   'src/compiler_main.c','runtime/src/ternfold_rt.c','tests/test_differential.c')) {
    if ($file -cnotin $tree) { throw "Required source is missing: $file" }
}
Write-Output "Verified distribution tree: $($tree.Count) tracked files"
