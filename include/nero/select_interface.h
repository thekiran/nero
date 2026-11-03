#ifndef SELECT_INTERFACE_H
#define SELECT_INTERFACE_H

/* Read local interface metadata and prompt for a bounded numeric selection.
 * Returns an adapter-owned name (do not free), or NULL if selection fails. */
const char *list_and_select_interface(void);

#endif

