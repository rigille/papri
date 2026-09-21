{
  description = "papri — a pure C project in the Verifiable C subset";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
  let
    systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];

    forAllSystems = nixpkgs.lib.genAttrs systems;

    # CompCert is unfree in nixpkgs (its license permits noncommercial use
    # only), and `clightgen` ships in that package. `make normalform` is the
    # gate that keeps the source inside the Verifiable C subset, so the shell
    # is not useful without it.
    pkgsFor = system: import nixpkgs {
      inherit system;
      config.allowUnfree = true;
    };

    papriFor = pkgs: pkgs.stdenv.mkDerivation {
      pname   = "papri";
      version = "0.1.0";
      src     = ./.;

      nativeBuildInputs = [ pkgs.gnumake pkgs.clang ];

      # clightgen is deliberately absent here: `nix build` produces the binary,
      # `nix flake check` runs the subset gate. Keeping them apart means a
      # release build does not drag in the CompCert closure.
      buildPhase   = "make CC=clang";
      checkPhase   = "make CC=clang test";
      doCheck      = true;
      installPhase = ''
        install -Dm755 build/papri      $out/bin/papri
        install -Dm644 build/libpapri.a $out/lib/libpapri.a
        install -Dm644 src/papri.h      $out/include/papri.h
      '';
    };
  in {
    devShells = forAllSystems (system:
    let pkgs = pkgsFor system; in {
      default = pkgs.mkShell {
        name = "papri";

        packages = [
          pkgs.clang               # -Wlarge-by-value-copy is clang-only
          pkgs.gnumake
          pkgs.coqPackages.compcert # clightgen — the `normalform` gate
          pkgs.diffutils
          pkgs.bear                # compile_commands.json for ccls
          pkgs.ccls
          pkgs.gdb
        ];

        shellHook = ''
          echo ""
          echo "┌─ papri ───────────────────────────────────────────"
          echo "│  make             — build lib + binary"
          echo "│  make test        — build and run the tests"
          echo "│  make lint        — grep gate: goto/volatile/varargs/specs"
          echo "│  make normalform  — clightgen gate: source is already normal"
          echo "│  make check       — lint + normalform + test"
          echo "│  make compiledb   — regenerate compile_commands.json"
          echo "│  make clean       — remove build artifacts"
          echo "└───────────────────────────────────────────────────"
          echo ""
        '';
      };
    });

    packages = forAllSystems (system:
    let pkgs = pkgsFor system; in rec {
      papri   = papriFor pkgs;
      default = papri;
    });

    checks = forAllSystems (system:
    let pkgs = pkgsFor system; in {
      subset = pkgs.runCommand "papri-subset" {
        nativeBuildInputs = [ pkgs.gnumake pkgs.clang pkgs.coqPackages.compcert pkgs.diffutils ];
      } ''
        cp -r ${./.} source
        chmod -R u+w source
        cd source
        make CC=clang lint normalform
        touch $out
      '';
    });
  };
}
