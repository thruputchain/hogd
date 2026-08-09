#!/bin/sh

# Source this file with the absolute directory in which to install the runtime.
if [ "$#" -ne 1 ]; then
  echo "Usage: source $0 <runtime-dir>" >&2
  return 2
fi

HOG_GLIBC_2_43_ROOT=$1
HOG_GLIBC_2_43_VERSION='2.43+r22+g8362e8ce10b2-2'
glibc_package="glibc-${HOG_GLIBC_2_43_VERSION}-x86_64.pkg.tar.zst"
glibc_url="https://archive.archlinux.org/packages/g/glibc/${glibc_package}"
glibc_sha256='2c20828b3a571b272697671c90b1e3a8c426d6a7e7fb99a242099373f2710fe1'
glibc_package_path="${HOG_GLIBC_2_43_ROOT}/${glibc_package}"
complete_marker="${HOG_GLIBC_2_43_ROOT}/.complete-${glibc_sha256}"

HOG_GLIBC_2_43_LIBDIR="${HOG_GLIBC_2_43_ROOT}/usr/lib"
HOG_GLIBC_2_43_LOADER="${HOG_GLIBC_2_43_LIBDIR}/ld-linux-x86-64.so.2"

if [ ! -f "$complete_marker" ]; then
  mkdir -p "$HOG_GLIBC_2_43_ROOT" || return

  echo "Downloading glibc ${HOG_GLIBC_2_43_VERSION}"
  curl --fail --location --retry 3 \
    --output "$glibc_package_path" "$glibc_url" || return
  echo "${glibc_sha256}  ${glibc_package_path}" \
    | sha256sum --check --status || return
  bsdtar --extract --file "$glibc_package_path" \
    --directory "$HOG_GLIBC_2_43_ROOT" || return
  test -x "$HOG_GLIBC_2_43_LOADER" || return
  touch "$complete_marker" || return
fi

HOG_GLIBC_2_43_CMAKE_LINKER_FLAG="-DCMAKE_EXE_LINKER_FLAGS=-Wl,--dynamic-linker=${HOG_GLIBC_2_43_LOADER} -Wl,-rpath,${HOG_GLIBC_2_43_LIBDIR}"
HOG_GLIBC_2_43_LDFLAGS="-Wl,--dynamic-linker=${HOG_GLIBC_2_43_LOADER} -Wl,-rpath,${HOG_GLIBC_2_43_LIBDIR}"
HOG_GLIBC_2_43_RUSTFLAGS="-C link-arg=-Wl,--dynamic-linker=${HOG_GLIBC_2_43_LOADER} -C link-arg=-Wl,-rpath,${HOG_GLIBC_2_43_LIBDIR}"

export HOG_GLIBC_2_43_ROOT HOG_GLIBC_2_43_VERSION
export HOG_GLIBC_2_43_LIBDIR HOG_GLIBC_2_43_LOADER
export HOG_GLIBC_2_43_CMAKE_LINKER_FLAG
export HOG_GLIBC_2_43_LDFLAGS HOG_GLIBC_2_43_RUSTFLAGS
