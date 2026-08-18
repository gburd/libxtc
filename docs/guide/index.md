---
title: Guide
nav_order: 2
has_children: true
permalink: /guide/
lede: >-
  Read in order the first time: from your first coroutine to a supervised, multi-core, message-passing program.
---
The guide is written to be read in order the first time. It walks from
your first coroutine to a supervised, multi-core, message-passing
program, explaining the model as it goes and calling out the
alternatives that were considered and set aside.

- [1. Getting started]({{ '/guide/01-getting-started/' | relative_url }})
- [2. Fibers and the event loop]({{ '/guide/02-fibers-and-the-loop/' | relative_url }})
- [3. Processes and messages]({{ '/guide/03-processes-and-messages/' | relative_url }})
- [4. Links, monitors, and supervisors]({{ '/guide/04-supervision/' | relative_url }})
- [5. Blocking work and I/O]({{ '/guide/05-blocking-and-io/' | relative_url }})
- [8. Scheduling and CPU shares]({{ '/guide/08-scheduling/' | relative_url }}) -- proportional-share
  scheduling and the over-budget stall watchdog
- [Thinking in libxtc]({{ '/guide/transitioning/' | relative_url }}) -- the mental shifts and
  anti-patterns
- [Debugging and observing]({{ '/guide/debugging/' | relative_url }})
