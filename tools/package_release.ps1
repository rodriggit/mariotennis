# Packages a Release build for GitHub Releases: the executable, the DLLs it loads, the
# user readme (docs/release/README.txt), the license and the third-party notices, zipped
# into dist/MarioTennisRecompiled-v<Version>-windows-x64.zip.
#
#   tools\package_release.ps1 -Version 0.1.0
#   tools\package_release.ps1 -Version 0.1.0 -BuildDir cmake-build-release -RuntimeDir ..\N64ModernRuntime -Rt64Dir ..\rt64
#
# Relative paths are taken from the repository root. The build must be the Release
# configuration (no console) and -Version must match port/src/main.cpp.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir = "cmake-build-release",
    [string]$RuntimeDir = "..\N64ModernRuntime",
    [string]$Rt64Dir = "..\rt64",
    # Only to test the packaging with a RelWithDebInfo build; never for a published zip.
    [switch]$AllowConsole
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function From-Root([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return $path }
    return [IO.Path]::GetFullPath((Join-Path $root $path))
}

$build = From-Root $BuildDir
$runtime = From-Root $RuntimeDir
$rt64 = From-Root $Rt64Dir

# The executable: single-config generators (Ninja) put it in the build folder,
# multi-config ones (Visual Studio) in Release/.
$exe = @((Join-Path $build "MarioTennisRecompiled.exe"), (Join-Path $build "Release\MarioTennisRecompiled.exe")) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) {
    throw "MarioTennisRecompiled.exe not found in $build or $build\Release. Build the Release configuration first."
}
$exeDir = Split-Path -Parent $exe

# Release links as a Windows GUI program (PE subsystem 2); Debug/RelWithDebInfo keep a console.
$bytes = [IO.File]::ReadAllBytes($exe)
$peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
$subsystem = [BitConverter]::ToUInt16($bytes, $peOffset + 24 + 68)
if ($subsystem -ne 2) {
    if (-not $AllowConsole) {
        throw "$exe has a console (PE subsystem $subsystem): package the Release configuration."
    }
    Write-Warning "$exe has a console (PE subsystem $subsystem): this zip is only a packaging test."
}

$main = Get-Content (Join-Path $root "port\src\main.cpp") -Raw
if ($main -notmatch 'Version::from_string\("([^"]+)"') {
    throw "Version string not found in port/src/main.cpp."
}
if ($Matches[1] -ne $Version) {
    throw "port/src/main.cpp has version $($Matches[1]), not $Version."
}

# The zip should match a tagged commit (GPL: the source of what is distributed).
$tag = "v$Version"
if (git -C $root status --porcelain) {
    Write-Warning "The working tree has uncommitted changes: the zip may not match any commit."
}
$head = git -C $root rev-parse HEAD
$tagCommit = git -C $root rev-parse -q --verify "refs/tags/$tag^{commit}"
if (-not $tagCommit) {
    Write-Warning "There is no tag $tag yet. Tag the commit you built (git tag $tag) and push it with the release."
} elseif ($tagCommit -ne $head) {
    Write-Warning "Tag $tag is $tagCommit, but the current commit is $head."
}

$name = "MarioTennisRecompiled-v$Version-windows-x64"
$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($file in @("MarioTennisRecompiled.exe", "SDL2.dll", "dxil.dll", "dxcompiler.dll")) {
    $src = Join-Path $exeDir $file
    if (-not (Test-Path $src)) { throw "$src is missing." }
    Copy-Item $src $stage
}
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
$readme = Get-Content (Join-Path $root "docs\release\README.txt") -Raw
$readme = $readme.Replace("{VERSION}", "v$Version").Replace("{TAG}", $tag)
Set-Content -Path (Join-Path $stage "README.txt") -Value $readme -NoNewline -Encoding utf8

