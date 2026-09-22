import design


def test_pyro_sense_below_adc_abs_max():
    # Worst case: top resistor at its low limit, bottom at its high limit,
    # PADn_LO pulled up to the full pack voltage through an intact igniter.
    r_top = design.R_SENSE_TOP * (1 - design.R_SENSE_TOL)
    r_bottom = design.R_SENSE_BOTTOM * (1 + design.R_SENSE_TOL)
    v_sense = design.V_BAT_MAX * r_bottom / (r_top + r_bottom)

    v_abs_max = design.V_ADC_VDD + design.V_ADC_ABS_MAX_ABOVE_VDD
    assert v_sense < v_abs_max, f"SENSEn {v_sense:.3f} V >= abs max {v_abs_max:.3f} V"
