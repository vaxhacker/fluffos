// Five heartbeats that each run 150 ms of LPC; the first, third and fifth also
// schedule a delayed call_out_walltime. tools/test_http_deadline.py checks the
// heartbeats keep their cadence: each tick is due one gametick after the
// previous tick began, whether or not its LPC armed a timer.
int beats, intervals, last, total;

void start() { set_heart_beat(1); }
void noop() {}

void heart_beat() {
  int now = perf_counter_ns();
  if (beats++) { intervals++; total += now - last; }
  last = now;
  if (intervals == 5) {
    set_heart_beat(0);
    master()->drift_done(total / 1000000);
    return;
  }
  while (perf_counter_ns() - now < 150000000);
  if (beats % 2) call_out_walltime("noop", 30.0);
}
