// ztOS device submissions: the relay between ztos.online's "Add your port" form,
// the ztOS Mobile Discord, and the device list in the ztOS repository.
//
//   POST /submit        the form's entry: checked, then posted as a forum post in
//                       #ztos-device-ports with the photo, device.json, and
//                       Accept / Reject buttons
//   POST /interactions  Discord's button presses: Accept commits the entry to
//                       docs/devices/ (the site shows it a minute later); Reject
//                       asks for a reason, says it in the post and locks it
//
// Only members who can manage threads (moderators) can press the buttons.
// Secrets (wrangler secret put): DISCORD_TOKEN, DISCORD_PUBLIC_KEY, GITHUB_TOKEN.
// Settings (wrangler.toml [vars]): FORUM_CHANNEL, GITHUB_REPO, GITHUB_BRANCH,
// SITE, ALLOWED_ORIGINS.

const DISCORD = 'https://discord.com/api/v10';
const STATUSES = ['working', 'partial', 'broken', 'untested'];
const MARK = { working: '🟢', partial: '🟡', broken: '🔴', untested: '⚪' };
const MANAGE_THREADS = 1n << 34n;
const ADMINISTRATOR = 1n << 3n;
const MAX_BODY = 4 * 1024 * 1024;
const MAX_PHOTO = 3 * 1024 * 1024;

export default {
	async fetch(request, env, ctx) {
		const url = new URL(request.url);
		try {
			if (url.pathname === '/submit') return await submit(request, env);
			if (url.pathname === '/interactions') return await interactions(request, env, ctx);
			return new Response('ztOS device submissions\n', { status: 404 });
		} catch (e) {
			console.error(e.stack || e);
			return json({ error: 'The review service failed. Try again in a minute.' }, 500, cors(request, env));
		}
	},
};

// ---- the form ---------------------------------------------------------------

function cors(request, env) {
	const origin = request.headers.get('Origin') || '';
	const allowed = (env.ALLOWED_ORIGINS || '').split(',').map((s) => s.trim());
	if (!allowed.includes(origin)) return {};
	return {
		'Access-Control-Allow-Origin': origin,
		'Access-Control-Allow-Methods': 'POST, OPTIONS',
		'Access-Control-Allow-Headers': 'Content-Type',
		'Access-Control-Max-Age': '86400',
		Vary: 'Origin',
	};
}

function json(body, status = 200, headers = {}) {
	return new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json', ...headers } });
}

async function submit(request, env) {
	const headers = cors(request, env);
	if (request.method === 'OPTIONS') return new Response(null, { status: 204, headers });
	if (request.method !== 'POST') return json({ error: 'POST only' }, 405, headers);
	if (!headers['Access-Control-Allow-Origin']) return json({ error: 'Send it from ztos.online.' }, 403);

	if (env.SUBMIT_LIMIT) {
		const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
		const { success } = await env.SUBMIT_LIMIT.limit({ key: ip });
		if (!success) return json({ error: 'Too many entries from here. Try again in an hour.' }, 429, headers);
	}

	const text = await request.text();
	if (text.length > MAX_BODY) return json({ error: 'The entry is too big; use a smaller photo.' }, 413, headers);
	let raw;
	try {
		raw = JSON.parse(text);
	} catch {
		return json({ error: 'The entry was not readable.' }, 400, headers);
	}
	if (raw.website) return json({ ok: true }, 200, headers); // the hidden field: a bot filled it in

	const photo = decodePhoto(raw.photo);
	if (!photo) return json({ error: 'The photo must be a JPEG, PNG or WebP under 3 MB.' }, 400, headers);
	const { device, problems } = normalise(raw, `photo.${photo.ext}`);
	if (problems.length) return json({ error: `Fill in: ${problems.join(', ')}.` }, 400, headers);

	const existing = await siteHas(env, device.id);
	await post(env, device, photo, existing);
	return json({ ok: true, id: device.id }, 200, headers);
}

function clip(value, max) {
	return String(value == null ? '' : value).replace(/\r/g, '').trim().slice(0, max);
}

function slug(text) {
	return clip(text, 80).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '');
}

