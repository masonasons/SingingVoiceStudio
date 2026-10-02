# build_dectalk.ps1 -- build DECtalk's speech DLL and its US English
# dictionary from third_party\dectalk, and put them where the program looks
# for them: voices\dectalk\DECtalk.dll and voices\dectalk\dtalk_us.dic.
#
# DECtalk builds with Microsoft's compiler only, so it is not part of the
# CMake build; the program loads the DLL at run time (src/voices/dectalk.cpp).
#
#   powershell -ExecutionPolicy Bypass -File scripts\build_dectalk.ps1
#
# The copied DLL then has DECtalk's scale of sung notes retuned to concert
# pitch (see the end of this file); scripts/build_dectalk_mac.sh does the
# same to the macOS library.
#
# The build's own output stays inside third_party\dectalk\src\dapi\build. If
# the dictionary compiler cannot be built, the prebuilt dictionary that comes
# with DECtalk's emscripten port (an earlier build of the same word list) is
# used instead.
param(
    [string]$MSBuild = "",
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Src = Join-Path $Root "third_party\dectalk\src"
$Out = Join-Path $Root "voices\dectalk"
$Config = "Release - ENGLISH_US"

if (-not $MSBuild) {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    )
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($found) { $candidates = @($found) + $candidates }
    }
    foreach ($c in $candidates) { if (Test-Path $c) { $MSBuild = $c; break } }
}

