{ pkgs, kernel, ... }:

pkgs.stdenv.mkDerivation {
  pname = "jialong-fan-control";
  version = "0.1";
  src = ./.;

  nativeBuildInputs = kernel.moduleBuildDependencies;
  hardeningDisable = [ "pic" "format" ];

  buildPhase = ''
    runHook preBuild
    make -C "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build" \
      M="$PWD" KERNELRELEASE="${kernel.modDirVersion}" modules
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -D jialong-fan-control.ko \
      -t "$out/lib/modules/${kernel.modDirVersion}/extra"
    runHook postInstall
  '';

  meta = {
    description = "Gated JIAOLONG QC71-based fan control";
    platforms = [ "x86_64-linux" ];
  };
}
