package com.fobosapp.networkclient;

/** Receiver activity is independent from the TCP connection and its last image. */
final class RxIndicator {
    enum State { OFF, RETRY, WAIT, STOP, RX, STALE, ERROR }
    private boolean connected;
    private boolean retrying;
    private Boolean receiverRunning;
    private long lastFrame = -1;
    private boolean error;
    synchronized void connection(boolean connected, boolean retrying) {
        this.connected = connected;
        this.retrying = retrying;
        receiverRunning = null;
        lastFrame = -1;
        error = false;
    }
    synchronized void receiver(boolean running) {
        if (!running || Boolean.FALSE.equals(receiverRunning)) lastFrame = -1;
        receiverRunning = running;
        error = false;
    }
    synchronized void frame(long now) { lastFrame = now; error = false; }
    synchronized void error() { error = true; }
    synchronized State state(long now) {
        if (!connected) return retrying ? State.RETRY : State.OFF;
        if (error) return State.ERROR;
        if (Boolean.FALSE.equals(receiverRunning)) return State.STOP;
        if (lastFrame < 0) return State.WAIT;
        return now - lastFrame <= 2500 ? State.RX : State.STALE;
    }
}