if (-not $NoBuild) {
    if (-not $MSBuild) { throw "MSBuild was not found; install Visual Studio 2022 with C++ or pass -MSBuild <path>" }
    Write-Host "Building DECtalk ($Config, x64) with $MSBuild"
    # The two projects are built on their own rather than through the
    # solution, which would also build the sample programs. SolutionDir is
    # given because the dictionary compiler's post-build step finds the
    # dictionary source through it (it adds the backslash itself).
    #
    # The projects link the C runtime as a DLL (VCRUNTIME140.dll), which a
    # machine without the Visual C++ redistributable does not have. The
    # program itself is linked statically, so DECtalk is too: a props file,
    # written beside the build output and forced in after the C++ targets,
    # switches the runtime to the static one without touching DECtalk's own
    # project files. The DLL's project names the runtime's import libraries
    # (ucrt.lib, vcruntime.lib) in its link line, so that line is replaced
    # too, with the static libraries in their place. The DLL's entry point is
    # also changed from DECtalk's LibMain, which does nothing, to the C
    # runtime's own, which a static runtime needs to set itself up.
    $props = Join-Path $Src "dapi\build\svs_static_crt.props"
    New-Item -ItemType Directory -Force (Split-Path $props) | Out-Null
    Set-Content -Encoding utf8 $props @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup>
    <ClCompile>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
    </ClCompile>
    <Link>
      <AdditionalDependencies>$(CoreLibraryDependencies);winmm.lib;libcmt.lib;libucrt.lib;libvcruntime.lib</AdditionalDependencies>
      <EntryPointSymbol Condition="'$(ConfigurationType)'=='DynamicLibrary'">_DllMainCRTStartup</EntryPointSymbol>
    </Link>
  </ItemDefinitionGroup>
</Project>
'@
    foreach ($proj in @("dapi\src\DECtalk API.vcxproj", "dapi\src\Internal Dictionary Compiler.vcxproj")) {
        & $MSBuild (Join-Path $Src $proj) "/p:Configuration=$Config" "/p:Platform=x64" "/p:SolutionDir=$Src" `
            "/p:ForceImportAfterCppTargets=$props" /m /nologo /v:minimal
        if ($LASTEXITCODE -ne 0) {
            if ($proj -like "*DECtalk API*") { throw "DECtalk.dll did not build (MSBuild returned $LASTEXITCODE)" }
            Write-Warning "The dictionary did not build (MSBuild returned $LASTEXITCODE)"
        }
    }
}

$dll = Join-Path $Src "dapi\build\dectalk\x64\$Config\DECtalk.dll"
$dic = Join-Path $Src "dapi\build\dic\x64\$Config\dtalk_us.dic"
if (-not (Test-Path $dic)) {
    $dic = Join-Path $Root "third_party\dectalk\ports\emscripten\fs\dtalk_us.dic"
    Write-Host "No dictionary was built; using the prebuilt one at $dic"
}
if (-not (Test-Path $dll)) { throw "DECtalk.dll was not built (looked for $dll)" }
if (-not (Test-Path $dic)) { throw "dtalk_us.dic was not found" }

New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item -Force $dll (Join-Path $Out "DECtalk.dll")
Copy-Item -Force $dic (Join-Path $Out "dtalk_us.dic")
Write-Host "Copied DECtalk.dll and dtalk_us.dic to $Out"

# -- DECtalk's scale, retuned ------------------------------------------------
#
# DECtalk sings notes 1 to 37, C2 to C5, from a table of their pitches in
# tenths of a hertz (notetab, in ph\ph_romi.c; the US build links that one --
# the copy in p_us_rom_dectalk_1996m_43f.c is #if 0). The table is DECtalk's
# old scale with middle C at 256 Hz, and a clause sung in notes is multiplied
# by 4190/4096 on its way out (ph_drwt01.c: "Change from Middle C = 256 Hz to
# A = 440 Hz"), which brings it to concert pitch within two cents. What is
# then left is the pitch period: it is a whole number of quarter samples at
# 10 kHz, rescaled to quarter samples at 11025 Hz, so the pitches DECtalk can
# sing are a ladder whose rungs are 20 cents apart by B4, and the notes come
# out up to 16 cents sharp. DECtalk's vibrato (+-2.05 Hz) moves the pitch
# across the rungs, and what is heard is the average.
#
# So each note's value is chosen here, from the arithmetic DECtalk does, as
# the one whose sung pitch, averaged over a cycle of its vibrato, is nearest
# the equal-tempered note at A = 440 Hz. Every note then comes out within one
# cent, but A4 (+4) and B4 (+6), where the ladder is too coarse for the
# vibrato to reach a better average. The values are patched over the table in
# the copied DLL; DECtalk's own sources are not touched. The table must be
# found exactly once, as DECtalk's original or as already patched.
function Get-SungHz([int]$v) {
    # notetab value -> mean sung Hz over the vibrato (ph_drwt01.c,
    # linear_interp and pht0draw; vtm\vtm1.c for the period's rescaling)
    $cos = @(164, 163, 161, 158, 154, 148, 141, 132, 123, 112, 100, 86, 72, 56, 38, 20,
        0, -20, -38, -56, -72, -86, -100, -112, -123, -132, -141, -148, -154, -158, -161, -163,
        -164, -163, -161, -158, -154, -148, -141, -132, -123, -112, -100, -86, -72, -56, -38, -20,
        0, 20, 38, 56, 72, 86, 100, 112, 123, 132, 141, 148, 154, 158, 161, 163)
    $sum = 0.0
    foreach ($c in $cos) {
        $f = $v + [Math]::Floor($c / 8)                  # getcosine >> 3
        $f = [Math]::Max(500, [Math]::Min(5121, $f))     # LOWEST_F0, HIGHEST_F0
        $f = [Math]::Floor($f * 4190 / 4096)             # frac4mul(f0prime, 4190)
        $t0 = [Math]::Floor(400000 / $f)                 # T0 in quarter samples at 10 kHz
        $q = [Math]::Floor((18063 * $t0 + 8192) / 16384) # ... at 11025 Hz
        $sum += 44100.0 / $q
    }
    return $sum / $cos.Count
}
$original = @(640, 678, 718, 761, 806, 854, 905, 959, 1016, 1076, 1140, 1208,
    1280, 1356, 1437, 1522, 1613, 1709, 1810, 1918, 2032, 2152, 2280, 2416,
    2560, 2712, 2874, 3044, 3226, 3418, 3620, 3836, 4064, 4304, 4560, 4832, 5120)
$tuned = @()
for ($k = 1; $k -le 37; $k++) {
    $want = 440.0 * [Math]::Pow(2.0, ($k + 35 - 69) / 12.0)    # note k is MIDI 35 + k
    $nominal = [int][Math]::Round($want * 10 * 4096 / 4190)
    $best = $nominal; $err = 1e9
    for ($v = $nominal - 25; $v -le $nominal + 25; $v++) {
        $e = [Math]::Abs([Math]::Log((Get-SungHz $v) / $want)) + [Math]::Abs($v - $nominal) * 1e-9
        if ($e -lt $err) { $err = $e; $best = $v }
    }
    $tuned += $best
}
function Get-TableBytes($values) {
    $b = New-Object byte[] (2 * $values.Count)
    for ($i = 0; $i -lt $values.Count; $i++) {
        $b[2 * $i] = [byte]($values[$i] -band 0xFF)
        $b[2 * $i + 1] = [byte](($values[$i] -shr 8) -band 0xFF)
    }
    return , $b
}
function Find-All([byte[]]$hay, [byte[]]$needle) {
    $found = @()
    $first = $needle[0]
    $i = [Array]::IndexOf($hay, $first)
    while ($i -ge 0 -and $i -le $hay.Length - $needle.Length) {
        $match = $true
        for ($j = 1; $j -lt $needle.Length; $j++) {
            if ($hay[$i + $j] -ne $needle[$j]) { $match = $false; break }
        }
        if ($match) { $found += $i }
        $i = [Array]::IndexOf($hay, $first, $i + 1)
    }
    return , $found
}
$target = Join-Path $Out "DECtalk.dll"
$bytes = [System.IO.File]::ReadAllBytes($target)
$was = Find-All $bytes (Get-TableBytes $original)
$now = Find-All $bytes (Get-TableBytes $tuned)
if ($now.Count -eq 1 -and $was.Count -eq 0) {
    Write-Host "DECtalk's scale is already retuned to A = 440 Hz"
} elseif ($was.Count -eq 1 -and $now.Count -eq 0) {
    $new = Get-TableBytes $tuned
    [Array]::Copy($new, 0, $bytes, $was[0], $new.Length)
    [System.IO.File]::WriteAllBytes($target, $bytes)
    Write-Host ("Retuned DECtalk's scale to A = 440 Hz (notetab at file offset 0x{0:X})" -f $was[0])
} else {
    throw ("DECtalk's note table was found {0} times as built and {1} times retuned in $target; " +
        "it must be there exactly once, so the DLL cannot be retuned") -f $was.Count, $now.Count
}
