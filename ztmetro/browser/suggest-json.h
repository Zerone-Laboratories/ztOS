/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Decode the OpenSearch suggestion array, including JSON Unicode escapes. */
static char *suggest_string(const char **cursor) {
	const char *p = *cursor;
	if (*p++ != '"') return NULL;
	GString *out = g_string_new(NULL);
	while (*p && *p != '"') {
		unsigned char c = *p++;
		if (c < 0x20) goto invalid;
		if (c != '\\') { g_string_append_c(out, c); continue; }
		c = *p++;
		if (!c) goto invalid;
		if (c == 'u') {
			gunichar value = 0;
			for (int i=0; i<4; i++) {
				int digit = g_ascii_xdigit_value(*p);
				if (digit < 0) goto invalid;
				value = value*16+digit; p++;
			}
			if (value >= 0xd800 && value <= 0xdbff) {
				if (p[0] != '\\' || p[1] != 'u') goto invalid;
				p += 2; gunichar low = 0;
				for (int i=0; i<4; i++) {
					int digit = g_ascii_xdigit_value(*p);
					if (digit < 0) goto invalid;
					low = low*16+digit; p++;
				}
				if (low < 0xdc00 || low > 0xdfff) goto invalid;
				value = 0x10000 + ((value-0xd800)<<10) + low-0xdc00;
			}
			if (!value || !g_unichar_validate(value)) goto invalid;
			g_string_append_unichar(out, value < 0x20 ? ' ' : value);
		} else {
			const char *esc = "\"\\/bfnrt", *decoded = "\"\\/     ";
			const char *found = strchr(esc, c);
			if (!found) goto invalid;
			g_string_append_c(out, decoded[found-esc]);
		}
	}
	if (*p != '"' || !g_utf8_validate(out->str, out->len, NULL)) goto invalid;
	*cursor = p+1;
	return g_string_free(out, FALSE);
invalid:
	g_string_free(out, TRUE);
	return NULL;
}
static void suggest_space(const char **p) { while (g_ascii_isspace(**p)) (*p)++; }
static int suggest_parse(char *body, char out[4][96]) {
	const char *p = body;
	suggest_space(&p); if (*p++ != '[') return 0;
	suggest_space(&p); char *echo = suggest_string(&p);
	if (!echo) return 0;
	suggest_space(&p); if (*p++ != ',') { g_free(echo); return 0; }
	suggest_space(&p); if (*p++ != '[') { g_free(echo); return 0; }
	int n = 0;
	while (n < 4) {
		suggest_space(&p); if (*p == ']') break;
		char *answer = suggest_string(&p);
		if (!answer) { n=0; break; }
		bool seen = !g_ascii_strcasecmp(answer, echo);
		for (int i=0; i<n; i++) seen |= !strcmp(out[i], answer);
		if (!seen && *answer && strlen(answer)<96) g_strlcpy(out[n++], answer, 96);
		g_free(answer);
		suggest_space(&p); if (*p == ']') break;
		if (*p++ != ',') { n=0; break; }
	}
	g_free(echo);
	return n;
}
