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

if ($env:CLANGD) {
    $clangd = $env:CLANGD
} else {
    $defaultPath = 'C:\Tools\LLVM-20.1.2\bin\clangd.exe'
    if (Test-Path -LiteralPath $defaultPath) {
        $clangd = $defaultPath
    } else {
        $command = Get-Command clangd-20 -ErrorAction SilentlyContinue
        if ($command) {
            $clangd = $command.Source
        }
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
    git -C $root ls-files --cached --others --exclude-standard |
        Where-Object {
            $_ -match $extensions -and
            $_ -notmatch '^vendor/' -and
            $_ -notmatch '^tests/websocket/interop/uWebSockets/'
        }
)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to list source files.'
}
if ($files.Count -eq 0) {
    throw 'No source files found.'
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

if ($Check) {
    if (-not $clangd -or -not (Test-Path -LiteralPath $clangd -PathType Leaf)) {
        throw 'clangd not found. Set CLANGD to clangd 20.1.2.'
    }
    $clangdVersion = (& $clangd --version) -join ' '
    if ($LASTEXITCODE -ne 0 -or
        $clangdVersion -notmatch "\b$([regex]::Escape($expectedVersion))\b") {
        throw "Expected clangd $expectedVersion, got: $clangdVersion"
    }

    $buildDirectory = Join-Path $root 'build'
    $compilationDatabase = Join-Path $buildDirectory 'compile_commands.json'
    if (-not (Test-Path -LiteralPath $compilationDatabase -PathType Leaf)) {
        throw "Compilation database not found: $compilationDatabase. Configure build/ with a Ninja generator and CMAKE_EXPORT_COMPILE_COMMANDS=ON."
    }

    $database = Get-Content -LiteralPath $compilationDatabase -Raw | ConvertFrom-Json
    $translationUnits = @(
        $database.file |
            ForEach-Object { [IO.Path]::GetFullPath($_) } |
            Where-Object {
                $_.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
                    [StringComparison]::OrdinalIgnoreCase) -and
                $_ -notmatch '[\\/]vendor[\\/]' -and
                $_ -notmatch '[\\/]tests[\\/]websocket[\\/]interop[\\/]uWebSockets[\\/]'
            } |
            Sort-Object -Unique
    )
    if ($translationUnits.Count -eq 0) {
        throw 'No project translation units found in build/compile_commands.json.'
    }

    foreach ($file in $translationUnits) {
        $output = (& $clangd "--check=$file" `
            "--compile-commands-dir=$buildDirectory" --log=verbose 2>&1) -join "`n"
        if ($output -match 'unused-includes') {
            $diagnostics = $output -split "`n" |
                Where-Object { $_ -match 'unused-includes|Included header' }
            throw "clangd reported unused includes for $file`n$($diagnostics -join "`n")"
        }
    }
    Write-Host "$($translationUnits.Count) translation units checked for unused includes with clangd $expectedVersion."
}

$mode = if ($Check) { 'checked' } else { 'formatted' }
Write-Host "$($files.Count) source files $mode with clang-format $expectedVersion."
