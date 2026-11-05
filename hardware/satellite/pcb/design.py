"""Satellite board design parameters.

These are entered by hand and are NOT read from the schematic. When a value
changes in eeschema it has to be changed here too.
"""

# Battery: single 3S LiPo (spec §1)
#
# CONFLICT: spec §1 says 9.0 - 12.6 V for a 3S pack; this is 15 V. Whichever is
# right, the SENSE_H divider below is sized against this number, so the two have
# to be reconciled before the divider can be signed off (spec §16.13).
V_BAT_MAX = 15  # V, fully charged

# Pyro sense divider, PYRO_H -> SENSE_H_Pn (spec §9.4)
#
# Rev B: Rn11 / Rn12 in output_channel.kicad_sch. Ratio 1k / (3.3k + 1k) = 0.2326.
R_SENSE_TOP = 3.30e3  # Ω, Rn11
R_SENSE_BOTTOM = 1.00e3  # Ω, Rn12

# CONFLICT: the schematic carries plain "3k3" and "1k" parts, not the 0.1 %
# resistors rev A specified. This tolerance is optimistic until the schematic
# names precision parts (spec §16.13).
R_SENSE_TOL = 0.001  # 0.1 %

# Continuity test source resistor, V_TEST -> PYRO_H (spec §9.4)
R_TEST_SOURCE = 1.00e3  # Ω, Rn10
N_CHANNELS = 6

# Continuity test current budget (spec §1). At R_TEST_SOURCE = 1k these are
# exceeded; see spec §11.2 and §16.10.
I_TEST_NOMINAL_MAX = 1.0e-3  # A per channel
I_TEST_SINGLE_FAULT_MAX = 12.0e-3  # A total, set by Estes-class starters

# Current sense, TPS1HB08 SNS -> SENSE_I_Pn (spec §9.3)
R_CURRENT_SENSE = 620.0  # Ω, Rn7
# Rn9 (series to ADC) and Cn (filter) are not set in the schematic yet.

# ADCs (spec §10.1), both supplied from +3V3_A
#
# U1201 ADS124S08: continuity and rail voltages, ratiometric against V_TEST.
# U1202 MCP3208:   per-channel fire current, VREF from +3V3_A.
V_ADC_VDD = 3.3  # V
V_ADC_ABS_MAX_ABOVE_VDD = 0.3  # V, analog input abs max is VDD + 0.3 V

# A reading above V_ADC_VDD is saturated even though it is below the absolute
# maximum. Checks should use this, not the abs max, to decide if a channel is
# measurable (spec §16.13).
V_ADC_FULL_SCALE = V_ADC_VDD
