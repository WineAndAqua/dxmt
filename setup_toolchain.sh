#!/bin/sh

port installed llvm-15@+universal | grep llvm-15 > /dev/null
if [ $? -eq 1 ]; then
    echo
    echo "MacPort 'llvm-15' (universal) is not installed."
    echo "Run command:"
    echo "sudo port install llvm-15 +universal"
    echo
    exit 1;
fi

if [ ! `which winebuild` ]; then
    echo
    echo "Wine must be installed from sources and binaries added to PATH."
    echo
    exit 1
fi

mkdir -p ./toolchains

curl -SL https://github.com/mstorsjo/llvm-mingw/releases/download/20231017/llvm-mingw-20231017-ucrt-macos-universal.tar.xz > ./llvm.tar.xz

tar -zvxf ./llvm.tar.xz -C ./toolchains
rm ./llvm.tar.xz

export LLVM_TABLEGEN="/opt/local/libexec/llvm-15/bin/llvm-tblgen"

git clone --depth 1 --branch llvmorg-15.0.7 https://github.com/llvm/llvm-project.git toolchains/llvm-project
mkdir -p ./toolchains/llvm-build
cmake -B ./toolchains/llvm-build -S ./toolchains/llvm-project/llvm -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_INSTALL_PREFIX="$(pwd)/toolchains/llvm" \
  -DLLVM_HOST_TRIPLE=x86_64-w64-mingw32 \
  -DLLVM_ENABLE_ASSERTIONS=On \
  -DLLVM_ENABLE_ZSTD=Off \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD="" \
  -DLLVM_BUILD_TOOLS=Off \
  -DLLVM_TABLEGEN="$LLVM_TABLEGEN" \
  -DCMAKE_SYSROOT="$(pwd)/toolchains/llvm-mingw-20231017-ucrt-macos-universal" \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DBUG_REPORT_URL="https://github.com/3Shain/dxmt" \
  -DPACKAGE_VENDOR="DXMT" \
  -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off \
  -G Ninja
pushd ./toolchains/llvm-build
ninja
ninja install
popd

mkdir -p ./toolchains/llvm-darwin-build
cmake -B ./toolchains/llvm-darwin-build -S ./toolchains/llvm-project/llvm \
  -DCMAKE_INSTALL_PREFIX="$(pwd)/toolchains/llvm-darwin" \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DLLVM_HOST_TRIPLE=x86_64-apple-darwin \
  -DLLVM_ENABLE_ASSERTIONS=On \
  -DLLVM_ENABLE_ZSTD=Off \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD="" \
  -DLLVM_BUILD_TOOLS=Off \
  -DLLVM_TABLEGEN="$LLVM_TABLEGEN" \
  -DBUG_REPORT_URL="https://github.com/3Shain/dxmt" \
  -DPACKAGE_VENDOR="DXMT" \
  -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off \
  -G Ninja
pushd ./toolchains/llvm-darwin-build
ninja
ninja install
popd
