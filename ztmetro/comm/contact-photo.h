// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Contact photos, as zrn-contacts-sync leaves them:
 * ~/.local/share/ztmetro/contact-photos/<fnv1a-64 of the uid>.jpg, mode 600.
 * A few are kept decoded at 192 px, the size a list's tile takes them; there is
 * no column in the store for this, the path comes from the uid. Include after
 * comm.h and zrnmetrocore.h. See contacts/CONTACTS-SYNC.md.
 */
#ifndef ZT_CONTACT_PHOTO_H
#define ZT_CONTACT_PHOTO_H

#include <gdk-pixbuf/gdk-pixbuf.h>

#define CONTACT_PHOTOS_KEPT 24	/* 147 KB each; a screen of a list is 12 */

static inline uint64_t contact_photo_hash(const char *uid)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (; *uid; uid++)
		h = (h ^ (unsigned char)*uid) * 0x100000001b3ULL;
	return h;
}

static struct {
	uint64_t key;
	cairo_surface_t *picture;	/* NULL: this person has none */
	unsigned used;
} contact_photos[CONTACT_PHOTOS_KEPT];
static unsigned contact_photo_clock;

static cairo_surface_t *contact_photo_load(uint64_t key)
{
	char name[64], path[500];
	snprintf(name, sizeof(name), "contact-photos/%016llx.jpg", (unsigned long long)key);
	data_path(path, sizeof(path), name);
	if (access(path, R_OK) != 0)
		return NULL;
	GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_scale(path, 192, 192, TRUE, NULL);
	if (!pix)
		return NULL;
	int w = gdk_pixbuf_get_width(pix), h = gdk_pixbuf_get_height(pix);
	int n = gdk_pixbuf_get_n_channels(pix), from = gdk_pixbuf_get_rowstride(pix);
	const guchar *src = gdk_pixbuf_read_pixels(pix);
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	unsigned char *dst = cairo_image_surface_get_data(out);
	int to = cairo_image_surface_get_stride(out);
	for (int y = 0; y < h; y++) {
		const guchar *s = src + (size_t)y * from;
		uint32_t *d = (uint32_t *)(dst + (size_t)y * to);
		for (int x = 0; x < w; x++, s += n)
			d[x] = (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
	}
	cairo_surface_mark_dirty(out);
	g_object_unref(pix);
	return out;
}

/* This person's photo, decoded; NULL if there is none. Not to be kept. */
static inline cairo_surface_t *contact_photo_get(const char *uid)
{
	if (!uid || !uid[0])
		return NULL;
	uint64_t key = contact_photo_hash(uid);
	int oldest = 0;
	for (int i = 0; i < CONTACT_PHOTOS_KEPT; i++) {
		if (contact_photos[i].key == key && contact_photos[i].used) {
			contact_photos[i].used = ++contact_photo_clock;
			return contact_photos[i].picture;
		}
		if (contact_photos[i].used < contact_photos[oldest].used)
			oldest = i;
	}
	if (contact_photos[oldest].picture)
		cairo_surface_destroy(contact_photos[oldest].picture);
	contact_photos[oldest].key = key;
	contact_photos[oldest].picture = contact_photo_load(key);
	contact_photos[oldest].used = ++contact_photo_clock;
	return contact_photos[oldest].picture;
}

/* The sync brought new pictures: nothing kept is to be believed. */
static inline void contact_photos_forget(void)
{
	for (int i = 0; i < CONTACT_PHOTOS_KEPT; i++) {
		if (contact_photos[i].picture)
			cairo_surface_destroy(contact_photos[i].picture);
		contact_photos[i].picture = NULL;
		contact_photos[i].used = 0;
	}
}

/* When the folder last changed: the sync renames each picture into it. */
static inline long contact_photos_stamp(void)
{
	char path[500];
	struct stat st;
	data_path(path, sizeof(path), "contact-photos");
	return stat(path, &st) == 0 ? (long)st.st_mtime : 0;
}

/* Whose number this is, or NULL. */
static inline const struct contact *contact_of_number(const char *number)
{
	for (int i = 0; i < n_contacts; i++) {
		if (same_number(contacts[i].mobile, number) || same_number(contacts[i].other, number))
			return &contacts[i];
	}
	return NULL;
}

/* One UTF-8 character of s into out; returns its length. */
static inline size_t contact_one_char(const char *s, char *out)
{
	size_t n = 1;
	while (s[n] && ((unsigned char)s[n] & 0xc0) == 0x80 && n < 4)
		n++;
	memcpy(out, s, n);
	out[n] = 0;
	if (n == 1)
		out[0] = (char)toupper((unsigned char)out[0]);
	return n;
}

/* The person's square: their photo, or the first letters of their first and
 * last words. Under DotMatrix the photo comes out as dots (zmc_picture). */
static inline void contact_tile(cairo_t *cr, double x, double y, double size, const char *name,
		const char *uid)
{
	cairo_surface_t *photo = contact_photo_get(uid);
	if (photo) {
		zmc_picture(cr, photo, x, y, size);
		return;
	}
	char a[5] = "", b[5] = "", text[10];
	const char *last = strrchr(name, ' ');
	if (name[0])
		contact_one_char(name, a);
	if (last && last[1])
		contact_one_char(last + 1, b);
	snprintf(text, sizeof(text), "%s%s", a, b);
	zmc_set_rgb(cr, 0x3a3a3a);
	cairo_rectangle(cr, x, y, size, size);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	double type = size * 0.42;
	zmc_text_centred(cr, text, FONT_SEMILIGHT, type, x + size / 2, y + (size - type * 1.3) / 2);
}

#endif
