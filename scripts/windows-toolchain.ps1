param([string]$CacheDirectory = '')
$ErrorActionPreference = 'Stop'
if (-not $CacheDirectory) { $CacheDirectory = Join-Path $PSScriptRoot '../.uv-cache' }
$cache = [IO.Path]::GetFullPath($CacheDirectory)
$version = '20261020'
$name = "llvm-mingw-$version-ucrt-x86_64"
$compiler = Join-Path $cache "$name/bin/clang.exe"
if (Test-Path -LiteralPath $compiler -PathType Leaf) { return $compiler }
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$archive = Join-Path $cache "$name.zip"
$expected = 'fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd'
$ProgressPreference = 'SilentlyContinue'
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    Invoke-WebRequest -Uri "https://github.com/mstorsjo/llvm-mingw/releases/download/$version/$name.zip" -OutFile $archive
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant() -cne $expected) {
    throw "Toolchain archive has the wrong SHA-256: $archive"
}
Expand-Archive -LiteralPath $archive -DestinationPath $cache -Force
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw 'Toolchain did not contain clang.exe' }
return $compiler
