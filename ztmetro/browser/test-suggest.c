/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdbool.h>
#include <string.h>
#include <glib.h>
#include "suggest-json.h"
int main(void) {
	char out[4][96];
	char good[] = "[\"linux\",[\"linux\",\"linux mint\",\"caf\\u00e9\",\"a\\\"b\",\"\\ud83d\\ude00\"]]";
	g_assert_cmpint(suggest_parse(good,out), ==, 4);
	g_assert_cmpstr(out[0], ==, "linux mint");
	g_assert_cmpstr(out[1], ==, "café");
	g_assert_cmpstr(out[2], ==, "a\"b");
	g_assert_cmpstr(out[3], ==, "😀");
	char bad[] = "[\"x\",[\"\\ud800\"]]";
	g_assert_cmpint(suggest_parse(bad,out), ==, 0);
	char html[] = "<!doctype html><title>search</title>";
	g_assert_cmpint(suggest_parse(html,out), ==, 0);
	char duplicate[] = "[\"x\",[\"one\",\"one\",\"two\"]]";
	g_assert_cmpint(suggest_parse(duplicate,out), ==, 2);
	for (size_t i=0; i<strlen(good); i++) {
		char *prefix = g_strndup(good, i);
		suggest_parse(prefix, out);
		g_free(prefix);
	}
	return 0;
}
