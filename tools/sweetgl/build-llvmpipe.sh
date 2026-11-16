#!/bin/bash
# wp210: build sweetgl.dylib with Mesa llvmpipe AND softpipe (CPU rendering)
# for iOS 16 arm64. Runs on an arm64 Mac with Xcode (GitHub Actions:
# .github/workflows/sweetgl-llvmpipe.yml).
#
#   MESA_SRC=path/to/mesa LLVM_IOS=path/to/llvm-ios tools/sweetgl/build-llvmpipe.sh
#       -> tools/sweetgl/out-lp/sweetgl.dylib
#
# Like build-sw.sh, plus -DSWEETGL_LLVMPIPE: llvmpipe by default, softpipe
# with SGLDRIVER=softpipe. LLVM_IOS is LLVM's static libraries built for
# iOS arm64, with a host llvm-config of the same configuration in its bin/
# (llvm-config reports paths relative to its own place).
#
# Too big for /usr/lib/sweetgl.dylib's space (17 MB): it goes into the
# software update ramdisk's (/usr/standalone/update/ramdisk/arm64SURamDisk.dmg,
# 108 MB in one extent), loaded from there by DYLD_INSERT_LIBRARIES.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
M=${MESA_SRC:?set MESA_SRC to a Mesa source tree}
B=${MESA_BUILD:-$H/../../research/mesa-ios-lp}
L=${LLVM_IOS:?set LLVM_IOS to the iOS LLVM install}
O=$H/out-lp
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
llvm-config = '$L/bin/llvm-config'

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
		-Dgallium-drivers=softpipe,llvmpipe -Dvulkan-drivers= \
		-Dllvm=enabled -Dshared-llvm=disabled -Dcpp_rtti=false -Ddraw-use-llvm=true \
		-Dgles1=disabled -Dgles2=enabled -Dopengl=true \
		-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
		-Dbuild-tests=false -Dshader-cache=disabled -Dperfetto=false \
		-Dgallium-va=disabled -Dgallium-rusticl=false -Dtools= \
		-Dhtml-docs=disabled -Ddisplay-info=disabled -Dteflon=false)
fi

LIBS="src/mesa/libmesa.a src/gallium/auxiliary/libgallium.a
	src/gallium/drivers/softpipe/libsoftpipe.a src/gallium/drivers/llvmpipe/libllvmpipe.a
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
for f in sweetgl sweet_sw eagl fastpath layerdump "$O/dispatch"; do
	case $f in /*) src=$f.c ;; *) src=$H/$f.c ;; esac
	$CC -O2 -Wall -fvisibility=hidden -DSWEETGL_SOFTPIPE -DSWEETGL_LLVMPIPE @"$O/mesa-defines.rsp" $INC -I"$H" -c "$src" -o "$O/$(basename "$f").o"
done
printf '_sweetgl_init\n_sweetgl_create\n_sweetgl_make_current\n_sweetgl_destroy\n_sweetgl_selftest\n' > "$O/exports.txt"
xcrun -sdk iphoneos clang++ -arch arm64 -miphoneos-version-min=16.0 -dynamiclib -dead_strip \
	-install_name /usr/lib/sweetgl.dylib -exported_symbols_list "$O/exports.txt" \
	-o "$O/sweetgl.dylib" "$O/sweetgl.o" "$O/sweet_sw.o" "$O/eagl.o" "$O/fastpath.o" "$O/layerdump.o" "$O/dispatch.o" \
	$(for l in $LIBS; do echo "$B/$l"; done) \
	"$L"/lib/libLLVM*.a \
	-framework IOKit -framework CoreFoundation -framework IOSurface -framework OpenGLES -framework CoreGraphics -lobjc -lc++ -lz
# The installed file must fit the ramdisk's 113246235 bytes: drop the local
# symbols, keep a copy with them for reading profiles.
cp "$O/sweetgl.dylib" "$O/sweetgl.unstripped.dylib"
xcrun -sdk iphoneos strip -x "$O/sweetgl.dylib"
codesign -f -s - "$O/sweetgl.dylib"
ls -l "$O/sweetgl.dylib"
