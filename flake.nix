{
  description = "Experimental Linux support for MECHREVO JIAOLONG 16 Pro 2025";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          kernel = pkgs.linuxPackages_latest.kernel;
        in
        {
          jialong-ec-monitor = pkgs.callPackage ./drivers/jialong-ec-monitor {
            inherit kernel;
          };
          jialong-fan-control = pkgs.callPackage ./drivers/jialong-fan-control {
            inherit kernel;
          };
          default = self.packages.${system}.jialong-ec-monitor;
        });

      nixosModules = {
        jialong = import ./nix/module.nix;
        default = self.nixosModules.jialong;
      };
    };
}
