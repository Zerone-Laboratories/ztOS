// devices.ztos.online: one page (public/index.html) for every address, sent
// with that address's title, description and structured data already in it,
// so search engines and link previews see each phone as its own page.
//
//   /                          the list            /community, /cross
//   /<id>[/install|works|quirks]  a community port   /cross/<codename>  a cross port
//   /maker/<maker>, /cross/maker/<maker>             /add  the form
//   /sitemap.xml, /robots.txt  made from the device data
//
// The device data is read from ztos.online (docs/devices/ in ztOS).

const DATA = 'https://ztos.online/devices/';
const SITE = 'https://devices.ztos.online';

async function json(path) {
	const r = await fetch(DATA + path, { cf: { cacheTtl: 600, cacheEverything: true } });
	if (!r.ok) throw new Error(`${path}: ${r.status}`);
	return r.json();
}

async function data() {
	const [index, cross] = await Promise.all([json('index.json'), json('cross-ports.json').catch(() => ({ devices: [] }))]);
	const devices = (await Promise.all(index.devices.map((id) => json(`${id}/device.json`).catch(() => null)))).filter(Boolean);
	return { devices, cross: cross.devices || [] };
}

function slug(t) {
	return String(t).toLowerCase().replace(/[^a-z0-9]+/g, '-');
}

function clip(t, n = 300) {
	t = String(t || '').replace(/\s+/g, ' ').trim();
	return t.length > n ? t.slice(0, n - 1).replace(/\s+\S*$/, '') + '…' : t;
}

