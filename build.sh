## configure
export PATH="$PATH:$(pwd)/toolchains/llvm-mingw-20231017-ucrt-macos-universal/bin"
meson setup --prefix $(pwd)/target --cross-file build-win64.txt --native-file build-osx.txt build
## build
meson compile -C build
## install
mkdir -p target/x86_64-windows
cp build/src/d3d10/d3d10core.dll target/x86_64-windows/d3d10core.dll
cp build/src/d3d11/d3d11.dll target/x86_64-windows/d3d11.dll
cp build/src/dxgi/dxgi.dll target/x86_64-windows/dxgi.dll
cp build/src/winemetal/winemetal.dll target/x86_64-windows/winemetal.dll
mkdir -p target/x86_64-unix
cp build/src/winemetal/unix/winemetal.so target/x86_64-unix/winemetal.so
mkdir -p target/bin
cp build/src/airconv/darwin/airconv target/bin/airconv