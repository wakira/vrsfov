{
  description = "vrsfov — VRS-based foveated rendering for Linux VR (Vulkan; PC OpenXR/OpenVR and Steam Frame)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          name = "vrsfov-dev";
          packages = with pkgs; [
            cmake
            ninja
            gcc
            gdb
            pkg-config
            git
            python3
            glslang
            shaderc
            vulkan-headers
            vulkan-loader
            vulkan-tools
            vulkan-validation-layers
            openxr-loader
            openvr
          ];
          # Headers come from nixpkgs instead of FetchContent (offline builds).
          shellHook = ''
            export VRSFOV_USE_SYSTEM_HEADERS=ON
            export VRSFOV_VULKAN_INCLUDE_DIR="${pkgs.vulkan-headers}/include"
            export VRSFOV_OPENXR_INCLUDE_DIR="${pkgs.openxr-loader.dev}/include"
            export VRSFOV_OPENVR_INCLUDE_DIR="${pkgs.openvr}/include"
            export VRSFOV_OPENVR_LIBRARY_DIR="${pkgs.openvr}/lib"
            export VRSFOV_GLSLANG_VALIDATOR="${pkgs.glslang}/bin/glslangValidator"
            echo "vrsfov dev shell"
            echo "  cmake -B build -GNinja -DVRSFOV_USE_SYSTEM_HEADERS=ON -DVRSFOV_PLATFORM=pc|frame"
            echo "  cmake --build build && ctest --test-dir build --output-on-failure"
          '';
        };
      });
    };
}
