/* Standalone device tests have no shared CPU clock and advance their own device
 * explicitly. Link this mock ONLY when hal/irq.c is not part of the test. */
void hal_clock_sync_devices(void) {}
void hal_clock_device_changed(void) {}
