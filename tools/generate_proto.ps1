$ErrorActionPreference = 'Stop'

$gameRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$workspaceRoot = (Resolve-Path -LiteralPath (Join-Path $gameRoot '..')).Path
$protoDir = Join-Path $gameRoot 'proto'
$protoFile = Join-Path $protoDir 'rts.proto'
$cppOut = Join-Path $gameRoot 'generated/cpp'
$csharpOut = Join-Path $workspaceRoot 'Assets/Scripts/Network/Generated'

if (-not (Test-Path -LiteralPath $protoFile)) {
    throw "proto file not found: $protoFile"
}

$protocPath = $null
$protoc = Get-Command protoc -ErrorAction SilentlyContinue
if ($protoc) {
    $protocPath = $protoc.Source
} else {
    $candidateRoots = @()
    if ($env:VCPKG_ROOT) {
        $candidateRoots += $env:VCPKG_ROOT
    }
    $candidateRoots += 'D:\cppsoft\vcpkg'

    foreach ($root in $candidateRoots) {
        $candidate = Join-Path $root 'installed/x64-windows/tools/protobuf/protoc.exe'
        if (Test-Path -LiteralPath $candidate) {
            $protocPath = $candidate
            break
        }
    }
}

if (-not $protocPath) {
    throw 'protoc not found. Install protobuf compiler first, then rerun this script.'
}

New-Item -ItemType Directory -Force -Path $cppOut | Out-Null
New-Item -ItemType Directory -Force -Path $csharpOut | Out-Null

& $protocPath `
    "--proto_path=$protoDir" `
    "--cpp_out=$cppOut" `
    "--csharp_out=$csharpOut" `
    $protoFile

if ($LASTEXITCODE -ne 0) {
    throw "protoc failed with exit code $LASTEXITCODE"
}

Write-Output "Generated C++ protobuf files:"
Write-Output "  $cppOut\rts.pb.h"
Write-Output "  $cppOut\rts.pb.cc"
Write-Output "Generated Unity C# protobuf file:"
Write-Output "  $csharpOut\Rts.cs"
