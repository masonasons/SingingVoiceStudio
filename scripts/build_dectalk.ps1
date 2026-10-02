# build_dectalk.ps1 -- build DECtalk's speech DLL and its US English
# dictionary from third_party\dectalk, and put them where the program looks
# for them: voices\dectalk\DECtalk.dll and voices\dectalk\dtalk_us.dic.
#
# DECtalk builds with Microsoft's compiler only, so it is not part of the
# CMake build; the program loads the DLL at run time (src/voices/dectalk.cpp).
#
#   powershell -ExecutionPolicy Bypass -File scripts\build_dectalk.ps1
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