# Third-party notices: the license of every component linked into the executable or
# shipped next to it. "header:" entries take the license comment at the top of a source file.
$components = @(
    @("N64ModernRuntime (GPL-3.0)", "https://github.com/N64Recomp/N64ModernRuntime", "$runtime\COPYING"),
    @("N64Recomp", "https://github.com/N64Recomp/N64Recomp", "$runtime\N64Recomp\LICENSE"),
    @("miniz (runtime)", "https://github.com/richgel999/miniz", "$runtime\thirdparty\miniz\LICENSE"),
    @("o1heap", "https://github.com/pavel-kirienko/o1heap", "$runtime\thirdparty\o1heap\LICENSE"),
    @("xxHash (runtime)", "https://github.com/Cyan4973/xxHash", "$runtime\thirdparty\xxHash\LICENSE"),
    @("concurrentqueue", "https://github.com/cameron314/concurrentqueue", "header:$runtime\thirdparty\concurrentqueue\concurrentqueue.h"),
    @("JSON for Modern C++ (runtime)", "https://github.com/nlohmann/json", "header:$runtime\thirdparty\json\json.hpp"),
    @("RT64", "https://github.com/rt64/rt64", "$rt64\LICENSE"),
    @("D3D12 Memory Allocator", "https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator", "$rt64\src\contrib\D3D12MemoryAllocator\LICENSE.txt"),
    @("Vulkan Memory Allocator", "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator", "$rt64\src\contrib\VulkanMemoryAllocator\LICENSE.txt"),
    @("Vulkan-Headers", "https://github.com/KhronosGroup/Vulkan-Headers", "$rt64\src\contrib\Vulkan-Headers\LICENSE.md"),
    @("volk", "https://github.com/zeux/volk", "$rt64\src\contrib\volk\LICENSE.md"),
    @("SPIRV-Cross", "https://github.com/KhronosGroup/SPIRV-Cross", "$rt64\src\contrib\spirv-cross\LICENSE"),
    @("re-spirv", "https://github.com/rt64/re-spirv", "$rt64\src\contrib\re-spirv\LICENSE"),
    @("ddspp", "https://github.com/redorav/ddspp", "$rt64\src\contrib\ddspp\LICENSE"),
    @("hlslpp", "https://github.com/redorav/hlslpp", "$rt64\src\contrib\hlslpp\LICENSE"),
    @("Dear ImGui", "https://github.com/ocornut/imgui", "$rt64\src\contrib\imgui\LICENSE.txt"),
    @("ImPlot", "https://github.com/epezent/implot", "$rt64\src\contrib\implot\LICENSE"),
    @("im3d", "https://github.com/john-chapman/im3d", "$rt64\src\contrib\im3d\LICENSE"),
    @("Native File Dialog Extended", "https://github.com/btzy/nativefiledialog-extended", "$rt64\src\contrib\nativefiledialog-extended\LICENSE"),
    @("stb", "https://github.com/nothings/stb", "$rt64\src\contrib\stb\LICENSE"),
    @("xxHash (RT64)", "https://github.com/Cyan4973/xxHash", "$rt64\src\contrib\xxHash\LICENSE"),
    @("Zstandard", "https://github.com/facebook/zstd", "$rt64\src\contrib\zstd\LICENSE"),
    @("miniz (RT64)", "https://github.com/richgel999/miniz", "header:$rt64\src\contrib\miniz\miniz.c"),
    @("utf8conv", "https://github.com/GiovanniDicanio/UnicodeConversions", "header:$rt64\src\contrib\utf8conv\utf8conv.h"),
    @("JSON for Modern C++ (RT64)", "https://github.com/nlohmann/json", "header:$rt64\src\contrib\json\json.hpp"),
    @("SDL2 (SDL2.dll)", "https://www.libsdl.org/", "$build\_deps\sdl2-src\COPYING.txt")
)

function Header-License([string]$path) {
    $lines = Get-Content $path -TotalCount 200
    $out = @()
    if ($lines[0].TrimStart().StartsWith("/*")) {
        foreach ($line in $lines) {
            $out += $line
            if ($line.Contains("*/")) { break }
        }
    } else {
        # Consecutive // lines; blank lines between them belong to the same comment.
        foreach ($line in $lines) {
            $trimmed = $line.Trim()
            if (($trimmed -ne "") -and -not $trimmed.StartsWith("//")) { break }
            $out += $line
        }
    }
    return ($out -join "`n")
}

$notices = @(
    "Third-party components in Mario Tennis Recompiled $tag",
    "",
    "The executable as a whole is under the GNU General Public License v3.0 (LICENSE.txt).",
    "It links or ships the components below, under their own licenses.",
    ""
)
foreach ($c in $components) {
    $title, $url, $source = $c
    if ($source.StartsWith("header:")) {
        $path = $source.Substring(7)
        if (-not (Test-Path $path)) { throw "License source not found: $path" }
        $text = Header-License $path
    } else {
        if (-not (Test-Path $source)) { throw "License file not found: $source" }
        $text = Get-Content $source -Raw
    }
    $notices += ("=" * 78), "$title - $url", ("=" * 78), "", $text.TrimEnd(), ""
}
$notices += ("=" * 78), "DirectX Shader Compiler (dxcompiler.dll, dxil.dll) - https://github.com/microsoft/DirectXShaderCompiler", ("=" * 78), "",
    "dxcompiler.dll is distributed under the University of Illinois/NCSA Open Source License",
    "(see LICENSE.TXT in the repository above). dxil.dll is distributed by Microsoft as a",
    "redistributable component of the DirectX Shader Compiler releases.", ""
Set-Content -Path (Join-Path $stage "THIRD_PARTY_NOTICES.txt") -Value ($notices -join "`n") -Encoding utf8

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()

Write-Host "Packaged $zip"
Get-ChildItem $stage | ForEach-Object { Write-Host ("  {0,-28} {1,12:N0} bytes" -f $_.Name, $_.Length) }
Write-Host "SHA-256: $hash"
exit 0