// the entry as device.json holds it, whatever the form (or anyone else) sent
function normalise(raw, photoName) {
	const install = raw.install || {};
	const device = {
		id: '',
		name: clip(raw.name, 80),
		maker: clip(raw.maker, 40),
		model: clip(raw.model, 60),
		codename: clip(raw.codename, 40),
		maintainer: clip(raw.maintainer, 80),
		contact: clip(raw.contact, 200),
		added: new Date().toISOString().slice(0, 10),
		summary: clip(raw.summary, 800),
		photo: photoName,
		photo_caption: clip(raw.photo_caption, 120),
		specs: (Array.isArray(raw.specs) ? raw.specs : [])
			.slice(0, 30)
			.map((r) => [clip(r && r[0], 40), clip(r && r[1], 200)])
			.filter((r) => r[0] && r[1]),
		// the installation guide: Markdown, with its commands in code blocks
		install: { markdown: clip(install.markdown, 30000) },
		features: (Array.isArray(raw.features) ? raw.features : [])
			.slice(0, 80)
			.map((f) => ({
				category: clip(f && f.category, 40),
				feature: clip(f && f.feature, 80),
				status: STATUSES.includes(f && f.status) ? f.status : 'untested',
				notes: clip(f && f.notes, 300),
			}))
			.filter((f) => f.feature),
		notes: (Array.isArray(raw.notes) ? raw.notes : [])
			.slice(0, 20)
			.map((n) => ({ title: clip(n && n.title, 120), text: clip(n && n.text, 800) }))
			.filter((n) => n.title),
	};
	if (!/^[A-Za-z0-9_.-]+$/.test(device.codename)) device.codename = '';
	device.id = slug(`${device.maker}-${device.codename}`);
	const problems = [];
	for (const [key, label] of [['name', 'name'], ['maker', 'maker'], ['codename', 'codename'], ['maintainer', 'your name'], ['contact', 'contact'], ['summary', 'about this port']]) {
		if (!device[key]) problems.push(label);
	}
	if (device.install.markdown.length < 40) problems.push('the installation guide');
	return { device, problems };
}

function decodePhoto(dataUrl) {
	const m = /^data:image\/(jpeg|png|webp);base64,([A-Za-z0-9+/=]+)$/.exec(String(dataUrl || ''));
	if (!m) return null;
	const bytes = Uint8Array.from(atob(m[2]), (c) => c.charCodeAt(0));
	if (bytes.length > MAX_PHOTO) return null;
	const magic = {
		jpeg: bytes[0] === 0xff && bytes[1] === 0xd8,
		png: bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47,
		webp: String.fromCharCode(...bytes.slice(8, 12)) === 'WEBP',
	};
	if (!magic[m[1]]) return null;
	return { bytes, ext: m[1] === 'jpeg' ? 'jpg' : m[1], type: `image/${m[1]}` };
}

async function siteHas(env, id) {
	try {
		const r = await fetch(`${env.SITE}/devices/index.json`, { cf: { cacheTtl: 0 } });
		return r.ok && (await r.json()).devices.includes(id);
	} catch {
		return false;
	}
}

// ---- Discord ------------------------------------------------------------------

async function discord(env, path, init = {}) {
	const r = await fetch(`${DISCORD}${path}`, {
		...init,
		headers: { Authorization: `Bot ${env.DISCORD_TOKEN}`, ...(init.headers || {}) },
	});
	if (!r.ok) throw new Error(`Discord ${init.method || 'GET'} ${path}: ${r.status} ${await r.text()}`);
	return r.status === 204 ? null : r.json();
}

function buttons(disabled = false) {
	return [
		{
			type: 1,
			components: [
				{ type: 2, style: 3, label: 'Accept', custom_id: 'accept', disabled },
				{ type: 2, style: 4, label: 'Reject', custom_id: 'reject', disabled },
			],
		},
	];
}

function field(name, lines) {
	let value = '';
	for (const line of lines) {
		if (value.length + line.length + 1 > 1000) {
			value += '\n…';
			break;
		}
		value += (value ? '\n' : '') + line;
	}
	return { name, value: value || '—' };
}

