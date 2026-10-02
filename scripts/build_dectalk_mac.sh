#!/bin/sh
# build_dectalk_mac.sh -- build DECtalk's speech engine as a shared library on
# macOS, and put it where the program looks for it: voices/dectalk/libtts.dylib,
# with the US English dictionary voices/dectalk/dtalk_us.dic beside it.
#
#   sh scripts/build_dectalk_mac.sh
#
# DECtalk's own build is autotools, written for Linux and Digital Unix, and
# its macOS rules make the whole kit -- demos, GTK samples, installers. Only
# the engine is wanted here, so it is compiled directly with clang: the same
# source list and defines as DECtalk's own self-contained emscripten port
# (ports/emscripten/package.json), which has no dlopen of per-language
# modules, no audio device, and in-memory synthesis only. The sources are
# copied to build-deps/dectalk first so that the submodule stays as it is.
#
# This is 1990s C, so it is compiled in its own dialect (gnu89), with the one
# fix that modern clang insists on: DriverCallback in nt/disable_audio.c is
# declared with a pointer parameter and defined with a short.
#
# The dictionary is DECtalk's own word list compiled into its binary form. It
# is taken from, in order: voices/dectalk/dtalk_us.dic if it is already
# there, $SVS_DECTALK_DIC, a DECtalk for Apple checkout beside this one
# (github.com/masonasons/DECTalkApple), or the dist/ folder of a full
# DECtalk build. The format is the same on every platform.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
SRC=$ROOT/third_party/dectalk/src
WORK=$ROOT/build-deps/dectalk
OUT=$ROOT/voices/dectalk
MIN=${SVS_MACOS_MIN:-11.0}
ARCHS=${SVS_ARCHS:-$(uname -m)}

if [ ! -f "$SRC/dapi/src/api/ttsapi.c" ]; then
    echo "third_party/dectalk is empty: git submodule update --init" >&2
    exit 1
fi

echo "== copying DECtalk's sources to build-deps/dectalk"
mkdir -p "$WORK"
rsync -a --delete --exclude .git "$ROOT/third_party/dectalk/" "$WORK/"
DAPI=$WORK/src/dapi/src

# the one genuine type bug clang rejects (see the top of this file)
sed -i '' 's/UINT16 uFlags, HANDLE16 hDev,/UINT16 uFlags, HWAVEOUT hDev,/' "$DAPI/nt/disable_audio.c"
# the engine notes on stderr that no DECtalk.conf named a dictionary, just
# before it takes the name the program hands it; the note is nothing
sed -i '' 's|fprintf(stderr,"libtts.so: Using default dictionary name\\n");|;|' "$DAPI/lts/lsw_main.c"

# DECtalk's scale retuned to A = 440 Hz, as scripts/build_dectalk.ps1 does to
# the Windows DLL (it explains the values: they are the ones whose sung pitch,
# through DECtalk's own pitch arithmetic and averaged over its vibrato, is
# nearest each equal-tempered note). Here they are written into the copy of
# ph/ph_romi.c before it is compiled; the table must be there exactly once,
# with DECtalk's original values (or these, if the copy is already retuned).
perl -0777 -pi -e '
    my @orig = qw(640 678 718 761 806 854 905 959 1016 1076 1140 1208 1280 1356 1437
                  1522 1613 1709 1810 1918 2032 2152 2280 2416 2560 2712 2874 3044 3226
                  3418 3620 3836 4064 4304 4560 4832 5120);
    my @tuned = qw(640 678 718 761 806 853 904 958 1015 1075 1139 1206 1278 1353 1434
                   1519 1610 1704 1804 1913 2028 2146 2274 2407 2554 2704 2860 3026 3212
                   3411 3601 3812 4049 4277 4540 4808 5092);
    my $n = () = /const short notetab\[\]\s*=\s*\{[^}]*\}/g;
    die "notetab is in ph_romi.c $n times, not once\n" unless $n == 1;
    s#(const short notetab\[\]\s*=\s*\{)([^}]*)(\})#
        my ($head, $body, $tail) = ($1, $2, $3);
        $body =~ s{/\*.*?\*/}{}gs;
        my @v = $body =~ /(\d+)/g;
        die "notetab in ph_romi.c is not the table DECtalk came with\n"
            unless "@v" eq "@orig" or "@v" eq "@tuned";
        $head . "\n\t" . join(",\n\t", @tuned) . "\n" . $tail
    #e;
' "$DAPI/ph/ph_romi.c"

# lsw_main.c includes config.h, which autotools would have made; it only needs
# to exist (DECTALK_INSTALL_PREFIX has a fallback in dectalkf.h)
if [ ! -f "$WORK/src/config.h" ]; then
    cat > "$WORK/src/config.h" <<'EOF'
/* Minimal config.h for the clang build (scripts/build_dectalk_mac.sh). */
#define HAVE_ICONV 1
#define PACKAGE_NAME "DECtalk"
#define PACKAGE_TARNAME "dectalk"
#define PACKAGE_VERSION "svs"
#define PACKAGE_STRING "DECtalk svs"
#define PACKAGE_BUGREPORT "https://github.com/dectalk/dectalk/issues"
#define PACKAGE_URL "https://github.com/dectalk/dectalk"
EOF
fi

