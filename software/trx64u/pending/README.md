# Changes waiting for the maintainer

`trxmon-ring-steps.patch`: the change to trxmon (the firmware's `software/ultimate-app/apps/trxmon/src`, one source with
the monitor library, U18 D4) that runs everything of the F4 terminal that reaches the machine in the library's poll:
the key only queues the line (`ui_set_polled`, `ui_poll`), the status, register and device rows are what the poll read
last, and a ring file runs there one step per pass. The firmware sets neither the queue nor a step table, so its trxmon
keeps today's path; the firmware builds with it and trxmon's host tests pass. It is left uncommitted in the submodule's working tree until the maintainer has read it and run the board
test in U18 §5; the monitor library on `u18` compiles only with it applied. Apply with
`git -C software/ultimate-app apply ../trx64u/pending/trxmon-ring-steps.patch`.
