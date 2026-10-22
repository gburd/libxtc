#!/usr/sbin/dtrace -qs
/*
 * xtc_timer_lateness.d -- how late libxtc timers fire.
 *
 * Usage:  dtrace -qs xtc_timer_lateness.d -c ./prog
 *
 * timer-fire carries the timer's deadline (arg1) and the loop's clock
 * reading when it fired (arg2), both on the libxtc monotonic clock, so
 * arg2 - arg1 is the lateness the program saw.  Under the deterministic
 * simulator that clock is virtual and the lateness is virtual too.
 * timer-arm / timer-cancel give the churn: a cancel-heavy workload arms
 * timers it never needs (every timed recv arms one).
 */

xtc$target:::timer-arm    { @n["armed"] = count(); }
xtc$target:::timer-cancel { @n["cancelled"] = count(); }

xtc$target:::timer-fire
{
	@n["fired"] = count();
	@late = quantize(arg2 - arg1);
	@worst = max(arg2 - arg1);
}

END
{
	printa("  %-10s %@d\n", @n);
	printf("\nfire lateness (ns, now - deadline)\n");
	printa(@late);
	printa("  worst: %@d ns\n", @worst);
}