# the emscripten port's file set, relative to dapi/src
SOURCES="
api/crypt2.c api/decstd97.c api/ttsapi.c
cmd/cmd_init.c cmd/cmd_wav.c cmd/cm_char.c cmd/cm_cmd.c cmd/cm_copt.c cmd/cm_main.c
cmd/cm_pars.c cmd/cm_phon.c cmd/cm_text.c cmd/cm_util.c cmd/par_ambi.c cmd/par_char.c
cmd/par_dict.c cmd/par_pars.c cmd/par_rule.c
hlsyn/acxf1c.c hlsyn/brent.c hlsyn/circuit.c hlsyn/frame.c hlsyn/hlframe.c hlsyn/inithl.c
hlsyn/llinit.c hlsyn/log10table.c hlsyn/nasalf1x.c hlsyn/reson.c hlsyn/sample.c
hlsyn/sqrttable.c hlsyn/voice.c
kernel/services.c kernel/usa_init.c
lts/loaddict.c lts/lsa_adju.c lts/lsa_coni.c lts/lsa_fr.c lts/lsa_gr.c lts/lsa_ir.c
lts/lsa_it.c lts/lsa_ja.c lts/lsa_rtbi.c lts/lsa_rule.c lts/lsa_sl.c lts/lsa_sp.c
lts/lsa_task.c lts/lsa_us.c lts/lsa_util.c lts/lsw_main.c lts/ls_chari.c lts/ls_dict.c
lts/ls_homo.c lts/ls_math.c lts/ls_proc.c lts/ls_spel.c lts/ls_speli.c lts/ls_suff.c
lts/ls_suffi.c
osf/dtmmio.c osf/loadable.c osf/playstub.c
nt/dbgwins.c nt/disable_audio.c nt/mmalloc.c nt/opthread.c nt/pipe.c nt/spc.c
ph/phinit.c ph/phlog.c ph/phprint.c ph/ph_aloph.c ph/ph_claus.c ph/ph_draw.c ph/ph_drwt0.c
ph/ph_inton.c ph/ph_main.c ph/ph_romi.c ph/ph_setar.c ph/ph_sort.c ph/ph_syl.c
ph/ph_syntx.c ph/ph_task.c ph/ph_timng.c ph/ph_vdefi.c ph/ph_vset.c
vtm/playtone.c vtm/sync.c vtm/vtm.c vtm/vtmiont.c
"

DEFINES="-D_REENTRANT -DNOMME -DLTSSIM -DTTSSIM -DANSI -DBLD_DECTALK_DLL -DACCESS32
         -DTYPING_MODE -DOS_SIXTY_FOUR_BIT -DACNA -DDISABLE_AUDIO -DENGLISH -DENGLISH_US"
INCLUDES="-I$WORK/src -I$DAPI -I$DAPI/api -I$DAPI/cmd -I$DAPI/dic -I$DAPI/include
          -I$DAPI/kernel -I$DAPI/lts -I$DAPI/osf -I$DAPI/ph -I$DAPI/protos -I$DAPI/vtm
          -I$DAPI/nt"
# warnings off: the code is kept as its authors wrote it
FLAGS="-std=gnu89 -O2 -fPIC -w -fcommon -Wno-error=implicit-function-declaration
       -Wno-error=implicit-int -Wno-error=int-conversion
       -Wno-error=incompatible-function-pointer-types -mmacosx-version-min=$MIN"
ARCHFLAGS=""
for a in $(echo "$ARCHS" | tr ',;' '  '); do ARCHFLAGS="$ARCHFLAGS -arch $a"; done

OBJ=$WORK/obj
rm -rf "$OBJ"
mkdir -p "$OBJ" "$OUT"
echo "== compiling the engine ($ARCHS)"
OBJS=""
for src in $SOURCES; do
    o=$OBJ/$(echo "$src" | tr '/' '_').o
    clang -c $FLAGS $ARCHFLAGS $DEFINES $INCLUDES "$DAPI/$src" -o "$o" &
    OBJS="$OBJS $o"
    # a handful at a time
    while [ "$(jobs -p | wc -l)" -ge "$(sysctl -n hw.ncpu)" ]; do sleep 0.2; done
done
wait

echo "== linking voices/dectalk/libtts.dylib"
clang -dynamiclib $ARCHFLAGS -mmacosx-version-min=$MIN -o "$OUT/libtts.dylib" $OBJS \
    -install_name @rpath/libtts.dylib -lpthread -lm
# the entry points the program asks for by name must be there
for sym in TextToSpeechStartupExFonix TextToSpeechSpeak TextToSpeechSync \
           TextToSpeechOpenInMemory TextToSpeechAddBuffer TextToSpeechReturnBuffer; do
    if ! nm -g "$OUT/libtts.dylib" | grep -q " T _$sym\$"; then
        echo "libtts.dylib does not export $sym" >&2
        exit 1
    fi
done

# the dictionary
DIC=$OUT/dtalk_us.dic
if [ ! -f "$DIC" ]; then
    for c in "$SVS_DECTALK_DIC" \
             "$ROOT/../DECTalkApple/upstream/dist/dic/dtalk_us.dic" \
             "$ROOT/../DECTalkApple/Sources/DECtalkKit/Resources/dtalk_us.dic" \
             "$WORK/dist/dic/dtalk_us.dic"; do
        if [ -n "$c" ] && [ -f "$c" ]; then
            echo "== dictionary from $c"
            cp "$c" "$DIC"
            break
        fi
    done
fi
if [ ! -f "$DIC" ]; then
    echo "!! no dtalk_us.dic: put DECtalk's US English dictionary in voices/dectalk, or set SVS_DECTALK_DIC" >&2
    exit 1
fi
echo "== done: $(ls "$OUT")"
