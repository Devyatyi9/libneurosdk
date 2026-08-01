[CmdletBinding()]
param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$expectedVersion = '20.1.2'
$root = Split-Path -Parent $PSScriptRoot

if ($env:CLANG_FORMAT) {
    $clangFormat = $env:CLANG_FORMAT
} else {
    $defaultPath = 'C:\Tools\LLVM-20.1.2\bin\clang-format.exe'
    if (Test-Path -LiteralPath $defaultPath) {
        $clangFormat = $defaultPath
    } else {
        $command = Get-Command clang-format-20 -ErrorAction SilentlyContinue
        if (-not $command) {
            throw 'clang-format not found. Set CLANG_FORMAT to clang-format 20.1.2.'
        }
        $clangFormat = $command.Source
    }
}

if (-not (Test-Path -LiteralPath $clangFormat -PathType Leaf)) {
    throw "clang-format does not exist: $clangFormat"
}

$version = (& $clangFormat --version) -join ' '
if ($LASTEXITCODE -ne 0 -or $version -notmatch "\b$([regex]::Escape($expectedVersion))\b") {
    throw "Expected clang-format $expectedVersion, got: $version"
}

$extensions = '\.(c|h|cc|cpp|cxx|hpp)$'
$files = @(
    git -C $root ls-files |
        Where-Object {
            $_ -match $extensions -and
            $_ -notmatch '^vendor/' -and
            $_ -notmatch '^tests/interop/uWebSockets/'
        }
)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to list tracked source files.'
}
if ($files.Count -eq 0) {
    throw 'No tracked source files found.'
}

foreach ($file in $files) {
    $path = Join-Path $root $file
    if ($Check) {
        & $clangFormat --dry-run --Werror $path
    } else {
        & $clangFormat -i $path
    }
    if ($LASTEXITCODE -ne 0) {
        throw "clang-format failed for $file"
    }
}

$mode = if ($Check) { 'checked' } else { 'formatted' }
Write-Host "$($files.Count) source files $mode with clang-format $expectedVersion."
