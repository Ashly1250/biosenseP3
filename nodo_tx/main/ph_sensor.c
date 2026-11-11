#include "ph_sensor.h"
#include "config.h"

float ph_module_mv(float adc_mv)
{
    return adc_mv / PH_DIVIDER_RATIO;
}

float ph_from_adc_mv(float adc_mv, float temp_c)
{
    if (adc_mv < 50.0f || adc_mv > 3090.0f) {
        return PH_ERROR_VALUE;
    }

    float ph;
#if PH_MODE == 1
    
    (void)temp_c;
    ph = PH_LIN_M * (ph_module_mv(adc_mv) / 1000.0f) + PH_LIN_B;
#else
    const float v = ph_module_mv(adc_mv);

    const float slope_cal = (PH_CAL_MV_B - PH_CAL_MV_A) / (PH_CAL_PH_B - PH_CAL_PH_A);

    const float v7 = PH_CAL_MV_A + slope_cal * (7.0f - PH_CAL_PH_A);

    const float slope = slope_cal * (temp_c + 273.15f) / (PH_CAL_TEMP_C + 273.15f);

    ph = 7.0f + (v - v7) / slope;
#endif
    if (ph < 0.0f)  ph = 0.0f;
    if (ph > 14.0f) ph = 14.0f;
    return ph;
}
