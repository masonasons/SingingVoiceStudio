#!/bin/sh
# build_mac.sh -- build Singing Voice Studio for macOS with every engine,
# gather every voice's data into the bundle, check that it sings from there,
# sign it, put it on a disk image, and have Apple notarize it.
#
#   sh scripts/build_mac.sh                 everything, into dist/
#   sh scripts/build_mac.sh --no-sign       an unsigned bundle, to run here
#   sh scripts/build_mac.sh --no-notarize   signed, but not sent to Apple
#   sh scripts/build_mac.sh --clean         rebuild the program from nothing
#   sh scripts/build_mac.sh --strict        a missing engine stops the build
#
# What comes out:
#
#   dist/Singing Voice Studio.app                 the editor; svs inside it
#                                                 (Contents/MacOS/svs) is the
#                                                 same program on the command line
#   dist/SingingVoiceStudio-<date>-macos-<arch>.dmg   the same, on a disk image,
#                                                 signed, notarized and stapled
#
# Inside the bundle:
#
#   Contents/MacOS/SingingVoiceStudio, svs
#   Contents/Frameworks/libtts.dylib              DECtalk (scripts/build_dectalk_mac.sh)
#   Contents/Resources/assets/                    VocalWriter 2.0's tables, voices, bank, dictionary
#   Contents/Resources/voices/dectalk/            DECtalk's dictionary
#   Contents/Resources/voices/microsoft/          Sam, Mike and Mary
#   Contents/Resources/licenses/                  each engine's licence
#
# Where the data comes from:
#
#   VocalWriter   assets/ in this tree, or a VocalWriter Studio checkout beside it
#                 (../vocalwriter/assets, or ../VocalWriter/assets)
#   DECtalk       built here from third_party/dectalk, with clang
#   Microsoft     voices/microsoft in this tree (Sam.spd, Mike.spd, Mary.spd and
#                 the .sdf files beside them, from a Windows machine)
#   SSI-263       nothing: its phoneme ROM is compiled in
#
# What the build needs: Xcode's command line tools, CMake, Ninja and git.
# wxWidgets is fetched and built statically into build-deps/wx the first time,
# at the master commit CMakeLists.txt pins for Windows, so both platforms are
# built on the same wxWidgets; it is built again whenever that pin moves.
#
# Signing uses the one "Developer ID Application" identity in the keychain,
# or $SVS_SIGN_IDENTITY. Notarization needs either a notarytool keychain
# profile in $SVS_NOTARY_PROFILE (xcrun notarytool store-credentials), or an
# App Store Connect API key in $ASC_KEY_PATH, $ASC_KEY_ID and $ASC_ISSUER;
# $SVS_NOTARY_ENV names a file to source for these, and
# ~/.appstoreconnect/singingvoicestudio.env is sourced if it is there.
#
# The disk image carries KAE Labs' and Microsoft's data, so it is for your
# own machines -- not something to publish.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
DIST=$ROOT/dist
APP="$DIST/Singing Voice Studio.app"
# the one place the pin is written down is CMakeLists.txt
WX_COMMIT=$(sed -n 's/.*GIT_TAG \([0-9a-f]\{40\}\).*/\1/p' CMakeLists.txt | head -n 1)
ARCH=$(uname -m)
MIN=${SVS_MACOS_MIN:-11.0}

CLEAN=0; SIGN=1; NOTARIZE=1; STRICT=0; DMG=1
for a in "$@"; do
    case "$a" in
        --clean) CLEAN=1 ;;
        --no-sign) SIGN=0; NOTARIZE=0 ;;
        --no-notarize) NOTARIZE=0 ;;
        --no-dmg) DMG=0 ;;
        --strict) STRICT=1 ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done

