"""Trusted local 2/3-rate example; no listener or hardware access on import."""


def make_block(sample_rate):
    from gnuradio import filter

    # GNU designs the anti-alias/interpolation FIR. Keep the declared ratio
    # reduced; the adapter supports factors <= 64 and output/input <= 2.
    return filter.rational_resampler_ccf(2, 3), 2, 3
