# build_all.ps1 -- build Singing Voice Studio with every engine, gather every
# voice's data beside it, and zip the result.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build_all.ps1
#
# What comes out:
#
#   dist\SingingVoiceStudio\               the program, ready to run from anywhere
#       SingingVoiceStudio.exe             the editor
#       svs.exe                            the same program on the command line
#       assets\                            VocalWriter 2.0's tables, voices, bank, dictionary
#       voices\dectalk\                    DECtalk.dll and its dictionary
#       voices\microsoft\                  Sam, Mike and Mary
#       licenses\                          each engine's licence
#   dist\SingingVoiceStudio-<date>.zip     the same folder, zipped
#
# Where the data comes from:
#
#   VocalWriter   assets\ in this tree, or the VocalWriter repository beside it
#   DECtalk       built from third_party\dectalk with Visual Studio (scripts\build_dectalk.ps1)
#   Microsoft     voices\microsoft in this tree, or the voices Windows has installed
#                 (Common Files\SpeechEngines\Microsoft\TTS\1033, or XP's Microsoft Shared\Speech\1033)
#   SSI-263       nothing: its phoneme ROM is compiled in
#
# An engine whose data cannot be found is left out of the folder with a warning
# rather than stopping the build; its voices are then listed as not installed.
# Use -Strict to make that an error instead.
#
# The zip carries KAE Labs' and Microsoft's data, so it is for your own
# machines -- not something to publish.
param(
    [switch]$SkipDectalk,     # use the DECtalk.dll already in voices\dectalk
    [switch]$SkipCompile,     # use the executables already in build\
    [switch]$Clean,           # rebuild the program from nothing
    [switch]$NoZip,
    [switch]$Strict,
    [string]$Msys = ""        # msys2's root, if it is not where scoop puts it
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dist = Join-Path $Root "dist"
$Out = Join-Path $Dist "SingingVoiceStudio"
$missing = @()

function Say($text) { Write-Host "== $text" -ForegroundColor Cyan }
function Warn($text) {
    Write-Host "!! $text" -ForegroundColor Yellow
    $script:missing += $text
}

# -- 1. DECtalk, with Visual Studio --------------------------------------------
if (-not $SkipDectalk) {
    Say "building DECtalk"
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_dectalk.ps1")
        if ($LASTEXITCODE -ne 0) { throw "build_dectalk.ps1 failed ($LASTEXITCODE)" }
    } catch {
        Warn "DECtalk did not build: $_"
    }
}

# -- 2. the program, with msys2's GCC --------------------------------------------
if (-not $SkipCompile) {
    if (-not $Msys) {
        foreach ($c in @("$env:USERPROFILE\scoop\apps\msys2\current", "C:\msys64", "C:\tools\msys64")) {
            if (Test-Path (Join-Path $c "usr\bin\bash.exe")) { $Msys = $c; break }
        }
    }
    $bash = Join-Path $Msys "usr\bin\bash.exe"
    if (-not (Test-Path $bash)) { throw "msys2 was not found; install it, or pass -Msys <its folder>" }
    Say "building the program with $Msys"
    # C:\a\b as msys spells it, /c/a/b
    function Unix($p) {
        $p = $p -replace '\\', '/'
        if ($p -match '^([A-Za-z]):(.*)$') { $p = "/" + $Matches[1].ToLower() + $Matches[2] }
        return $p
    }
    $unixRoot = Unix $Root
    $unixMingw = Unix (Join-Path $Msys "mingw64")
    $env:MSYSTEM = "MINGW64"
    $env:CHERE_INVOKING = "1"
    $env:MINGW = $unixMingw
    $env:SVS_CMAKE_ARGS = ""
    $cleanArg = if ($Clean) { "clean" } else { "" }
    & $bash -lc "cd '$unixRoot' && sh build.sh $cleanArg"
    if ($LASTEXITCODE -ne 0) { throw "the program did not build ($LASTEXITCODE)" }
}
foreach ($exe in @("SingingVoiceStudio.exe", "svs.exe")) {
    if (-not (Test-Path (Join-Path $Root "build\$exe"))) { throw "build\$exe is missing" }
}

# -- 3. the folder ---------------------------------------------------------------
Say "gathering everything into $Out"
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

function Put($from, $to) {
    $dest = Join-Path $Out $to
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Copy-Item -Force $from $dest
}

Put (Join-Path $Root "build\SingingVoiceStudio.exe") "SingingVoiceStudio.exe"
Put (Join-Path $Root "build\svs.exe") "svs.exe"
Put (Join-Path $Root "README.md") "README.md"
Put (Join-Path $Root "LICENSE") "LICENSE"
Put (Join-Path $Root "NOTICE") "NOTICE"

# VocalWriter: the four files the synthesiser and the dictionary read
$vw = @(
    "assets\VocalWriter.app\Contents\Resources\VocalWriter.rsrc",
    "assets\GMSpeech.rsrc",
    "assets\GMBank.rsrc",
    "assets\EnglishLex"
)
$vwRoot = $null
foreach ($c in @($Root, (Join-Path (Split-Path -Parent $Root) "VocalWriter"))) {
    if (Test-Path (Join-Path $c $vw[0])) { $vwRoot = $c; break }
}
if ($vwRoot) {
    foreach ($f in $vw) {
        $src = Join-Path $vwRoot $f
        if (Test-Path $src) { Put $src $f }
        else { Warn "VocalWriter's $f is missing (looked in $vwRoot)" }
    }
    $agreement = Join-Path $vwRoot "assets\License Agreement.rtf"
    if (Test-Path $agreement) { Put $agreement "licenses\VocalWriter License Agreement.rtf" }
} else {
    Warn "VocalWriter's files were not found: put them in assets\, as the VocalWriter repository lays them out"
}

