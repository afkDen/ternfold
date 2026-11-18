param(
    [Parameter(Mandatory)][ValidatePattern('^v[0-9]+\.[0-9]+\.[0-9]+$')][string]$Tag,
    [Parameter(Mandatory)][string]$Compiler
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cc = (Get-Command $Compiler -ErrorAction Stop).Source
$pwsh = (Get-Process -Id $PID).Path
$destination = Join-Path $root "dist/$Tag"
if (Test-Path -LiteralPath $destination) { throw "Output already exists: $destination" }
$stage = Join-Path $root ('build/release-' + [guid]::NewGuid().ToString('N'))
$version = "ternfold-$($Tag.Substring(1))"
$source = Join-Path $stage "$version-source.zip"
$binary = Join-Path $stage "$version-windows-x64.zip"
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Push-Location $root
try {
    $commit = git rev-parse --verify "refs/tags/${Tag}^{commit}"
    if ($LASTEXITCODE -ne 0) { throw "No local tag $Tag" }
    & $pwsh -NoProfile -File (Join-Path $PSScriptRoot 'verify-distribution.ps1') -Revision $commit
    if ($LASTEXITCODE -ne 0) { throw 'Tagged source failed the distribution audit' }
    git archive -o $source --format=zip --prefix="$version/" $commit
    if ($LASTEXITCODE -ne 0) { throw 'Could not archive the tagged commit' }
    Expand-Archive -LiteralPath $source -DestinationPath $stage
    $fresh = Join-Path $stage $version
    & $pwsh -NoProfile -File (Join-Path $fresh 'scripts/build.ps1') release -Compiler $cc
    if ($LASTEXITCODE -ne 0) { throw 'Fresh source release build failed' }

    $starter = Join-Path $stage 'starter'
    New-Item -ItemType Directory -Path $starter | Out-Null
    $compilerExe = Join-Path $starter 'ternfold.exe'
    Copy-Item -LiteralPath (Join-Path $fresh 'build/ternfold.exe') -Destination $compilerExe
    Copy-Item -LiteralPath (Join-Path $fresh 'examples/intro/counter.tfd') -Destination $starter
    Copy-Item -LiteralPath (Join-Path $fresh 'examples/intro/hello.tfd') -Destination $starter
    @'
Run .\ternfold.exe run .\counter.tfd in PowerShell. It prints 4.
Run .\ternfold.exe --help for other commands.
For native builds, install LLVM-MinGW and unpack the source release.
Pass the unpacked source directory as --runtime-root when using build.
'@ | Set-Content -Encoding utf8 (Join-Path $starter 'README.txt')
    Compress-Archive -Path (Join-Path $starter '*') -DestinationPath $binary
    $smoke = Join-Path $stage 'fresh-binary'
    Expand-Archive -LiteralPath $binary -DestinationPath $smoke
    $shipped = Join-Path $smoke 'ternfold.exe'
    $inputProgram = Join-Path $smoke 'counter.tfd'
    $native = Join-Path $smoke 'counter.exe'
    $savedPath = $env:PATH
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $interpreted = & $shipped run $inputProgram
        if ($LASTEXITCODE -ne 0 -or ($interpreted -join "`n") -cne '4') { throw 'Packaged interpreter smoke failed' }
        & $shipped build $inputProgram --opt 1 --cc $cc --runtime-root $fresh -o $native
        if ($LASTEXITCODE -ne 0) { throw 'Packaged native build smoke failed' }
        $built = & $native
        if ($LASTEXITCODE -ne 0 -or ($built -join "`n") -cne '4') { throw 'Packaged native execution smoke failed' }
    } finally { $env:PATH = $savedPath }
    $sums = Join-Path $stage 'SHA256SUMS.txt'
    @($source,$binary) | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash.ToLowerInvariant(), [IO.Path]::GetFileName($_)
    } | Set-Content -Encoding ascii -LiteralPath $sums
    New-Item -ItemType Directory -Path $destination | Out-Null
    @($source,$binary,$sums) | ForEach-Object { Move-Item -LiteralPath $_ -Destination $destination }
    Write-Output "Release assets: $destination"
} finally { Pop-Location }
