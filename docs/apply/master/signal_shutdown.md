---
title: master / signal_shutdown
---
# signal_shutdown

### NAME

    signal_shutdown - shut the game down when the host sends SIGTERM or SIGINT

### SYNOPSIS

    void signal_shutdown( int signal );

### DESCRIPTION

    When the driver receives SIGTERM or SIGINT, its signal handler only
    records the signal. On the next game tick the driver calls
    signal_shutdown() in the master once, with the signal number, in
    ordinary VM context: this_player() is 0, and call_outs, promises and
    network I/O keep running afterwards.

    The master is expected to start the game's normal shutdown -- save and
    disconnect players, wait for whatever must finish -- and end it with
    shutdown(3). The driver keeps running until it does.

    If the master does not define signal_shutdown(), or it raises an
    error, the driver takes the crash path instead: crash(4) is called and
    the driver aborts, as it did for these signals before this apply
    existed.

    A second SIGTERM or SIGINT, received while the first is waiting for
    the game tick or while the master is still shutting down, takes that
    crash path at once. A service manager's stop timeout (for example
    Docker's stop grace period) should therefore exceed the time the
    master's shutdown takes.

### SEE ALSO

    crash(4), shutdown(3)