function plain(text) {
	return String(text).replace(/([*_~|>])/g, '\\$1');
}

async function post(env, device, photo, existing) {
	const counts = Object.fromEntries(STATUSES.map((s) => [s, device.features.filter((f) => f.status === s).length]));
	const embed = {
		title: `${device.name}${device.model ? ` (${device.model})` : ''}`,
		description: plain(device.summary),
		color: 0xe51400,
		fields: [
			{ name: 'Codename', value: `\`${device.codename}\``, inline: true },
			{ name: 'Maintainer', value: plain(device.maintainer), inline: true },
			{ name: 'Contact', value: plain(device.contact), inline: true },
			field('Specifications', device.specs.map((r) => `**${plain(r[0])}**: ${plain(r[1])}`)),
			field(
				`What works: ${STATUSES.map((s) => `${MARK[s]} ${counts[s]}`).join('  ')}`,
				device.features.map((f) => `${MARK[f.status]} ${plain(f.feature)}${f.notes ? ` — ${plain(f.notes)}` : ''}`),
			),
			{ name: 'Installation', value: 'The full guide is in the messages below.' },
		],
		image: { url: `attachment://${device.photo}` },
		footer: { text: `${device.id}${existing ? ' · replaces the entry already in the device list' : ''} · the full entry is in device.json` },
	};
	const form = new FormData();
	form.append(
		'payload_json',
		JSON.stringify({
			name: `${existing ? 'Update: ' : ''}${device.name} (${device.codename})`.slice(0, 100),
			message: {
				content: `New entry for the [device list](https://devices.ztos.online/). Moderators: **Accept** puts it on the site, **Reject** asks for a reason and locks this post.`,
				embeds: [embed],
				components: buttons(),
				allowed_mentions: { parse: [] },
				attachments: [
					{ id: 0, filename: device.photo },
					{ id: 1, filename: 'device.json' },
				],
			},
		}),
	);
	form.append('files[0]', new Blob([photo.bytes], { type: photo.type }), device.photo);
	form.append('files[1]', new Blob([JSON.stringify(device, null, 2) + '\n'], { type: 'application/json' }), 'device.json');
	const thread = await discord(env, `/channels/${env.FORUM_CHANNEL}/threads`, { method: 'POST', body: form });
	for (const content of chunks(`## Installation\n\n${forDiscord(device.install.markdown)}`)) {
		await discord(env, `/channels/${thread.id}/messages`, {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify({ content, allowed_mentions: { parse: [] }, flags: 4 }), // 4: no link previews
		});
	}
}

// Discord draws no tables: each row becomes "- first cell: the rest"
function forDiscord(markdown) {
	const rows = markdown.split('\n');
	return rows
		.filter((line, i) => !/^\s*\|?\s*:?-{3,}/.test(line) && !(/^\s*\|/.test(line) && /^\s*\|?\s*:?-{3,}/.test(rows[i + 1] || '')))
		.map((line) => {
			if (!/^\s*\|.*\|\s*$/.test(line)) return line;
			const cells = line.trim().slice(1, -1).split('|').map((c) => c.trim());
			return `- **${cells[0]}**: ${cells.slice(1).join(' · ')}`;
		})
		.join('\n');
}