MISSING=""
say() { printf '\033[36m== %s\033[0m\n' "$*"; }
warn() { printf '\033[33m!! %s\033[0m\n' "$*"; MISSING="$MISSING
  - $*"; }
die() { printf '\033[31m!! %s\033[0m\n' "$*" >&2; exit 1; }

for tool in cmake ninja clang curl git codesign hdiutil; do
    command -v $tool >/dev/null 2>&1 || die "$tool is not installed"
done

# -- 1. wxWidgets, statically, once per pin ---------------------------------------
[ -n "$WX_COMMIT" ] || die "no wxWidgets GIT_TAG commit found in CMakeLists.txt"
WX=$ROOT/build-deps/wx
if [ ! -x "$WX/bin/wx-config" ] || [ "$(cat "$WX/.commit" 2>/dev/null)" != "$WX_COMMIT" ]; then
    say "building wxWidgets $WX_COMMIT (master) into build-deps/wx"
    mkdir -p "$ROOT/build-deps"
    cd "$ROOT/build-deps"
    if [ ! -d wxWidgets/.git ]; then
        git clone -q https://github.com/wxWidgets/wxWidgets.git wxWidgets || die "could not fetch wxWidgets"
    fi
    git -C wxWidgets fetch -q origin "$WX_COMMIT" 2>/dev/null || git -C wxWidgets fetch -q origin
    git -C wxWidgets checkout -q "$WX_COMMIT" || die "wxWidgets has no commit $WX_COMMIT"
    # pcre, png, zlib, expat and the rest are submodules in a checkout
    git -C wxWidgets submodule update -q --init --recursive || die "could not fetch wxWidgets' submodules"
    rm -rf wx-build "$WX" && mkdir -p wx-build && cd wx-build
    # only what the program uses, every library built in, nothing from Homebrew
    "../wxWidgets/configure" --prefix="$WX" --disable-shared \
        --with-osx_cocoa --with-macosx-version-min=$MIN --disable-sys-libs \
        --with-libpng=builtin --with-libjpeg=builtin --with-libtiff=builtin --with-zlib=builtin \
        --with-expat=builtin --with-regex=builtin --without-liblzma \
        --disable-webview --disable-mediactrl --disable-stc --disable-html --disable-richtext \
        --disable-propgrid --disable-ribbon --disable-aui --disable-xrc --disable-debug \
        --disable-tests > configure.log 2>&1 || die "wxWidgets did not configure: see build-deps/wx-build/configure.log"
    make -j"$(sysctl -n hw.ncpu)" > make.log 2>&1 || die "wxWidgets did not build: see build-deps/wx-build/make.log"
    make install > install.log 2>&1
    echo "$WX_COMMIT" > "$WX/.commit"
    cd "$ROOT"
fi

# -- 2. DECtalk, with clang ------------------------------------------------------
if [ "$CLEAN" = 1 ] || [ ! -f voices/dectalk/libtts.dylib ] || [ ! -f voices/dectalk/dtalk_us.dic ]; then
    say "building DECtalk"
    if ! sh scripts/build_dectalk_mac.sh; then
        warn "DECtalk did not build; its voices will be listed as not installed"
    fi
fi

# -- 3. the program --------------------------------------------------------------
say "building the program"
[ "$CLEAN" = 1 ] && rm -rf build
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release ${SVS_CMAKE_ARGS} > /dev/null
cmake --build build
[ -x build/SingingVoiceStudio.app/Contents/MacOS/SingingVoiceStudio ] || die "build/SingingVoiceStudio.app is missing"
[ -x build/svs ] || die "build/svs is missing"

# -- 4. the bundle ----------------------------------------------------------------
say "gathering everything into $APP"
mkdir -p "$DIST"
rm -rf "$APP"
cp -R build/SingingVoiceStudio.app "$APP"
cp build/svs "$APP/Contents/MacOS/svs"
RES="$APP/Contents/Resources"
mkdir -p "$RES/licenses" "$APP/Contents/Frameworks"
put() { mkdir -p "$(dirname "$2")"; cp "$1" "$2"; }
put README.md "$RES/README.md"
put LICENSE "$RES/LICENSE"
put NOTICE "$RES/NOTICE"

# VocalWriter: the four files the synthesiser and the dictionary read
VW_ROOT=""
for c in "$ROOT" "$ROOT/../vocalwriter" "$ROOT/../VocalWriter"; do
    if [ -f "$c/assets/VocalWriter.app/Contents/Resources/VocalWriter.rsrc" ]; then VW_ROOT=$c; break; fi
done
if [ -n "$VW_ROOT" ]; then
    for f in assets/VocalWriter.app/Contents/Resources/VocalWriter.rsrc assets/GMSpeech.rsrc \
             assets/GMBank.rsrc assets/EnglishLex; do
        if [ -f "$VW_ROOT/$f" ]; then put "$VW_ROOT/$f" "$RES/$f"; else warn "VocalWriter's $f is missing (looked in $VW_ROOT)"; fi
    done
    [ -f "$VW_ROOT/assets/License Agreement.rtf" ] && put "$VW_ROOT/assets/License Agreement.rtf" "$RES/licenses/VocalWriter License Agreement.rtf"
else
    warn "VocalWriter's files were not found: put them in assets/, as the VocalWriter repository lays them out"
fi

# DECtalk: the engine in Frameworks, where a bundle keeps its libraries; the dictionary with the data
if [ -f voices/dectalk/libtts.dylib ]; then
    cp voices/dectalk/libtts.dylib "$APP/Contents/Frameworks/libtts.dylib"
else
    warn "DECtalk's libtts.dylib is missing from voices/dectalk"
fi
if [ -f voices/dectalk/dtalk_us.dic ]; then
    put voices/dectalk/dtalk_us.dic "$RES/voices/dectalk/dtalk_us.dic"
else
    warn "DECtalk's dictionary, dtalk_us.dic, is missing from voices/dectalk"
fi

# Microsoft: a copy in this tree
for v in Sam Mike Mary; do
    if [ -f "voices/microsoft/$v.spd" ]; then
        put "voices/microsoft/$v.spd" "$RES/voices/microsoft/$v.spd"
        [ -f "voices/microsoft/$v.sdf" ] && put "voices/microsoft/$v.sdf" "$RES/voices/microsoft/$v.sdf"
    else
        warn "Microsoft $v's voice file ($v.spd) is not in voices/microsoft"
    fi
done

# licences
put_if() { [ -f "$1" ] && put "$1" "$2"; return 0; }
put_if third_party/dectalk/LICENCE "$RES/licenses/DECtalk.txt"
put_if third_party/ssi263-speech/LICENSE "$RES/licenses/SSI-263.txt"
put_if third_party/ssi263-speech/third_party/casso/LICENSE "$RES/licenses/SSI-263 Casso.txt"
put_if third_party/ms-sam-mike-mary-decomp/LICENSE "$RES/licenses/Microsoft Sam reconstruction.txt"
put_if engine/README.md "$RES/licenses/VocalWriterC README.md"
cat > "$RES/DATA-NOTICE.txt" <<'EOF'
This application carries data that belongs to others, gathered from this machine:

  Contents/Resources/assets/            VocalWriter 2.0, Copyright (c) 2005 KAE Labs, all rights reserved
  Contents/Resources/voices/microsoft/  Microsoft Sam, Mike and Mary, Copyright Microsoft Corporation

It is for your own use. Do not publish it.
EOF

# -- 5. does it sing from where it is? --------------------------------------------
say "checking the engines from the new bundle"
# only the bundle itself counts: not the source tree the program was built from
STATUS=$(SVS_ONLY_BESIDE=1 "$APP/Contents/MacOS/svs" --version 2>&1)
echo "$STATUS" | sed 's/^/   /'
echo "$STATUS" | grep -E '^(VocalWriter|DECtalk|SSI-263|Microsoft): ' | grep -v 'voices ready' | while read -r line; do
    warn "engine not ready: $line"
done
TMP=$(mktemp -d)
cat > "$TMP/check.vws" <<'EOF'
{"format": "vocalwriter-studio", "version": 2, "bpm": 100, "tracks": [{"name": "Voice 1", "notes": [
 {"phonemes": ["d", "EY"], "pitch": 67, "beats": 1.0}, {"phonemes": ["z", "IY"], "pitch": 65, "beats": 1.0}]}]}
EOF
for voice in Robert "DECtalk Betty" SSI-263 "Microsoft Sam"; do
    if SVS_ONLY_BESIDE=1 "$APP/Contents/MacOS/svs" "$TMP/check.vws" -o "$TMP/check.wav" --voice "$voice" > "$TMP/render.log" 2>&1 \
       && [ "$(stat -f %z "$TMP/check.wav")" -gt 40000 ]; then
        echo "   $voice sings ($(stat -f %z "$TMP/check.wav") bytes)"
    else
        warn "$voice did not sing from the bundle: $(tail -1 "$TMP/render.log")"
    fi
    rm -f "$TMP/check.wav"
done
rm -rf "$TMP"

if [ "$STRICT" = 1 ] && [ -n "$MISSING" ]; then die "missing:$MISSING"; fi

# -- 6. signing -------------------------------------------------------------------
if [ "$SIGN" = 1 ]; then
    IDENTITY=${SVS_SIGN_IDENTITY:-$(security find-identity -v -p codesigning | grep 'Developer ID Application' | head -1 | sed 's/.*"\(.*\)".*/\1/')}
    [ -n "$IDENTITY" ] || die "no Developer ID Application identity in the keychain; set SVS_SIGN_IDENTITY, or pass --no-sign"
    say "signing as $IDENTITY"
    # the pieces first, then the bundle, which seals them; the hardened
    # runtime is what notarization expects, and nothing here needs an
    # exception from it (DECtalk is loaded from inside the bundle, signed
    # by the same team)
    [ -f "$APP/Contents/Frameworks/libtts.dylib" ] && codesign --force --timestamp --options runtime --sign "$IDENTITY" "$APP/Contents/Frameworks/libtts.dylib"
    codesign --force --timestamp --options runtime --sign "$IDENTITY" "$APP/Contents/MacOS/svs"
    codesign --force --timestamp --options runtime --sign "$IDENTITY" "$APP"
    codesign --verify --strict --deep -v "$APP" 2>&1 | sed 's/^/   /'
fi

# -- 7. notarization credentials ---------------------------------------------------
notarize() {
    # notarize <file> : submit, wait, and staple; fail with Apple's log if refused
    if [ -n "$SVS_NOTARY_PROFILE" ]; then
        set -- "$1" --keychain-profile "$SVS_NOTARY_PROFILE"
    else
        set -- "$1" --key "$ASC_KEY_PATH" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER"
    fi
    file=$1; shift
    say "notarizing $(basename "$file")"
    OUT=$(xcrun notarytool submit "$file" --wait "$@" 2>&1) || true
    echo "$OUT" | sed 's/^/   /'
    ID=$(echo "$OUT" | grep -m1 '  id:' | awk '{print $2}')
    if ! echo "$OUT" | grep -q 'status: Accepted'; then
        [ -n "$ID" ] && xcrun notarytool log "$ID" "$@" 2>&1 | sed 's/^/   /'
        die "Apple did not accept $(basename "$file")"
    fi
    # a zip cannot carry a ticket; the bundle inside it is stapled by the caller
    case "$file" in *.zip) ;; *) xcrun stapler staple "$file" | sed 's/^/   /' ;; esac
}
if [ "$NOTARIZE" = 1 ]; then
    [ -n "$SVS_NOTARY_ENV" ] && [ -f "$SVS_NOTARY_ENV" ] && . "$SVS_NOTARY_ENV"
    if [ -z "$SVS_NOTARY_PROFILE" ] && [ -z "$ASC_KEY_PATH" ] && [ -f "$HOME/.appstoreconnect/singingvoicestudio.env" ]; then
        . "$HOME/.appstoreconnect/singingvoicestudio.env"
    fi
    if [ -z "$SVS_NOTARY_PROFILE" ] && { [ -z "$ASC_KEY_PATH" ] || [ -z "$ASC_KEY_ID" ] || [ -z "$ASC_ISSUER" ]; }; then
        die "no notarization credentials: set SVS_NOTARY_PROFILE, or ASC_KEY_PATH, ASC_KEY_ID and ASC_ISSUER (or pass --no-notarize)"
    fi
    # the application first, on its own, so that the copy someone drags out
    # of the disk image carries its ticket and opens without asking Apple
    ZIP="$DIST/SingingVoiceStudio-app.zip"
    rm -f "$ZIP"
    ditto -c -k --keepParent "$APP" "$ZIP"
    notarize "$ZIP" && rm -f "$ZIP"
    xcrun stapler staple "$APP" | sed 's/^/   /'
    spctl -a -t exec -vv "$APP" 2>&1 | sed 's/^/   /'
