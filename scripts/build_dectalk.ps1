# build_dectalk.ps1 -- build DECtalk's speech DLL and its US English
# dictionary from third_party\dectalk, and put them where the program looks
# for them: voices\dectalk\DECtalk.dll and voices\dectalk\dtalk_us.dic.
#
# The program compiles DECtalk in by itself (cmake/engines/dectalk.cmake),
# and its dictionary too, which it takes from voices\dectalk if it is there:
# -DictionaryOnly builds just that. The DLL is for a program built with
# SVS_DECTALK_STATIC off, which loads it at run time (src/voices/dectalk.cpp),
# and for tests/engines/dectalk built the same way.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build_dectalk.ps1 [-DictionaryOnly]
#
# Both are built from a copy of DECtalk's sources in build-deps\dectalk. For
# the DLL, DECtalk's range of sung notes is widened and its scale retuned to
# concert pitch (see below), as cmake/engines/dectalk.cmake and
# scripts/build_dectalk_mac.sh do.
#
# The build's own output stays inside build-deps\dectalk\src\dapi\build. If
# the dictionary compiler cannot be built, the prebuilt dictionary that comes
# with DECtalk's emscripten port (an earlier build of the same word list) is
# used instead.
param(
    [string]$MSBuild = "",
    [switch]$NoBuild,
    [switch]$DictionaryOnly   # the dictionary alone, without the DLL
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dt = Join-Path $Root "third_party\dectalk\src"
$Src = Join-Path $Root "build-deps\dectalk\src"
$Out = Join-Path $Root "voices\dectalk"
$Config = "Release - ENGLISH_US"

# -- DECtalk's range, widened ------------------------------------------------
#
# C2 - C5 widened to C1 - G5, as in cmake/engines/dectalk.cmake (which says
# why). Edit-DtSource replaces every FROM in a file of the copy by its TO,
# byte for byte, and stops if one is missing.
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)
function Edit-DtSource([string]$unit, [string[]]$pairs) {
    $path = Join-Path $Src "dapi\src\$unit"
    $text = [System.IO.File]::ReadAllText($path, $Latin1)
    for ($i = 0; $i -lt $pairs.Count; $i += 2) {
        if ($text.IndexOf($pairs[$i], [System.StringComparison]::Ordinal) -lt 0) {
            throw "$unit is not as this script expects: no `"$($pairs[$i])`""
        }
        $text = $text.Replace($pairs[$i], $pairs[$i + 1])
    }
    [System.IO.File]::WriteAllText($path, $text, $Latin1)
}
function Set-DtRange {
    Edit-DtSource "ph\ph_drwt01.c" @(
        '#define HIGHEST_F0        5121', '#define HIGHEST_F0        7700',
        '#define LOWEST_F0          500', '#define LOWEST_F0          (pDph_t->f0mode == SINGING ? 300 : 500)',
        'else if (f0command >= 2000)', 'else if (f0command >= 2000 || (f0command >= 1988 && pDph_t->f0mode == SINGING))',
        'if (*psF0command <= 37)', 'if (*psF0command <= 44)',
        'notetab[*psF0command - 1]', 'notetab[*psF0command + (*psF0command < 0 ? 12 : 11)]')
    Edit-DtSource "ph\ph_sort.c" @('((*psCurr_f0%1000) <= 37)', '((*psCurr_f0%1000) <= 44)')
    Edit-DtSource "ph\ph_vdefi.c" @('50, 350,', '50, 500,')
}

# -- DECtalk's scale, retuned ------------------------------------------------
#
# DECtalk sings notes 1 to 37, C2 to C5, from a table of their pitches in
# tenths of a hertz (notetab, in ph\ph_romi.c; the US build links that one --
# the copy in p_us_rom_dectalk_1996m_43f.c is #if 0); widened, it holds
# notes -12 to -1 and 1 to 44, C1 to G5. The table is DECtalk's old scale
# with middle C at 256 Hz, and a clause sung in notes is multiplied by
# 4190/4096 on its way out (ph_drwt01.c: "Change from Middle C = 256 Hz to
# A = 440 Hz"), which brings it to concert pitch within two cents. What is
# then left is the pitch period: it is a whole number of quarter samples at
# 10 kHz, rescaled to quarter samples at 11025 Hz, so the pitches DECtalk can
# sing are a ladder whose rungs are 20 cents apart by B4 (31 by G5), and the
# notes come out up to 16 cents sharp. DECtalk's vibrato (+-2.05 Hz) moves
# the pitch across the rungs, and what is heard is the average.
#
# So each note's value is chosen here, from the arithmetic DECtalk does, as
# the one whose sung pitch, averaged over a cycle of its vibrato, is nearest
# the equal-tempered note at A = 440 Hz, among those whose vibrato still
# crosses a rung: on a single rung the pitch would hold still, with no
# vibrato at all; nor may the vibrato reach the lowest or highest F0, which
# would cut it off. The ladder is uneven, so the search goes 150 either
# side of the note's nominal value; every note then comes out within about a
# cent (below C2 within three). The values are written over the table in the
# copy of ph_romi.c, which must hold it exactly once, as DECtalk's original.
$cos = @(164, 163, 161, 158, 154, 148, 141, 132, 123, 112, 100, 86, 72, 56, 38, 20,
    0, -20, -38, -56, -72, -86, -100, -112, -123, -132, -141, -148, -154, -158, -161, -163,
    -164, -163, -161, -158, -154, -148, -141, -132, -123, -112, -100, -86, -72, -56, -38, -20,
    0, 20, 38, 56, 72, 86, 100, 112, 123, 132, 141, 148, 154, 158, 161, 163)
$LowestF0 = 300     # LOWEST_F0 when singing, as widened
$HighestF0 = 7700   # HIGHEST_F0, as widened
function Get-Periods([int]$v) {
    # notetab value -> the pitch period over a cycle of the vibrato, in
    # quarter samples at 11025 Hz (ph_drwt01.c, linear_interp and pht0draw;
    # vtm\vtm1.c for the period's rescaling)
    foreach ($c in $cos) {
        $f = $v + [Math]::Floor($c / 8)                  # getcosine >> 3
        $f = [Math]::Max($LowestF0, [Math]::Min($HighestF0, $f))
        $f = [Math]::Floor($f * 4190 / 4096)             # frac4mul(f0prime, 4190)
        $t0 = [Math]::Floor(400000 / $f)                 # T0 in quarter samples at 10 kHz
        [Math]::Floor((18063 * $t0 + 8192) / 16384)      # ... at 11025 Hz
    }
}
$original = @(640, 678, 718, 761, 806, 854, 905, 959, 1016, 1076, 1140, 1208,
    1280, 1356, 1437, 1522, 1613, 1709, 1810, 1918, 2032, 2152, 2280, 2416,
    2560, 2712, 2874, 3044, 3226, 3418, 3620, 3836, 4064, 4304, 4560, 4832, 5120)
function Get-Tuned {
    $tuned = @()
    foreach ($k in @(-12..-1) + @(1..44)) {
        $midi = if ($k -gt 0) { $k + 35 } else { $k + 36 }      # note 1 is C2, MIDI 36; -1 is B1
        $want = 440.0 * [Math]::Pow(2.0, ($midi - 69) / 12.0)
        $nominal = [int][Math]::Round($want * 10 * 4096 / 4190)
        $best = $nominal; $err = 1e9
        for ($v = $nominal - 150; $v -le $nominal + 150; $v++) {
            if ($v - 21 -lt $LowestF0 -or $v + 20 -gt $HighestF0) { continue }
            $periods = @(Get-Periods $v)
            if (@($periods | Sort-Object -Unique).Count -lt 2) { continue }
            $sum = 0.0
            foreach ($q in $periods) { $sum += 44100.0 / $q }      # mean sung Hz over the vibrato
            $e = [Math]::Abs([Math]::Log($sum / $periods.Count / $want)) + [Math]::Abs($v - $nominal) * 1e-9
            if ($e -lt $err) { $err = $e; $best = $v }
        }
        $tuned += $best
    }
    return , $tuned
}
function Set-DtScale($tuned) {
    $path = Join-Path $Src "dapi\src\ph\ph_romi.c"
    $text = [System.IO.File]::ReadAllText($path, $Latin1)
    $tables = [regex]::Matches($text, 'const short notetab\[\]\s*=\s*\{[^}]*\}')
    if ($tables.Count -ne 1) { throw "notetab is in ph_romi.c $($tables.Count) times, not once" }
    $bare = [regex]::Replace($tables[0].Value, '/\*.*?\*/', '', 'Singleline') -replace '^[^{]*\{', ''
    $values = @([regex]::Matches($bare, '\d+') | ForEach-Object { [int]$_.Value })
    if (($values -join ',') -ne ($original -join ',')) { throw "notetab in ph_romi.c is not the table DECtalk came with" }
    $table = "const short notetab[] =`r`n{`r`n`t" + ($tuned -join ",`r`n`t") + "`r`n}"
    $text = $text.Substring(0, $tables[0].Index) + $table + $text.Substring($tables[0].Index + $tables[0].Length)
    [System.IO.File]::WriteAllText($path, $text, $Latin1)
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
if (-not $DictionaryOnly) { $tuned = Get-Tuned }

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
    if (-not (Test-Path (Join-Path $Dt "dapi\src\api\ttsapi.c"))) { throw "third_party\dectalk is empty: git submodule update --init" }
    # The copy: dapi\src and the headers it includes from ..\.. . Only what
    # differs is copied, which also puts back the files changed last time.
    robocopy (Join-Path $Dt "dapi\src") (Join-Path $Src "dapi\src") /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "DECtalk's sources were not copied to $Src (robocopy returned $LASTEXITCODE)" }
    robocopy $Dt $Src *.h /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "DECtalk's headers were not copied to $Src (robocopy returned $LASTEXITCODE)" }
    if (-not $DictionaryOnly) {
        Set-DtRange
        Set-DtScale $tuned
    }
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
    # runtime's own, which a static runtime needs to set itself up. No debug
    # information is made, in the DLL or beside it.
    $props = Join-Path $Src "dapi\build\svs_static_crt.props"
    New-Item -ItemType Directory -Force (Split-Path $props) | Out-Null
    Set-Content -Encoding utf8 $props @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup>
    <ClCompile>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
      <DebugInformationFormat>None</DebugInformationFormat>
    </ClCompile>
    <Link>
      <AdditionalDependencies>$(CoreLibraryDependencies);winmm.lib;libcmt.lib;libucrt.lib;libvcruntime.lib</AdditionalDependencies>
      <EntryPointSymbol Condition="'$(ConfigurationType)'=='DynamicLibrary'">_DllMainCRTStartup</EntryPointSymbol>
      <GenerateDebugInformation>false</GenerateDebugInformation>
    </Link>
  </ItemDefinitionGroup>
</Project>
'@
    $projects = @("dapi\src\DECtalk API.vcxproj", "dapi\src\Internal Dictionary Compiler.vcxproj")
    if ($DictionaryOnly) { $projects = @($projects[1]) }
    foreach ($proj in $projects) {
        & $MSBuild (Join-Path $Src $proj) "/p:Configuration=$Config" "/p:Platform=x64" "/p:SolutionDir=$Src" `
            "/p:ForceImportAfterCppTargets=$props" /m /nologo /v:minimal
        if ($LASTEXITCODE -ne 0) {
            if ($proj -like "*DECtalk API*") { throw "DECtalk.dll did not build (MSBuild returned $LASTEXITCODE)" }
            if ($DictionaryOnly) { throw "The dictionary did not build (MSBuild returned $LASTEXITCODE)" }
            Write-Warning "The dictionary did not build (MSBuild returned $LASTEXITCODE)"
        }
    }
}

$dll = Join-Path $Src "dapi\build\dectalk\x64\$Config\DECtalk.dll"
$dic = Join-Path $Src "dapi\build\dic\x64\$Config\dtalk_us.dic"
if ($DictionaryOnly) {
    if (-not (Test-Path $dic)) { throw "dtalk_us.dic was not built (looked for $dic)" }
    New-Item -ItemType Directory -Force $Out | Out-Null
    Copy-Item -Force $dic (Join-Path $Out "dtalk_us.dic")
    Write-Host "Copied dtalk_us.dic to $Out"
    exit 0
}
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

# Built from the changed copy, the DLL holds the retuned table exactly once.
$bytes = [System.IO.File]::ReadAllBytes((Join-Path $Out "DECtalk.dll"))
$n = (Find-All $bytes (Get-TableBytes $tuned)).Count
if ($n -ne 1) {
    throw "DECtalk's retuned note table is in DECtalk.dll $n times, not once: build it again without -NoBuild"
}
Write-Host "DECtalk sings C1 to G5, its scale retuned to A = 440 Hz"
