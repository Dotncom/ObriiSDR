"""Runnable GRC-style hierarchy example for the trusted local bridge loader."""


def make_block(sample_rate):
    from gnuradio import blocks, filter, gr

    class Channel(gr.hier_block2):
        def __init__(self):
            super().__init__("Obrii GRC-style channel", gr.io_signature(1, 1, 8), gr.io_signature(1, 1, 8))
            self.resampler = filter.rational_resampler_ccf(2, 3)
            self.resampler.set_output_multiple(2)
            self.gain = blocks.multiply_const_cc(0.5)
            self.connect(self, self.resampler, self.gain, self)

    channel = Channel()
    history = (len(channel.resampler.taps()) + 1) // 2
    reserve = ((history - 1 + 2) // 3 + 1) * 3
    return channel, 2, 3, reserve
