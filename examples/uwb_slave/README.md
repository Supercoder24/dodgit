# uwb_slave
UWB ranging demo, slave side (DWM3001CDK). Power it from a battery; it only answers `uwb_master`.
It listens for a POLL, replies, records its timestamps and returns them to the master in a REPORT frame.

LEDs: led0 green toggles after each completed exchange; led1 + led2 steady = radio init failed.

Build/flash: `JLINK_SN=<slave serial> ./flash.sh examples/uwb_slave`. See `uwb_master/README.md`.
