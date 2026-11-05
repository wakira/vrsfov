# Convenience for NixOS: `nix-shell` uses <nixpkgs> directly (no flake fetch).
# Prefer `nix develop` for the pinned nixpkgs-unstable dev shell.
with import <nixpkgs> { };

mkShell {
  name = "vrsfov-dev";
  packages = [
    cmake
    ninja
    gcc
    gdb
    pkg-config
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
  shellHook = ''
    export VRSFOV_USE_SYSTEM_HEADERS=ON
    export VRSFOV_VULKAN_INCLUDE_DIR="${vulkan-headers}/include"
    export VRSFOV_OPENXR_INCLUDE_DIR="${openxr-loader.dev}/include"
    export VRSFOV_OPENVR_INCLUDE_DIR="${openvr}/include"
    export VRSFOV_OPENVR_LIBRARY_DIR="${openvr}/lib"
    export VRSFOV_GLSLANG_VALIDATOR="${glslang}/bin/glslangValidator"
    echo "vrsfov dev shell (nix-shell)"
    echo "  cmake -B build -GNinja -DVRSFOV_USE_SYSTEM_HEADERS=ON -DVRSFOV_PLATFORM=pc|frame"
  '';
}
