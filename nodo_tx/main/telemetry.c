#include "telemetry.h"
#include <stdio.h>

int telemetry_format_csv(
    uint32_t seq,
    const char *timestamp,
    const sensor_data_t *d,
    char *out,
    size_t out_size)
{
    int n = snprintf(
        out,
        out_size,
        "%lu,%s,%.2f,%.2f,%.2f,%.2f,%.2f",
        (unsigned long)seq,
        timestamp,
        d->temp_c,
        d->tds_ppm,
        d->ph,
        d->od_mgl,
        d->sst_mgl
    );

    if (n < 0 || (size_t)n >= out_size)
        return -1;

    return n;
}
