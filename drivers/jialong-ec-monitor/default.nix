{ pkgs, kernel, ... }:

pkgs.stdenv.mkDerivation {
  pname = "jialong-ec-monitor";
  version = "0.1";
  src = ./.;

  nativeBuildInputs = [ pkgs.kmod ] ++ kernel.moduleBuildDependencies;
  hardeningDisable = [ "pic" "format" ];

  buildPhase = ''
    runHook preBuild
    make -C "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build" \
      M="$PWD" KERNELRELEASE="${kernel.modDirVersion}" modules
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -D jialong-ec-monitor.ko \
      -t "$out/lib/modules/${kernel.modDirVersion}/extra"
    runHook postInstall
  '';

  meta = {
    description = "Read-only JIAOLONG ACPI EC hwmon monitor";
    platforms = [ "x86_64-linux" ];
  };
}
