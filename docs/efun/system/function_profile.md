---
title: system / function_profile
---
# function_profile

### NAME

    function_profile() - get function profiling information for an object

### SYNOPSIS

    mapping *function_profile( object ob );

### DESCRIPTION

    Returns  function  profiling  information for 'ob', or this_object() if
    'ob' is not specified.  This is only available if the driver  was  com‐
    piled with PROFILE_FUNCTIONS defined.

### RETURN VALUE

    An  array  of  mappings is returned, one for each function in 'ob', the
    format of the mapping is:
           ([ "name"     : name_of_the_function,
              "calls"    : number_of_calls,

              /* elapsed time in microseconds */
              "self"     : time_spent_in_self,
              "children" : time_spent_in_children
           ])
    Times are elapsed microseconds on the monotonic steady clock, read at
    every function entry and exit, so time a function spends blocked (a file
    read, a save) counts, as it does for the single-threaded game. "self"
    excludes the time spent in the functions it called; "children" is that
    time. Direct recursion is not counted as a child of itself.

    Reading an object's profile resets its program's counters. Clones share
    their blueprint's program, so one call covers every clone; an inherited
    program's counters are read through the object it was loaded as.

### SEE ALSO

    rusage(3), time_expression(3)

