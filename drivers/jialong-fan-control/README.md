# JIAOLONG fan control (experimental)

This package is an opt-in adaptation of the QC71 WMI EC transaction for the
MECHREVO JIAOLONG Series-X6xR55xK-B2.

Safety properties:

- exact DMI, BIOS and EC firmware checks;
- project ID `0x1a` and universal-fan capability check;
- no write on module load;
- the write interface is disabled unless `control_enable=1` is supplied;
- root-only, write-only `fan_control` attribute;
- raw PWM is restricted to `50..200` (fan-off is not exposed);
- original PWM/mode state is saved and restored after a 30-second lease or
  module unload.

This package is intentionally not enabled by the current NixOS configuration.
The independent `jialong-ec-monitor` package remains the read-only telemetry
source.
