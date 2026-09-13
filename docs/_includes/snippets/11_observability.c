/*
 * docs/_includes/snippets/11_observability.c
 *
 * Enable the xtc_tail microscope, run a small workload, and produce a
 * dial9-format trace -- both to a file (xtc_tail_dump_dial9) and as a
 * spilled segment in a directory (xtc_tail_spill_dial9), the way a deployed
 * application gets a trace off the box.  The trace opens in the dial9 GUI
 * viewer unchanged.
 *
 * Compiled AND run by test/docs/test_doc_snippets.sh (a release gate): it
 * must exit 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "xtc.h"
#include "xtc_proc.h"
#include "xtc_tail.h"

static void
task(void *arg)
{
	(void)arg;
	/* a park/run cycle so the trace has scheduler events to show */
	(void)xtc_proc_sleep(1000);
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	char dir[] = "/tmp/xtc_obs_snippet_XXXXXX";
	char trace[320];
	int fd, i;

	/*
	 * Turn recording on.  In a deployed binary you would instead call
	 * xtc_tail_from_env() at startup and let the operator set
	 * XTC_TAIL_ENABLE=1 -- zero code change to arm the microscope.
	 */
	xtc_tail_enable(XTC_TAIL_SCHED);

	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;
	for (i = 0; i < 4; i++) {
		if (xtc_proc_spawn(loop, task, NULL, NULL, NULL) != XTC_OK)
			return 1;
	}
	if (xtc_loop_run(loop) != XTC_OK)
		return 1;
	(void)xtc_loop_fini(loop);

	/* One-shot dump to a file, for `dial9 serve --local-dir`. */
	snprintf(trace, sizeof trace, "/tmp/xtc_obs_snippet_%ld.d9",
	    (long)getpid());
	fd = open(trace, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return 1;
	if (xtc_tail_dump_dial9(fd) != XTC_OK) {
		(void)close(fd);
		return 1;
	}
	(void)close(fd);
	(void)unlink(trace);

	/* Or spill a segment into a directory a sidecar ships off the box. */
	if (mkdtemp(dir) == NULL)
		return 1;
	if (xtc_tail_spill_dial9(dir) != XTC_OK)
		return 1;
	/* A real deployment leaves the segment for its sidecar / rotation to
	 * ship and reap; this snippet just proves the call succeeds. */

	xtc_tail_disable();
	printf("observability: dial9 trace written and segment spilled\n");
	return 0;
}
