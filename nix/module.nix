{ config, lib, pkgs, ... }:

let
  kernel = config.boot.kernelPackages.kernel;
  monitorPackage = pkgs.callPackage ./drivers/jialong-ec-monitor { inherit kernel; };
  fanPackage = pkgs.callPackage ./drivers/jialong-fan-control { inherit kernel; };

  monitorCfg = config.boot.jialongEcMonitor;
  fanCfg = config.boot.jialongFanControl;
  fanWriteCfg = config.boot.jialongFanWrite;
in
{
  options.boot.jialongEcMonitor = lib.mkEnableOption
    "the read-only JIAOLONG EC hwmon monitor";

  options.boot.jialongFanControl = lib.mkEnableOption
    "the QC71/WMI-based JIAOLONG fan control module";

  options.boot.jialongFanWrite = lib.mkOption {
    type = lib.types.bool;
    default = false;
    description = "Enable the leased fan write interface. This performs EC writes.";
  };

  config = lib.mkMerge [
    (lib.mkIf monitorCfg {
      boot.extraModulePackages = [ monitorPackage ];
      boot.kernelModules = [ "jialong_ec_monitor" ];
    })
    (lib.mkIf fanCfg {
      boot.extraModulePackages = [ fanPackage ];
      boot.kernelModules = [ "jialong_fan_control" ];
    })
    (lib.mkIf fanWriteCfg {
      assertions = [
        {
          assertion = fanCfg;
          message = "boot.jialongFanWrite requires boot.jialongFanControl = true.";
        }
      ];
      boot.kernelParams = [ "jialong_fan_control.control_enable=1" ];
    })
  ];
}
