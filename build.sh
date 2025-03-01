## configure
export PATH="$PATH:$(pwd)/toolchains/llvm-mingw-20231017-ucrt-macos-universal/bin"
INSTALL_PATH=/opt/stuff/wine/dxmt-target
meson setup \
  -Dnative_llvm_path=toolchains/llvm-darwin \
  -Dwine_install_path=/opt/stuff/wine/target \
  -Dwine_builtin_dll=true \
  -Denable_nvapi=true \
  -Denable_nvngx=true \
  --prefix $INSTALL_PATH \
  --cross-file build-win64.txt \
  --native-file build-osx.txt \
  --buildtype release build
## build
meson compile -C build
## install
mkdir -p $INSTALL_PATH/x86_64-windows
cp build/src/d3d10/d3d10core.dll $INSTALL_PATH/x86_64-windows/d3d10core.dll
cp build/src/d3d11/d3d11.dll $INSTALL_PATH/x86_64-windows/d3d11.dll
cp build/src/dxgi/dxgi.dll $INSTALL_PATH/x86_64-windows/dxgi.dll
cp build/src/winemetal/winemetal.dll $INSTALL_PATH/x86_64-windows/winemetal.dll
cp build/src/nvapi/nvapi64.dll $INSTALL_PATH/x86_64-windows/nvapi64.dll
cp build/src/nvngx/nvngx.dll $INSTALL_PATH/x86_64-windows/nvngx.dll
mkdir -p $INSTALL_PATH/x86_64-unix
cp build/src/winemetal/unix/winemetal.so $INSTALL_PATH/x86_64-unix/winemetal.so
mkdir -p $INSTALL_PATH/bin
cp build/src/airconv/darwin/airconv $INSTALL_PATH/bin/airconv
