{
  description = "tbtun - kernel TCP tunnel over N+M independent 5-tuples";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    let
      version = self.shortRev or self.dirtyShortRev or "unknown";

      # Build the module for one kernel. The kernel's own build dependencies
      # (gcc, make, pahole when CONFIG_DEBUG_INFO_BTF_MODULES is set) come from
      # moduleBuildDependencies, so the derivation tracks the kernel it is for.
      mkTbtun = pkgs: kernel: pkgs.stdenv.mkDerivation {
        pname = "tbtun";
        inherit version;
        src = ./.;

        nativeBuildInputs = kernel.moduleBuildDependencies;

        makeFlags = kernel.makeFlags ++ [
          "KERNEL_RELEASE=${kernel.modDirVersion}"
          "KERNEL_DIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
          "INSTALL_MOD_DIR=extra"
        ];

        buildPhase = ''
          runHook preBuild
          make ''${makeFlagsArray[@]} \
            -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
            M=$(pwd) modules
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          make ''${makeFlagsArray[@]} \
            -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
            M=$(pwd) INSTALL_MOD_PATH=$out modules_install
          runHook postInstall
        '';

        meta = with pkgs.lib; {
          description = "Kernel TCP tunnel over N+M independent 5-tuples";
          homepage = "https://github.com/HyNetworks/tcp-brutal";
          license = licenses.gpl3Only;
          platforms = platforms.linux;
        };
      };
    in
    {
      # For a kernel other than the running one: lib.mkTbtun pkgs kernel
      lib.mkTbtun = mkTbtun;

      # boot.tbtun.enable = true; boot.kernelModules is set for you.
      # Module parameters go through modprobe, for example:
      #   boot.extraModprobeConfig = "options tbtun debug_loss=1 max_carriers=16";
      nixosModules.default = { config, lib, pkgs, ... }: {
        options.boot.tbtun.enable = lib.mkEnableOption "the tbtun kernel module";
        config = lib.mkIf config.boot.tbtun.enable {
          boot.extraModulePackages = [
            (mkTbtun pkgs config.boot.kernelPackages.kernel)
          ];
          boot.kernelModules = [ "tbtun" ];
        };
      };
    }
    // flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        packages.default = mkTbtun pkgs pkgs.linuxPackages.kernel;
        packages.tbtun = mkTbtun pkgs pkgs.linuxPackages.kernel;
      });
}
