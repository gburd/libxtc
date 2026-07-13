{
  description = "xtc -- high-performance async/concurrency runtime for C";

  inputs = {
    nixpkgs.url      = "github:NixOS/nixpkgs/nixos-24.11";
    flake-utils.url  = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        version =
          pkgs.lib.removeSuffix "\n" (builtins.readFile ./dist/version.in);

        # The actual library build.  xtc mandates an out-of-source build
        # driven from dist/configure; we create a build dir, configure
        # with --enable-shared, build, run the test suite, and install
        # into $out.  liburing is Linux-only.
        xtc = pkgs.stdenv.mkDerivation {
          pname = "xtc";
          inherit version;
          src = self;

          nativeBuildInputs = with pkgs; [ autoconf gcc gnumake pkg-config ];
          buildInputs = with pkgs; [ openssl ]
            ++ pkgs.lib.optional pkgs.stdenv.isLinux liburing
            # DPDK: optional userspace-networking backend, Linux-only.
            # Present so `configure --with-dpdk` can find libdpdk via
            # pkg-config; the default build does not require it.
            ++ pkgs.lib.optional pkgs.stdenv.isLinux dpdk;

          # configure.ac forbids configuring in the source root, so we
          # run it from a dedicated build directory.
          configurePhase = ''
            runHook preConfigure
            mkdir -p build_nix
            cd build_nix
            ../dist/configure \
              --prefix=$out \
              --enable-shared \
              --with-tls=auto
            runHook postConfigure
          '';

          buildPhase = ''
            runHook preBuild
            make -j$NIX_BUILD_CORES
            runHook postBuild
          '';

          doCheck = false;
          # The project's `make check` target mixes C unit tests with
          # shell meta-tests that need autoreconf, a writable source
          # tree, and host env vars (XTC_SRC_DIR) not present in the
          # Nix sandbox; run it from the devShell or CI instead.  The
          # packaging gate here is that the library builds and installs.

          installPhase = ''
            runHook preInstall
            make install
            runHook postInstall
          '';

          meta = with pkgs.lib; {
            description = "High-performance async/concurrency runtime for C";
            homepage    = "https://codeberg.org/gregburd/libxtc";
            license     = licenses.isc;
            platforms   = platforms.unix;
          };
        };
      in {
        packages.default = xtc;
        packages.xtc     = xtc;

        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            # Toolchain
            gcc14 clang_18 lld
            # Build systems
            autoconf automake libtool gnumake
            meson ninja pkg-config
            # Test / lint / doc
            shellcheck mandoc
            valgrind gdb lcov gcovr
            # Misc
            gawk
          ]
          # DPDK is Linux-only; make it available in the dev shell so
          # `configure --with-dpdk` can find it.  liburing too.
          ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ liburing dpdk ];
          shellHook = ''
            echo "xtc dev shell ready."
            echo "  cd dist && autoreconf -i && cd .. && mkdir -p build_unix && cd build_unix && ../dist/configure && make check"
            echo "  meson setup build_meson && meson test -C build_meson"
          '';
        };

        # Optional: tools/sim-monitor/, the VOPR-inspired DST trace
        # viewer.  NOT part of the default dev shell (it pulls in a
        # GUI toolkit -- raylib -- most contributors building the
        # library itself do not need) and NOT a dependency of the
        # library, make check, or the default build in any way; see
        # tools/sim-monitor/README.md.  Enter with:
        #   nix develop .#sim-monitor
        devShells.sim-monitor = pkgs.mkShell {
          packages = with pkgs; [
            gcc14 pkg-config gnumake
            raylib
          ];
          shellHook = ''
            echo "xtc sim-monitor dev shell ready (raylib $(pkg-config --modversion raylib))"
            echo "  cd tools/sim-monitor && make"
          '';
        };
      });
}
