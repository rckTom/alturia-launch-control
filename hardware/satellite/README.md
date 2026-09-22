# Satellite board — KiCad project

Schematic for the pad-side satellite board. The specification is
[design/satellite_schematic_spec.md](../../design/satellite_schematic_spec.md).

**The schematic is hand-edited.** It was originally generated from the spec by a
script under `generator/`; that script has been removed and the sheets are now
edited in eeschema directly. Do not reintroduce a generator over these files —
regenerating would overwrite hand-drawn work. The spec is maintained to follow
the schematic, not the other way round.

## Status

Mid-migration from rev A to rev B. The output channel and the sense subsystem
have been redrawn; the power, MCU, radio and arm sheets have not. §19 of the spec
lists what is drawn, what is stubbed and what is miswired, and is derived from
the netlist rather than from reading the drawings.

## Verify

Close eeschema first — the exports below read the files, but `.lck` files present
mean someone has the project open and may be mid-edit.

```sh
cd hardware/satellite
kicad-cli sch erc --severity-all satellite.kicad_sch
kicad-cli sch export netlist --format kicadsexpr -o satellite.net satellite.kicad_sch
```

The netlist is the useful artefact: it resolves the six `output_channel`
instances and the root wiring between them, which is where the errors in spec §19
were found and where the next ones will be. Reading the sheets will not show you
that two stages are wired to the wrong rail.

ERC is not currently clean, and will not be until the two ADCs and the enable
lines are wired.

## Design parameters

`design.py` holds the shared numbers — pack voltage, divider values, ADC limits —
and `tests/` asserts the design criteria over them. It does **not** read the
schematic, so the two have to be kept in step by hand.

```sh
uv run --group hardware python -m pytest hardware/satellite/tests -q
```

A passing test is weaker evidence than it looks: the current one checks only the
ADC absolute maximum, not the measurable range, so it passes on a divider that
already saturates. See spec §16.13.

## Layout of the files

| File | Content |
|---|---|
| `satellite.kicad_sch` | Root: hierarchy, six channel instances |
| `01_power`, `02_mcu`, `03_radio`, `04_arm`, `06_sense_ui` | One sheet per spec section |
| `output_channel.kicad_sch` | Rev B output channel, instanced ×6 as `PyroStage1…6` |
| `05_channel.kicad_sch` | Rev A output channel. No longer instanced; kept for reference |
| `design.py`, `tests/` | Design parameters and checks over them |
| `library/` | LCSC symbols/footprints imported with `easyeda2kicad` |

Reference designators are grouped by sheet (2xx power, 3xx MCU, 4xx radio,
5xx arm, 12xx sense). The six channel instances use 6xx, 7xx, 8xx, 9xx, 13xx and
14xx — stage 1 is 14xx, stage 2 is 13xx, stages 3–6 are 6xx–9xx. That ordering is
an artefact of how the instances were created, not a scheme.

## Stale artefacts

`bom_jlcpcb.csv` and `bom_jlcpcb.md` were exported before the rev B channel
existed and describe the rev A design. No generator remains to re-export them;
regenerate from KiCad when the schematic settles.

Parts without an LCSC number are hand-fit: pin headers, tactile buttons, test
points (bare pads) and the not-fitted test headers.