// Markdown in pieces under Discord's 2000 characters, split between lines; a
// code block cut in two is closed at the end of one piece and opened again
function chunks(markdown, size = 1900) {
	const out = [];
	let piece = '';
	let fence = null;
	for (const line of markdown.split('\n')) {
		if (piece.length + line.length + 1 > size && piece) {
			out.push(fence ? `${piece}\n\`\`\`` : piece);
			piece = fence ? fence : '';
		}
		piece += (piece ? '\n' : '') + line.slice(0, size - 10);
		const m = /^\s*(```+)(.*)$/.exec(line);
		if (m) fence = fence ? null : '```' + m[2].trim();
	}
	if (piece.trim()) out.push(piece);
	return out;
}

// ---- the buttons ----------------------------------------------------------------

function hex(s) {
	return Uint8Array.from(s.match(/../g).map((b) => parseInt(b, 16)));
}

async function verified(request, env, body) {
	const signature = request.headers.get('X-Signature-Ed25519');
	const timestamp = request.headers.get('X-Signature-Timestamp');
	if (!signature || !timestamp) return false;
	const key = await crypto.subtle.importKey('raw', hex(env.DISCORD_PUBLIC_KEY), { name: 'Ed25519' }, false, ['verify']);
	return crypto.subtle.verify('Ed25519', key, hex(signature), new TextEncoder().encode(timestamp + body));
}

function isModerator(interaction) {
	const permissions = BigInt((interaction.member && interaction.member.permissions) || '0');
	return (permissions & MANAGE_THREADS) !== 0n || (permissions & ADMINISTRATOR) !== 0n;
}

function ephemeral(content) {
	return json({ type: 4, data: { content, flags: 64, allowed_mentions: { parse: [] } } });
}

async function interactions(request, env, ctx) {
	const body = await request.text();
	if (!(await verified(request, env, body))) return new Response('bad signature', { status: 401 });
	const interaction = JSON.parse(body);

	if (interaction.type === 1) return json({ type: 1 }); // Discord's PING

	if (!isModerator(interaction)) return ephemeral('Only moderators can accept or reject entries.');
	const who = interaction.member.user;

	// Reject: ask why first
	if (interaction.type === 3 && interaction.data.custom_id === 'reject') {
		return json({
			type: 9,
			data: {
				custom_id: 'reject-reason',
				title: 'Reject this entry',
				components: [
					{
						type: 1,
						components: [{ type: 4, custom_id: 'reason', label: 'Why? (posted for the maintainer)', style: 2, min_length: 3, max_length: 800, required: true }],
					},
				],
			},
		});
	}

	if (interaction.type === 5 && interaction.data.custom_id === 'reject-reason') {
		const reason = interaction.data.components[0].components[0].value;
		ctx.waitUntil(
			(async () => {
				await discord(env, `/channels/${interaction.channel_id}/messages`, {
					method: 'POST',
					headers: { 'Content-Type': 'application/json' },
					body: JSON.stringify({ content: `❌ Rejected by <@${who.id}>: ${reason}\nFix it and send the entry again from https://devices.ztos.online/add.`, allowed_mentions: { parse: [] } }),
				});
				await discord(env, `/channels/${interaction.channel_id}`, {
					method: 'PATCH',
					headers: { 'Content-Type': 'application/json' },
					body: JSON.stringify({ locked: true, archived: true }),
				});
			})().catch((e) => console.error(e.stack || e)),
		);
		return json({ type: 7, data: { components: [], content: `❌ Rejected by <@${who.id}>.`, allowed_mentions: { parse: [] } } });
	}

	// Accept: answer at once (Discord waits 3 s), commit, then say how it went
	if (interaction.type === 3 && interaction.data.custom_id === 'accept') {
		ctx.waitUntil(accept(env, interaction, who));
		return json({ type: 7, data: { components: buttons(true), content: `⏳ Accepting (<@${who.id}>)…`, allowed_mentions: { parse: [] } } });
	}

	return ephemeral('That button is no longer in use.');
}

async function editOriginal(env, interaction, data) {
	await fetch(`${DISCORD}/webhooks/${interaction.application_id}/${interaction.token}/messages/@original`, {
		method: 'PATCH',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify({ allowed_mentions: { parse: [] }, ...data }),
	});
}

async function accept(env, interaction, who) {
	try {
		const files = interaction.message.attachments || [];
		const entry = files.find((a) => a.filename === 'device.json');
		if (!entry) throw new Error('the post has no device.json');
		const raw = await (await fetch(entry.url)).json();
		// the photo the embed shows is moved out of the attachments into the embed
		const picture = files.find((a) => a.filename === raw.photo);
		const embedded = (interaction.message.embeds || []).map((e) => e.image && e.image.url).find(Boolean);
		const pictureUrl = picture ? picture.url : embedded;
		if (!pictureUrl) throw new Error(`the post has no ${raw.photo}`);
		const bytes = new Uint8Array(await (await fetch(pictureUrl)).arrayBuffer());
		const { device, problems } = normalise(raw, raw.photo);
		if (problems.length || !/^photo\.(jpg|png|webp)$/.test(raw.photo)) throw new Error(`the entry is incomplete: ${problems.join(', ')}`);
		await commit(env, device, bytes, `${who.global_name || who.username}`);
		await editOriginal(env, interaction, {
			content: `✅ Accepted by <@${who.id}>. It is in the [device list](https://devices.ztos.online/${device.id}) within a few minutes.`,
			components: [],
		});
		await discord(env, `/channels/${interaction.channel_id}`, {
			method: 'PATCH',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify({ name: `✅ ${interaction.channel && interaction.channel.name ? interaction.channel.name : device.name}`.slice(0, 100) }),
		}).catch(() => {});
	} catch (e) {
		console.error(e.stack || e);
		await editOriginal(env, interaction, { content: `⚠️ Accepting failed: ${String(e.message).slice(0, 300)}. Press Accept to try again.`, components: buttons() });
	}
}

// ---- GitHub: one commit with the entry, its photo and the list -------------------

async function github(env, path, init = {}) {
	const r = await fetch(`https://api.github.com/repos/${env.GITHUB_REPO}${path}`, {
		...init,
		headers: {
			Authorization: `Bearer ${env.GITHUB_TOKEN}`,
			Accept: 'application/vnd.github+json',
			'User-Agent': 'ztos-device-submit',
			'X-GitHub-Api-Version': '2022-11-28',
			...(init.body ? { 'Content-Type': 'application/json' } : {}),
		},
	});
	if (!r.ok) {
		const error = new Error(`GitHub ${init.method || 'GET'} ${path}: ${r.status} ${(await r.text()).slice(0, 200)}`);
		error.status = r.status;
		throw error;
	}
	return r.json();
}

function base64(bytes) {
	let s = '';
	for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
	return btoa(s);
}

async function commit(env, device, photo, moderator, tries = 3) {
	const branch = env.GITHUB_BRANCH || 'main';
	const dir = `docs/devices/${device.id}`;
	const ref = await github(env, `/git/ref/heads/${branch}`);
	const head = await github(env, `/git/commits/${ref.object.sha}`);
	const listed = await github(env, `/contents/docs/devices/index.json?ref=${ref.object.sha}`);
	const index = JSON.parse(new TextDecoder().decode(Uint8Array.from(atob(listed.content.replace(/\n/g, '')), (c) => c.charCodeAt(0))));
	const update = index.devices.includes(device.id);
	if (!update) index.devices.push(device.id);

	const blob = async (content, encoding) => (await github(env, '/git/blobs', { method: 'POST', body: JSON.stringify({ content, encoding }) })).sha;
	const tree = await github(env, '/git/trees', {
		method: 'POST',
		body: JSON.stringify({
			base_tree: head.tree.sha,
			tree: [
				{ path: `${dir}/device.json`, mode: '100644', type: 'blob', sha: await blob(JSON.stringify(device, null, 2) + '\n', 'utf-8') },
				{ path: `${dir}/${device.photo}`, mode: '100644', type: 'blob', sha: await blob(base64(photo), 'base64') },
				{ path: 'docs/devices/index.json', mode: '100644', type: 'blob', sha: await blob(JSON.stringify(index, null, 2) + '\n', 'utf-8') },
			],
		}),
	});
	const made = await github(env, '/git/commits', {
		method: 'POST',
		body: JSON.stringify({
			message: `devices: ${update ? 'update' : 'add'} ${device.name} (${device.codename})\n\nMaintained by ${device.maintainer}; accepted by ${moderator} in #ztos-device-ports.`,
			tree: tree.sha,
			parents: [ref.object.sha],
		}),
	});
	try {
		await github(env, `/git/refs/heads/${branch}`, { method: 'PATCH', body: JSON.stringify({ sha: made.sha }) });
	} catch (e) {
		if (e.status === 422 && tries > 1) return commit(env, device, photo, moderator, tries - 1); // the branch moved: again on top
		throw e;
	}
}
