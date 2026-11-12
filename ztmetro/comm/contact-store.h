// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Changing the contact store while zrn-contacts-sync may be too: both hold
 * this lock (flock on contacts.lock), read the file, change one thing, and
 * write it. Readers need nothing: the file is replaced by rename. Include
 * after comm.h. See contacts/CONTACTS-SYNC.md.
 */
#ifndef ZT_CONTACT_STORE_H
#define ZT_CONTACT_STORE_H

#include <sys/file.h>

static inline int contact_store_lock(void)
{
	char path[500];
	data_path(path, sizeof(path), "contacts.lock");
	int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd >= 0)
		flock(fd, LOCK_EX);	/* the sync holds it for milliseconds */
	return fd;
}

static inline void contact_store_unlock(int fd)
{
	if (fd >= 0)
		close(fd);
}

/* where this person is now: by uid if it has one, else by name and number */
static inline int contact_store_find(const struct contact *who)
{
	for (int i = 0; i < n_contacts; i++) {
		const struct contact *c = &contacts[i];
		if (who->uid[0] ? !strcmp(c->uid, who->uid) :
				!c->uid[0] && !strcmp(c->name, who->name) && !strcmp(c->mobile, who->mobile))
			return i;
	}
	return -1;
}

/* changes when the file does (rename: a new inode, a new time) */
static inline long contact_store_stamp(void)
{
	char path[500];
	struct stat st;
	data_path(path, sizeof(path), "contacts");
	return stat(path, &st) == 0 ? (long)st.st_mtime ^ ((long)st.st_ino << 20) : 0;
}

#endif
