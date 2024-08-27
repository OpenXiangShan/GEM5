# xieby1 2022.06.10
# place this file in gem5 src dir root
let
  name = "gem5-xa64";
  # pin pkgs to latest nixpkgs 22.05
  pkgs = import (fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/d881cf9fd64218a99a64a8bdae1272c3f94daea7.tar.gz";
    sha256 = "1jaghsmsc05lvfzaq4qcy281rhq3jlx75q5x2600984kx1amwaal";
  }) {};

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
  '';
}
