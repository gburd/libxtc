{
  description = "xtc — high-performance async/concurrency runtime for C";

  inputs = {
    nixpkgs.url      = "github:NixOS/nixpkgs/nixos-24.11";
    flake-utils.url  = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = import nixpkgs { inherit system; };
      in {
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
          ];
          shellHook = ''
            echo "xtc dev shell ready."
            echo "  cd dist && autoreconf -i && cd .. && mkdir -p build_unix && cd build_unix && ../dist/configure && make check"
            echo "  meson setup build_meson && meson test -C build_meson"
          '';
        };
      });
}
