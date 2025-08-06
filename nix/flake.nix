{
  description = "gem5 - a modular platform for computer-system architecture research";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
        };

        python = pkgs.python310;
        pythonPackages = python.pkgs;

        # Create a scons based on python310
        # pybind used in current version of gem5 need python <= 3.10
        scons = pkgs.scons.override {
          python3Packages = pythonPackages;
        };

        # gem5 dependencies
        buildInputs = with pkgs; [
          boost
          cmake
          gflags
          glog
          gnum4
          gnumake
          gperftools
          gtest
          libelf
          libffi
          libpng
          libtool
          m4
          ninja
          pkg-config
          python
          pythonPackages.distutils
          pythonPackages.pydot
          pythonPackages.six
          (scons.override {python3Packages = pkgs.python310Packages;})
          sqlite
          swig
          zlib
          zstd # xs-gem5 zstd checkpoint

          # The following dependencies are optional and seldom used
          # protobuf # protobuf version is strictly enforced by gem5, not sure which version
          # hdf5
        ];

        # Development tools
        devTools = with pkgs; [

          # Build & Development Tools
          gdb
          mold # Faster linking
          # bintools # gold linker
          ccache
          git
          valgrind
          pythonPackages.pyupgrade
          pythonPackages.pyyaml
          glibcLocales

          # LSP
          pyright # Python LSP
          # ruff          # Python formatter
          clang-tools # C++
          nixd # LSP of This file
          nixpkgs-fmt # Nix formatter
        ];
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "gem5";
          version = "1.0.0";
          src = self;

          enableParallelBuilding = true;

          inherit buildInputs;

          patchPhase = ''
            patchShebangs --build util/cpt_upgrader.py
          '';


          # Disable the automatic cmake configuration
          dontUseCmakeConfigure = true;
          # Disable standard configure
          dontConfigure = true;

          # Not intended for official build flow
          # Hard-code RISCV and opt for now
          buildPhase = ''
            scons -j $NIX_BUILD_CORES build/RISCV/gem5.opt
          '';

          installPhase = ''
            mkdir -p $out/bin
            cp -a build/*/gem5.opt $out/bin
          '';
        };

        devShells.default = pkgs.mkShell.override
          {
            stdenv = pkgs.stdenvAdapters.useMoldLinker (pkgs.stdenvAdapters.overrideCC pkgs.stdenv pkgs.clang);
          }
          {
            inherit buildInputs;
            nativeBuildInputs = devTools;

            # Disable hardening, FORTIFY is causing problem when building debug version without optimization
            hardeningDisable = [ "all" ];

            # Prebuilt NEMU need libstdc++, give it libstdc++
            shellHook = ''
              export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath [
                pkgs.stdenv.cc.cc
              ]}
            '';
          };

        formatter = pkgs.nixpkgs-fmt;
      }
    );
}
