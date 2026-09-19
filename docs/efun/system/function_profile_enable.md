---
title: system / function_profile_enable
---
# function_profile_enable

### NAME

    function_profile_enable() - start or stop the per-function counters

### SYNOPSIS

    int function_profile_enable( int on );
    int function_profile_enable();

### DESCRIPTION

    Starts (`on` nonzero) or stops the counters that function_profile() reads,
    and returns whether they were running before the call. Called with no
    argument it only answers, changing nothing.

    Available when the driver was built with the PROFILE_FUNCTIONS option. They
    are off until something turns them on: this efun, or the driver's --profile
    argument, which starts counting at boot so a run's own startup is measured.

    While they run, every LPC call frame reads a clock twice and walks to its
    calling frame: roughly a fifth of the time spent executing LPC. Switched
    off, a frame costs one branch each way. A game that is not being looked at
    should leave them off.

    Turning them on stamps the frames already open, so the first calls to return
    afterwards report elapsed time rather than whatever their stack slot held.

### SEE ALSO

    function_profile(3)
