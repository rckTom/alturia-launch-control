# Rocketry launch controller

A remote launch controller for amateur rocketry: a handheld operator console
talking over LoRa to one or more pad-side satellite boards, each firing up to six
igniter or valve channels.

The organising principle is that no single fault — a firmware bug, a stuck GPIO,
a welded relay or a failed MOSFET — can fire an igniter on its own.

## Layout

| Path | Content |
|---|---|
| [design/](design/) | Design documents. `satellite.md` is the original brief; `satellite_schematic_spec.md` is the netlist-level specification |
| [hardware/satellite/](hardware/satellite/) | KiCad project for the pad-side satellite board |
| [firmware/controller_ui/](firmware/controller_ui/) | Zephyr application for the operator console UI (LVGL, currently built for `native_sim`) |
| [src/launch_controller/](src/launch_controller/) | Python package — placeholder |

## Status

Nothing has been prototyped. The satellite schematic is mid-migration from rev A
to rev B: the output channel and sense subsystem have been redrawn around a smart
high-side switch and two new ADCs, and the remaining sheets have not caught up.
§19 of the schematic spec is the current, netlist-derived list of what is drawn,
what is stubbed and what is miswired.

The console firmware runs against Zephyr's `native_sim` board with an emulated
display; there is no hardware target for it yet.

## Working on it

The schematic is **hand-edited in eeschema**. It was once generated from the
spec; that generator has been removed. See
[hardware/satellite/README.md](hardware/satellite/README.md) for the verify loop
and for why the exported netlist, not the drawings, is the thing to check.

Design-parameter checks:

```sh
uv run --group dev python -m pytest -q
```

Console firmware, against a Zephyr tree already set up with west:

```sh
west build -b native_sim firmware/controller_ui && ./build/zephyr/zephyr.exe
```
