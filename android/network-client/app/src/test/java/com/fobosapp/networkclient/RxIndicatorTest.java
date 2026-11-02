package com.fobosapp.networkclient;
import org.junit.Test;
import static org.junit.Assert.*;

public class RxIndicatorTest {
    @Test public void stalePictureDoesNotMeanLiveReception() {
        RxIndicator rx = new RxIndicator();
        assertEquals(RxIndicator.State.OFF, rx.state(0));
        rx.connection(true, false);
        assertEquals(RxIndicator.State.WAIT, rx.state(0));
        rx.frame(100);
        assertEquals(RxIndicator.State.RX, rx.state(200));
        assertEquals(RxIndicator.State.STALE, rx.state(3000));
        rx.receiver(false);
        assertEquals(RxIndicator.State.STOP, rx.state(3000));
        rx.receiver(true);
        assertEquals(RxIndicator.State.WAIT, rx.state(3000));
        rx.connection(false, true);
        assertEquals(RxIndicator.State.RETRY, rx.state(3001));
        rx.connection(true, false);
        assertEquals(RxIndicator.State.WAIT, rx.state(3002));
        rx.error();
        assertEquals(RxIndicator.State.ERROR, rx.state(3002));
    }
}
