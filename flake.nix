{
  description = "papri — a pure C project in the Verifiable C subset";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  # The CertiCoq generational collector, as carried by CertiGraph. Only the
  # C is built here; the Coq development that proves it is not.
  #
  # Packaged but NOT linked. It is a verified collector, which is the reason
  # to want it, and the wrong shape for a long-running editor, which is the
  # reason papri uses Boehm instead: its generation count is bounded, it has
  # no way to collect the oldest generation (its own author's REMARK at the
  # foot of gc.c says so), and it calls exit(1) when it runs out. That is
  # fine for a CertiCoq program, which computes an answer and stops. An
  # editor open for a week is the case it was not written for. Kept here
  # because the day it grows an oldest-generation collection it becomes the
  # better choice, and because the packaging is the hard part to redo.
  inputs.certigraph.url =
    "github:CertiGraph/CertiGraph/8781550d8a116abb03ac7931f271ce03a5158a74";
  inputs.certigraph.flake = false;

  outputs = { self, nixpkgs, certigraph }:
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

    # The collector, built as a plain static library with its headers.
    certigcFor = pkgs: pkgs.stdenv.mkDerivation {
      pname   = "certigc";
      version = "0-unstable-2026";
      src     = certigraph;

      nativeBuildInputs = [ pkgs.clang ];

      buildPhase = ''
        cd "CertiGC/GC Source"
        clang -O2 -fPIC -c gc.c -o gc.o
        ar rcs libcertigc.a gc.o
      '';

      installPhase = ''
        install -Dm644 libcertigc.a $out/lib/libcertigc.a
        for header in gc.h values.h config.h; do
          install -Dm644 "$header" "$out/include/certigc/$header"
        done
      '';

      meta.description =
        "CertiCoq generational garbage collector (C sources from CertiGraph)";
    };

    papriFor = pkgs: pkgs.stdenv.mkDerivation {
      pname   = "papri";
      version = "0.1.0";
      src     = ./.;

      nativeBuildInputs = [ pkgs.gnumake pkgs.clang pkgs.pkg-config ];
      buildInputs = [ pkgs.liburing pkgs.tree-sitter pkgs.boehmgc ];

      # clightgen is deliberately absent here: `nix build` produces the binary,
      # `nix flake check` runs the subset gate. Keeping them apart means a
      # release build does not drag in the CompCert closure.
      buildPhase   = "make CC=clang";
      checkPhase   = "make CC=clang test";
      doCheck      = true;
      installPhase = ''
        install -Dm755 build/papri      $out/bin/papri
        install -Dm644 build/libpapri.a $out/lib/libpapri.a
        for header in src/*.h; do
          install -Dm644 "$header" "$out/include/papri/$(basename $header)"
        done
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
          pkgs.pkg-config
        ];

        # Libraries we link against, so buildInputs rather than packages:
        # that is what puts their pkg-config files on PKG_CONFIG_PATH.
        buildInputs = [
          pkgs.liburing
          pkgs.tree-sitter
          pkgs.boehmgc           # the collector papri actually uses
          (certigcFor pkgs)      # CertiCoq's, packaged to read, not linked
        ];

        shellHook = ''
          # clang, because -Wlarge-by-value-copy is clang-only and it is one
          # of the gates. gcc builds fine, just with one fewer check.
          export CC=clang

          # A tree-sitter grammar is a shared object plus a tags query,
          # loaded at run time. papri reads these two to find one.
          export PAPRI_GRAMMAR="${pkgs.tree-sitter-grammars.tree-sitter-c}"
          export PAPRI_LANGUAGE=c

          echo ""
          echo "┌─ papri ───────────────────────────────────────────"
          echo "│  make             — build lib + binary"
          echo "│  make test        — build and run the tests"
          echo "│  make lint        — grep gate: goto/volatile/varargs/specs"
          echo "│  make vendor-check — vendored verified code is unedited"
          echo "│  make asan        — tests under address/UB sanitizers"
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
      certigc = certigcFor pkgs;
      papri   = papriFor pkgs;
      default = papri;
    });

    checks = forAllSystems (system:
    let pkgs = pkgsFor system; in {
      subset = pkgs.runCommand "papri-subset" {
        nativeBuildInputs = [
          pkgs.gnumake pkgs.clang pkgs.pkg-config
          pkgs.coqPackages.compcert pkgs.diffutils
        ];
        buildInputs = [ pkgs.liburing pkgs.tree-sitter pkgs.boehmgc ];
      } ''
        cp -r ${./.} source
        chmod -R u+w source
        cd source
        make CC=clang lint normalform vendor-check
        touch $out
      '';
    });
  };
}