// what the address is about: its title, description, image and structured data
function describe(path, { devices, cross }) {
	const parts = path.split('/').filter(Boolean).map(decodeURIComponent);
	const base = {
		title: 'ztOS Supported Devices: phones that run ztOS',
		description: 'Every phone ztOS runs on, by maker: what works on each, and how to install ztOS on it, step by step. Community ports and experimental cross ports through ztConflo.',
		image: `${SITE}/og.jpg`,
		status: 200,
		ld: {
			'@context': 'https://schema.org',
			'@type': 'CollectionPage',
			name: 'ztOS Supported Devices',
			url: `${SITE}/`,
			isPartOf: { '@type': 'WebSite', name: 'ztOS', url: 'https://ztos.online/' },
		},
	};
	if (!parts.length) return base;
	if (parts[0] === 'community') return { ...base, title: 'Community Ports · ztOS Supported Devices', description: `${devices.length} phones ported to ztOS and checked by the community, with their installation guides.` };
	if (parts[0] === 'add') return { ...base, title: 'Add your port · ztOS Supported Devices', description: 'Ported ztOS to a phone? Send its photo, specifications, installation guide and what works for review.' };
	if (parts[0] === 'maker') {
		const ds = devices.filter((d) => slug(d.maker) === parts[1]);
		if (!ds.length) return { ...base, status: 404 };
		return { ...base, title: `${ds[0].maker} phones that run ztOS · ztOS Supported Devices`, description: `ztOS on ${ds.map((d) => d.name).join(', ')}: installation guides and what works.` };
	}
	if (parts[0] === 'cross') {
		if (!parts[1]) return { ...base, title: 'Cross Ports (experimental) · ztOS Supported Devices', description: `${cross.length} phones ztConflo can build ztOS for, from postmarketOS's ports on mainline Linux. Experimental and mostly untried; updated every day.` };
		if (parts[1] === 'maker') {
			const xs = cross.filter((x) => slug(x.manufacturer) === parts[2]);
			if (!xs.length) return { ...base, status: 404 };
			return { ...base, title: `${xs[0].manufacturer} cross ports (experimental) · ztOS Supported Devices`, description: clip(`ztConflo can build ztOS for ${xs.length} ${xs[0].manufacturer} phones: ${xs.map((x) => x.name).join(', ')}.`) };
		}
		const x = cross.find((c) => c.codename === parts[1]);
		if (!x) return { ...base, status: 404 };
		return {
			...base,
			title: `ztOS on the ${x.name} (${x.codename}): experimental cross port`,
			description: clip(`How to build ztOS for the ${x.name} (${x.codename}, ${x.soc}${x.year ? `, ${x.year}` : ''}) with ztConflo, from postmarketOS's mainline port. ${x.accelerated ? `GPU: ${x.gpu_driver}.` : 'No GPU driver, so the ztMETRO shell will not start.'} Experimental.`),
			ld: {
				'@context': 'https://schema.org',
				'@type': 'HowTo',
				name: `Build and install ztOS on the ${x.name}`,
				url: `${SITE}/cross/${x.codename}`,
				step: ['Install the tools', 'Build the ztOS system image (GSI)', 'Get ztConflo and check the phone', 'Try the build first', 'Build and flash', 'Check what works, and share it'].map((name, i) => ({ '@type': 'HowToStep', position: i + 1, name })),
				about: { '@type': 'Product', name: x.name, brand: { '@type': 'Brand', name: x.manufacturer } },
			},
		};
	}
	const d = devices.find((x) => x.id === parts[0]);
	if (!d) return { ...base, status: 404 };
	const counts = ['working', 'partial', 'broken'].map((s) => `${(d.features || []).filter((f) => f.status === s).length} ${s}`).join(', ');
	const steps = String((d.install && d.install.markdown) || '').split('\n').filter((l) => /^##\s+\d+\./.test(l)).map((l) => l.replace(/^##\s+\d+\.\s*/, ''));
	return {
		...base,
		title: `ztOS on the ${d.name} (${d.codename}): install guide and what works`,
		description: clip(`${d.summary} ${counts}.`),
		image: `${DATA}${d.id}/${d.photo}`,
		ld: {
			'@context': 'https://schema.org',
			'@type': 'HowTo',
			name: `Install ztOS on the ${d.name}`,
			description: clip(d.summary),
			url: `${SITE}/${d.id}`,
			image: `${DATA}${d.id}/${d.photo}`,
			step: steps.map((name, i) => ({ '@type': 'HowToStep', position: i + 1, name, url: `${SITE}/${d.id}/install` })),
			about: { '@type': 'Product', name: d.name, model: d.model || undefined, brand: { '@type': 'Brand', name: d.maker } },
			author: { '@type': 'Person', name: d.maintainer },
		},
	};
}

class Content {
	constructor(content) { this.content = content; }
	element(el) { el.setInnerContent(this.content); }
}
class Attr {
	constructor(name, value) { this.attr = name; this.to = value; }
	element(el) { el.setAttribute(this.attr, this.to); }
}

function sitemap({ devices, cross }) {
	const urls = ['/', '/community', '/cross', '/add']
		.concat([...new Set(devices.map((d) => `/maker/${slug(d.maker)}`))])
		.concat(devices.map((d) => `/${d.id}`))
		.concat([...new Set(cross.map((x) => `/cross/maker/${slug(x.manufacturer)}`))])
		.concat(cross.map((x) => `/cross/${x.codename}`));
	return '<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n' +
		urls.map((u) => `  <url><loc>${SITE}${encodeURI(u)}</loc></url>`).join('\n') + '\n</urlset>\n';
}

export default {
	async fetch(request, env) {
		const url = new URL(request.url);
		if (url.pathname === '/robots.txt') {
			return new Response(`User-agent: *\nAllow: /\n\nSitemap: ${SITE}/sitemap.xml\n`, { headers: { 'Content-Type': 'text/plain' } });
		}
		if (url.pathname === '/sitemap.xml') {
			return new Response(sitemap(await data()), { headers: { 'Content-Type': 'application/xml', 'Cache-Control': 'public, max-age=3600' } });
		}
		// a file (og.jpg, favicon.svg): as it is
		if (/\.[a-z0-9]+$/i.test(url.pathname) && url.pathname !== '/index.html') return env.ASSETS.fetch(request);

		// the asset server answers /index.html with a redirect to /: ask for / itself
		const page = await env.ASSETS.fetch(new Request(new URL('/', url), { headers: request.headers }));
		let about;
		try {
			about = describe(url.pathname, await data());
		} catch {
			return page; // the data didn't load: the page still works, and fetches it itself
		}
		const canonical = SITE + url.pathname.replace(/\/+$/, '') || SITE + '/';
		const rewritten = new HTMLRewriter()
			.on('title', new Content(about.title))
			.on('meta[name="description"]', new Attr('content', about.description))
			.on('link[rel="canonical"]', new Attr('href', url.pathname === '/' ? `${SITE}/` : canonical))
			.on('meta[property="og:title"]', new Attr('content', about.title))
			.on('meta[property="og:description"]', new Attr('content', about.description))
			.on('meta[property="og:url"]', new Attr('content', canonical))
			.on('meta[property="og:image"]', new Attr('content', about.image))
			.on('meta[name="twitter:image"]', new Attr('content', about.image))
			.on('meta[name="robots"]', new Attr('content', about.status === 404 ? 'noindex' : 'index, follow, max-image-preview:large'))
			.on('head', {
				element(el) {
					el.append(`<script type="application/ld+json">${JSON.stringify(about.ld).replace(/</g, '\\u003c')}</script>`, { html: true });
				},
			})
			.transform(page);
		const headers = new Headers(rewritten.headers);
		headers.set('Content-Type', 'text/html; charset=utf-8');
		headers.set('Cache-Control', 'public, max-age=300');
		return new Response(rewritten.body, { status: about.status, headers });
	},
};
