{
  description = "vrsfov — VRS-based foveated rendering for Linux VR (Vulkan; PC OpenXR/OpenVR and Steam Frame)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
      mkVrsfov = pkgs: platform: pkgs.stdenv.mkDerivation {
        pname = "vrsfov-${platform}";
        version = "0.1.0";
        src = self;
        nativeBuildInputs = with pkgs; [ cmake ninja ];
        cmakeFlags = [
          "-DVRSFOV_PLATFORM=${platform}"
          "-DVRSFOV_USE_SYSTEM_HEADERS=ON"
          "-DVRSFOV_BUILD_TESTS=OFF"
          "-DVRSFOV_BUILD_GPU_TESTS=OFF"
          "-DVRSFOV_VULKAN_INCLUDE_DIR=${pkgs.vulkan-headers}/include"
          "-DVRSFOV_OPENVR_INCLUDE_DIR=${pkgs.openvr}/include"
          "-DVRSFOV_OPENVR_LIBRARY=${pkgs.openvr}/lib/libopenvr_api.so"
        ] ++ pkgs.lib.optionals (platform == "pc") [
          "-DVRSFOV_OPENXR_INCLUDE_DIR=${pkgs.openxr-loader.dev}/include"
        ];
        installPhase = ''
          runHook preInstall
          mkdir -p $out/lib $out/bin \
            $out/share/vulkan/implicit_layer.d
          cp libvrsfov.so $out/lib/
          cp vrsfov-gaze $out/bin/
          if [ -f vrsfov-overlay ]; then
            cp vrsfov-overlay $out/bin/
          fi
          substitute $src/manifests/VkLayer_vrsfov.json.in \
            $out/share/vulkan/implicit_layer.d/VkLayer_vrsfov.json \
            --replace-fail "@VRSFOV_LIBRARY_PATH@" "$out/lib/libvrsfov.so"
          if [ "${platform}" = "pc" ]; then
            mkdir -p $out/share/openxr/1/api_layers/implicit.d
            substitute $src/manifests/XrApiLayer_vrsfov.json.in \
              $out/share/openxr/1/api_layers/implicit.d/XrApiLayer_vrsfov.json \
              --replace-fail "@VRSFOV_LIBRARY_PATH@" "$out/lib/libvrsfov.so"
          fi
          runHook postInstall
        '';
      };
    in
    {
      packages = forAllSystems (pkgs: rec {
        pc = mkVrsfov pkgs "pc";
        frame = mkVrsfov pkgs "frame";
        default = pc;
      });

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
            echo "  PC:    cmake -B build-pc -GNinja -DVRSFOV_USE_SYSTEM_HEADERS=ON -DVRSFOV_PLATFORM=pc"
            echo "  Frame: cmake -B build-frame -GNinja -DVRSFOV_USE_SYSTEM_HEADERS=ON -DVRSFOV_PLATFORM=frame"
            echo "  cmake --build build-pc && ctest --test-dir build-pc --output-on-failure"
          '';
        };
      });
    };
}
