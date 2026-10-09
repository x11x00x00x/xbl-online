#ifndef XBL_ONLINE_H
#define XBL_ONLINE_H

/*
 * Xbox Live profile attach for xboxrecomp games.
 *
 * This library does not live inside xboxrecomp. Link it from the game
 * project. It writes the account into the Partition0.img file the toolkit
 * already opens for \Device\Harddisk0\Partition0, so a toolkit update does
 * not require merging this code.
 *
 * A stock game needs no source edit: the library installs itself when the
 * process starts and uses xboxrecomp's default save directory. If the game
 * passes its own directory to xbox_path_init, call xbl_online_attach with
 * that same path afterwards.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* save_dir NULL selects the toolkit default (or $XBL_ONLINE_SAVE_DIR). */
void xbl_online_attach(const char *save_dir);

/* Empty string when nobody is signed in. */
const char *xbl_online_gamertag(void);

/* Short line for logs: what the library last did. */
const char *xbl_online_status(void);

/* Local page with the xb.live QR code and the password form. */
const char *xbl_online_signin_url(void);

/* Returns 1. This library signs into XB Online. */
int xbl_online_network(void);

#ifdef __cplusplus
}
#endif

#endif