# DECtalk
$dtDir = Join-Path $Root "voices\dectalk"
foreach ($f in @("DECtalk.dll", "dtalk_us.dic")) {
    $src = Join-Path $dtDir $f
    if (Test-Path $src) { Put $src "voices\dectalk\$f" }
    else { Warn "DECtalk's $f is missing from voices\dectalk" }
}

# Microsoft: a copy in this tree first, then what Windows has installed
$msDirs = @((Join-Path $Root "voices\microsoft"))
foreach ($base in @(${env:CommonProgramFiles(x86)}, $env:CommonProgramFiles)) {
    if ($base) {
        $msDirs += (Join-Path $base "SpeechEngines\Microsoft\TTS\1033")
        $msDirs += (Join-Path $base "Microsoft Shared\Speech\1033")
    }
}
$found = @{}
foreach ($voice in @("Sam", "Mike", "Mary")) {
    foreach ($d in $msDirs) {
        $spd = Join-Path $d "$voice.spd"
        if (Test-Path $spd) {
            # the names as the program looks for them, whatever case the installer used
            Put $spd "voices\microsoft\$voice.spd"
            $sdf = Join-Path $d "$voice.sdf"
            if (Test-Path $sdf) { Put $sdf "voices\microsoft\$voice.sdf" }
            $found[$voice] = $d
            break
        }
    }
    if (-not $found[$voice]) { Warn "Microsoft $voice's voice file ($voice.spd) was not found" }
}

# licences
$lic = @{
    "third_party\dectalk\LICENCE" = "licenses\DECtalk.txt";
    "third_party\ssi263-speech\LICENSE" = "licenses\SSI-263.txt";
    "third_party\ssi263-speech\third_party\casso\LICENSE" = "licenses\SSI-263 Casso.txt";
    "third_party\ms-sam-mike-mary-decomp\LICENSE" = "licenses\Microsoft Sam reconstruction.txt";
    "engine\README.md" = "licenses\VocalWriterC README.md"
}
foreach ($k in $lic.Keys) {
    $src = Join-Path $Root $k
    if (Test-Path $src) { Put $src $lic[$k] }
}
@"
This folder carries data that belongs to others, gathered from this machine:

  assets\            VocalWriter 2.0, Copyright (c) 2005 KAE Labs, all rights reserved
  voices\microsoft\  Microsoft Sam, Mike and Mary, Copyright Microsoft Corporation

It is for your own use. Do not publish it.
"@ | Set-Content -Encoding utf8 (Join-Path $Out "DATA-NOTICE.txt")

# -- 4. does it sing from where it is? --------------------------------------------
Say "checking the engines from the new folder"
# only the folder itself counts: not the source tree the program was built from
$env:SVS_ONLY_BESIDE = "1"
$status = & (Join-Path $Out "svs.exe") --version
Remove-Item Env:SVS_ONLY_BESIDE
$status | ForEach-Object { Write-Host "   $_" }
foreach ($line in $status) {
    if ($line -match "^(VocalWriter|DECtalk|SSI-263|Microsoft): " -and $line -notmatch "voices ready") {
        Warn "engine not ready: $line"
    }
}

if ($Strict -and $missing.Count) { throw "missing: $($missing -join '; ')" }

# -- 5. the zip ---------------------------------------------------------------------
if (-not $NoZip) {
    $zip = Join-Path $Dist ("SingingVoiceStudio-" + (Get-Date -Format "yyyy-MM-dd") + ".zip")
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Say "zipping"
    # An executable that has just been run is often still held for a moment by the virus
    # scanner, which Compress-Archive reports as access denied; so .NET's own zip writer,
    # tried a few times, keeping the folder itself as the top of the archive.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    for ($try = 1; ; $try++) {
        try {
            if (Test-Path $zip) { Remove-Item -Force $zip }
            [System.IO.Compression.ZipFile]::CreateFromDirectory($Out, $zip,
                [System.IO.Compression.CompressionLevel]::Optimal, $true)
            break
        } catch {
            if ($try -ge 5) { throw }
            Start-Sleep -Seconds 2
        }
    }
    $mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
    Say "wrote $zip ($mb MB)"
}

# The linker maps of this very build, kept beside the zip (not in it): a crash
# report names addresses, and only these maps turn them into functions.
#   python scripts\resolve_crash.py crash-....txt dist\maps-<date>\SingingVoiceStudio.map
$maps = Join-Path $Dist ("maps-" + (Get-Date -Format "yyyy-MM-dd"))
New-Item -ItemType Directory -Force $maps | Out-Null
foreach ($m in @("SingingVoiceStudio.map", "svs.map")) {
    $src = Join-Path $Root "build\$m"
    if (Test-Path $src) { Copy-Item -Force $src (Join-Path $maps $m) }
}
Say "kept the linker maps in $maps, for reading crash reports"

if ($missing.Count) {
    Write-Host ""
    Write-Host "Built, but without everything:" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
} else {
    Say "done: every engine is in it"
}
