# xieby1 2022.06.10
# place this file in gem5 src dir root
let
  name = "gem5-xa64";
  # pin pkgs to latest nixpkgs 22.05
  pkgs = import (fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/d881cf9fd64218a99a64a8bdae1272c3f94daea7.tar.gz";
    sha256 = "1jaghsmsc05lvfzaq4qcy281rhq3jlx75q5x2600984kx1amwaal";
  }) {};

  riscv-toolchain = pkgs.pkgsCross.riscv64.buildPackages.gcc;

  libCheckpointAlpha = pkgs.fetchFromGitHub {
    owner = "OpenXiangShan";
    repo = "LibCheckpointAlpha";
    rev = "c5c2fef74133fb2b8ef8642633f60e0996493f29"; # You might want to pin this to a specific commit
    sha256 = "sha256-Rxlv47QY273jbcSX/A1PuT7+2aCB2sVW32pL91G3BmI="; # Replace with the actual SHA256
  };

  softfloat = pkgs.fetchFromGitHub {
    owner = "ucb-bar";
    repo = "berkeley-softfloat-3";
    rev = "3b70b5d"; # You might want to pin this to a specific commit
    sha256 = "sha256-uBXfFgKuGixDIupetB/p421YmZM/AlBmJi4VgFOjbC0="; # Replace with the actual SHA256
  };

  nemu = pkgs.stdenv.mkDerivation {
    name = "nemu";
    src = pkgs.fetchgit {
      url = "https://github.com/OpenXiangShan/NEMU.git";
      rev = "4332a525";
      sha256 = "sha256-gaNBh/+gO/Lcsfj753Az7Ww0x6+mXvN+ewqGT/J0POw="; # Replace with the actual SHA256
    };

    nativeBuildInputs = with pkgs; [
      gnumake
      gcc
      git
      # Add any other dependencies required by NEMU
      zlib
      which
      # ccache
      gdb
      zstd
      readline
      ncurses
      pkg-config
      bison
      flex
      riscv-toolchain
    ];

    buildPhase = ''
      # Setup LibCheckpointAlpha
      mkdir -p resource/gcpt_restore
      cp -r ${libCheckpointAlpha}/* resource/gcpt_restore/

      # Setup berkeley-softfloat-3
      mkdir -p resource/softfloat/repo
      cp -r ${softfloat}/* resource/softfloat/repo/

      # Build NEMU
      echo "Starting build phase"
      echo "Current working directory: $PWD"
      export NEMU_HOME=$PWD
      export PATH=${pkgs.gcc}/bin:$PATH # Ensure gcc is in the PATH
      
      # Disable ccache
      export USE_CCACHE=
      export CCACHE_DISABLE=1

      # Set NEMU_HOME to the current build directory
      export NEMU_HOME=$PWD
      echo "NEMU_HOME set to: $NEMU_HOME"

      # Ensure all necessary directories exist
      mkdir -p tools/kconfig/build
      mkdir -p tools/fixdep/build
      mkdir -p build/obj-riscv64-nemu-interpreter-so

      # Build necessary tools
      echo "Building kconfig tools"
      echo "Host GCC: ${pkgs.gcc}/bin/gcc"
      echo "host g++: ${pkgs.gcc}/bin/g++"
      # make -C tools/kconfig name=conf
      make -C tools/kconfig name=conf CC="${pkgs.gcc}/bin/gcc" CXX="${pkgs.gcc}/bin/g++"

      echo "Building fixdep tool"
      make -C tools/fixdep CC="${pkgs.gcc}/bin/gcc" CXX="${pkgs.gcc}/bin/g++"

      # Ensure the fixdep tool is in the PATH
      export PATH=$PWD/tools/fixdep/build:$PATH

      # Build gcpt_restore
      echo "Building gcpt_restore"
      make -C resource/gcpt_restore -n
      make -C resource/gcpt_restore

      echo "Building NEMU ref_defconfig"
      make riscv64-gem5-ref_defconfig -n
      make riscv64-gem5-ref_defconfig

      # Ensure softfloat build directory has write permissions
      mkdir -p resource/softfloat/repo/build/Linux-x86_64-GCC
      chmod -R u+w resource/softfloat/repo/build

      make -j 100

      echo "Build phase completed"
    '';

    installPhase = ''
      mkdir -p $out/bin
      cp build/riscv64-nemu-interpreter-so $out/bin/
      mkdir -p $out/resource/gcpt_restore/build
      cp resource/gcpt_restore/build/gcpt.bin $out/resource/gcpt_restore/build/
    '';
  };

in pkgs.mkShell {
  inherit name;
  packages = with pkgs; [
    # 需要的各个包
    # for compilation
    stdenv.cc
    scons
    zlib
    m4
    gperftools
    protobuf
    libpng
    hdf5-cpp
    pkg-config
    pre-commit

    # for execution
    python3Packages.pydot

    # for commit
    python3Packages.pyyaml

    # for gem5-xs
    zstd
    sqlite
    boost
    git
  ];

  shellHook = ''
    # set python path for gem5
    echo "set PYTHONPATH"
    PYTHONPATH+=":$(realpath src/python)"
    PYTHONPATH+=":$(realpath site_scons)"
    export PYTHONPATH

    export gem5_home=$PWD

    export GCB_RESTORER=${nemu}/resource/gcpt_restore/build/gcpt.bin
    export GCBV_RESTORER=${nemu}/resource/gcpt_restore/build/gcpt.bin
    export GCB_REF_SO=${nemu}/bin/riscv64-nemu-interpreter-so
    export GCBV_REF_SO=${nemu}/bin/riscv64-nemu-interpreter-so
  '';
}
