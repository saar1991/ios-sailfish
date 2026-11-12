#!/bin/bash
# wp210: build sweetgl.dylib with Mesa softpipe (CPU rendering, no GPU driver)
# for iOS 16 arm64. Runs on macOS with Xcode (GitHub Actions: .github/workflows/sweetgl-sw.yml).
#
#   MESA_SRC=path/to/mesa tools/sweetgl/build-sw.sh      -> tools/sweetgl/out-sw/sweetgl.dylib
#
# Same frontend as build.sh (sweetgl.c, eagl.c, layerdump.c, dispatch), but
# sweetgl.c is compiled with -DSWEETGL_SOFTPIPE and sweet_sw.c replaces the
# msm emulation in sweet_drm.c.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
M=${MESA_SRC:?set MESA_SRC to a Mesa source tree}
B=${MESA_BUILD:-$H/../../research/mesa-ios-sw}
O=$H/out-sw
mkdir -p "$O"

SDK=$(xcrun -sdk iphoneos --show-sdk-path)
CC_=$(xcrun -sdk iphoneos -f clang)
CXX_=$(xcrun -sdk iphoneos -f clang++)
AR_=$(xcrun -sdk iphoneos -f ar)
STRIP_=$(xcrun -sdk iphoneos -f strip)
FLAGS="'-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0'"
cat > "$O/ios-cross.txt" <<CROSS
[binaries]
c = ['$CC_', $FLAGS]
cpp = ['$CXX_', $FLAGS]
objc = ['$CC_', $FLAGS]
ar = '$AR_'
strip = '$STRIP_'
pkg-config = 'false'

[built-in options]
c_args = ['-I$H/include']
cpp_args = ['-I$H/include']

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
CROSS

if [ ! -f "$B/build.ninja" ]; then
	(cd "$M" && meson setup "$B" --cross-file "$O/ios-cross.txt" \
		--buildtype=release -Db_ndebug=true -Ddefault_library=static \
		-Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
		-Dgallium-drivers=softpipe -Dvulkan-drivers= \
		-Dllvm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
		-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
		-Dbuild-tests=false -Dshader-cache=disabled -Dperfetto=false \
		-Dgallium-va=disabled -Dgallium-rusticl=false -Dtools= \
		-Dhtml-docs=disabled -Ddisplay-info=disabled -Dteflon=false)
fi

LIBS="src/mesa/libmesa.a src/gallium/auxiliary/libgallium.a
	src/gallium/drivers/softpipe/libsoftpipe.a
	src/compiler/glsl/libglsl.a src/compiler/glsl/glcpp/libglcpp.a src/compiler/nir/libnir.a
	src/compiler/spirv/libvtn.a src/compiler/libcompiler.a
	src/mesa/glapi/glapi/libglapi_bridge.a src/mesa/glapi/shared-glapi/libglapi.a
	src/util/libxmlconfig.a src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a
	src/c11/impl/libmesa_util_c11.a"
(cd "$B" && ninja $LIBS)
nm -g "$B/src/mesa/glapi/glapi/libglapi_bridge.a" > "$O/bridge.nm"

CC="xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0"
INC="-I$H/include -I$M/include -I$M/src -I$M/src/mesa -I$M/src/mapi -I$M/src/gallium/include
	-I$M/src/gallium/auxiliary -I$M/src/gallium/drivers -I$M/src/gallium/winsys
	-I$B/src -I$B/src/mesa -I$B/src/mapi"
python3 -I -c '
import json, shlex, sys
cc = json.load(open(sys.argv[1]))
e = [x for x in cc if x["file"].endswith("state_tracker/st_manager.c")][0]
print("\n".join(shlex.quote(a) for a in shlex.split(e["command"]) if a.startswith("-D")))
' "$B/compile_commands.json" > "$O/mesa-defines.rsp"
python3 -I "$H/gen-dispatch.py" "$H/ios-dispatch.txt" "$M/include" "$O/bridge.nm" "$O/dispatch.c"
for f in sweetgl sweet_sw eagl layerdump "$O/dispatch"; do
	case $f in /*) src=$f.c ;; *) src=$H/$f.c ;; esac
	$CC -O2 -Wall -fvisibility=hidden -DSWEETGL_SOFTPIPE @"$O/mesa-defines.rsp" $INC -I"$H" -c "$src" -o "$O/$(basename "$f").o"
done
printf '_sweetgl_init\n_sweetgl_create\n_sweetgl_make_current\n_sweetgl_destroy\n_sweetgl_selftest\n' > "$O/exports.txt"
xcrun -sdk iphoneos clang++ -arch arm64 -miphoneos-version-min=16.0 -dynamiclib -dead_strip \
	-install_name /usr/lib/sweetgl.dylib -exported_symbols_list "$O/exports.txt" \
	-o "$O/sweetgl.dylib" "$O/sweetgl.o" "$O/sweet_sw.o" "$O/eagl.o" "$O/layerdump.o" "$O/dispatch.o" \
	$(for l in $LIBS; do echo "$B/$l"; done) \
	-framework IOKit -framework CoreFoundation -framework IOSurface -framework OpenGLES -framework CoreGraphics -lobjc -lc++ -lz
codesign -f -s - "$O/sweetgl.dylib"
ls -l "$O/sweetgl.dylib"
