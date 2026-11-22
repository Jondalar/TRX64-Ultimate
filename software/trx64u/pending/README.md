# Changes waiting for the maintainer

`trxmon-ring-steps.patch`: the change to trxmon (the firmware's `software/ultimate-app/apps/trxmon/src`, one source with
the monitor library, U18 D4) that lets the F4 terminal's ring file commands run one step per poll pass in the library.
The firmware registers no step table, so its trxmon keeps today's path; the firmware builds with it and trxmon's host
tests pass. It is left uncommitted in the submodule's working tree until the maintainer has read it and run the board
test in U18 §5; the monitor library on `u18` compiles only with it applied. Apply with
`git -C software/ultimate-app apply ../trx64u/pending/trxmon-ring-steps.patch`.
