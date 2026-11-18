param([ValidatePattern('^(HEAD|[0-9a-f]{40})$')][string]$Revision='HEAD')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$roots = @('.editorconfig','.gitattributes','.gitignore','README.md','CONTRIBUTING.md',
    'CHANGELOG.md','build.bat')
$directories = @('.github','benchmarks','docs','examples','experiments','include',
    'runtime','scripts','src','stdlib','tests','tools')
$tree = @(git -C $root -c core.quotepath=false ls-tree -r --name-only $Revision)
if ($LASTEXITCODE -ne 0 -or -not $tree.Count) { throw 'Cannot read the tagged tree' }
$denied = @($tree | Where-Object {
    (($_ -notmatch '/' -and $_ -cnotin $roots) -or
     ($_ -match '/' -and ($_ -split '/',2)[0] -cnotin $directories)) -or
    ($_ -match '/' -and
     (($_ -replace '^\.github/','') -replace '(^|/)\.gitignore$','$1ignore') -match '(^|/)\.') -or
    ($_ -match '/[^/]+\.md$' -and
     ([IO.Path]::GetFileName($_)) -cnotin @('README.md','PULL_REQUEST_TEMPLATE.md') -and
     ([IO.Path]::GetFileName($_)) -cmatch '[A-Z_]') -or
    $_ -match '^(build|dist|output|tmp|submission|docs/(proposal|verification))/' -or
    $_ -match '\.(pdf|docx|zip|exe|dll|pdb|pem|key)$'
})
if ($denied.Count) { throw ('Files outside the release boundary: ' + ($denied -join ', ')) }
foreach ($file in @('README.md','docs/language-spec.md','include/ternfold/lexer.h',
                   'src/compiler_main.c','runtime/src/ternfold_rt.c','tests/test_differential.c')) {
    if ($file -cnotin $tree) { throw "Required source is missing: $file" }
}
Write-Output "Verified distribution tree: $($tree.Count) tracked files"