fi

# -- 8. the disk image --------------------------------------------------------------
if [ "$DMG" = 1 ]; then
    IMAGE="$DIST/SingingVoiceStudio-$(date +%Y-%m-%d)-macos-$ARCH.dmg"
    say "making $(basename "$IMAGE")"
    STAGE=$(mktemp -d)
    cp -R "$APP" "$STAGE/"
    ln -s /Applications "$STAGE/Applications"
    cat > "$STAGE/Read Me.txt" <<EOF
Singing Voice Studio for macOS ($ARCH, macOS $MIN or later)

Drag Singing Voice Studio to Applications and open it. It sings with
VocalWriter 2.0 (87 voices), DECtalk (10), the SSI-263 (4) and Microsoft
Sam, Mike and Mary (33, with the SAPI 4 voice modes). Every engine's data is
inside the application; see DATA-NOTICE.txt and the licenses folder in
Contents/Resources.

The keys are VocalWriter Studio's, with Cmd for Ctrl and Option for Alt;
F1 lists them in Messages. Hearing one note is Option+H (Cmd+H hides the
program on a Mac).

The same program on the command line:

  "/Applications/Singing Voice Studio.app/Contents/MacOS/svs" song.vws -o song.wav
  "/Applications/Singing Voice Studio.app/Contents/MacOS/svs" --list-voices

Settings and the recovery copy of an unsaved song live in
~/Library/Application Support/Singing Voice Studio.

Not affiliated with KAE Labs, Microsoft or the owners of DECtalk.
EOF
    rm -f "$IMAGE"
    hdiutil create -volname "Singing Voice Studio" -srcfolder "$STAGE" -ov -format UDZO "$IMAGE" | tail -1 | sed 's/^/   /'
    rm -rf "$STAGE"
    if [ "$SIGN" = 1 ]; then
        codesign --force --timestamp --sign "$IDENTITY" "$IMAGE"
    fi
    if [ "$NOTARIZE" = 1 ]; then
        notarize "$IMAGE"
        spctl -a -t open --context context:primary-signature -vv "$IMAGE" 2>&1 | sed 's/^/   /'
    fi
    say "wrote $IMAGE ($(du -h "$IMAGE" | cut -f1 | tr -d ' '))"
fi

if [ -n "$MISSING" ]; then
    printf '\n\033[33mBuilt, but without everything:%s\033[0m\n' "$MISSING"
else
    say "done: every engine is in it"
fi
