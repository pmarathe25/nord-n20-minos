/* The modem is already held online by the Linux supervisor during this
 * diagnostic. Report the Android-only peripheral-manager service as absent;
 * this library deliberately does not read or write device state. */
int pm_client_register(void) { return -1; }
int pm_client_unregister(void) { return -1; }
int pm_client_connect(void) { return -1; }
int pm_client_disconnect(void) { return -1; }
int pm_client_event_acknowledge(void) { return -1; }
