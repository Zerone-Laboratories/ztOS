# ztos-device-submit

The review relay behind ztos.online's **Add your port** form, a Cloudflare
Worker at `https://ztos-device-submit.ztos.workers.dev`.

1. The form posts the entry (`POST /submit`). The Worker checks it, then the
   **ztOS Ports** bot opens a post in **#ztos-device-ports** on the ztOS Mobile
   Discord with the photo, a summary, `device.json`, and Accept / Reject buttons.
2. A moderator (anyone who can manage threads) presses a button
   (`POST /interactions`, signed by Discord):
   - **Accept** commits `docs/devices/<id>/device.json`, the photo and
     `docs/devices/index.json` to Zerone-Laboratories/ztOS. GitHub Pages puts
     it on the site a minute or two later, and the post is marked ✅.
   - **Reject** asks for a reason, posts it for the maintainer, and locks the post.

An entry whose id (`<maker>-<codename>`) is already listed is posted as
"Update:" and replaces the listed one when accepted.

## Settings

| Where | Name | What |
|---|---|---|
| `wrangler.toml` | `FORUM_CHANNEL` | #ztos-device-ports' channel id |
| `wrangler.toml` | `GITHUB_REPO`, `GITHUB_BRANCH` | where accepted entries are committed |
| `wrangler.toml` | `SITE`, `ALLOWED_ORIGINS` | the site, and the pages allowed to send entries |
| secret | `DISCORD_TOKEN` | the ztOS Ports bot's token |
| secret | `DISCORD_PUBLIC_KEY` | the application's public key, to check Discord's signatures |
| secret | `GITHUB_TOKEN` | fine-grained token: Contents read and write on ztOS only |

The Discord application (developer portal, "ztOS Ports") has its
Interactions Endpoint URL set to `.../interactions`. The bot needs View
Channels, Send Messages (and in threads), Create Public Threads, Manage
Threads, Embed Links, Attach Files and Read Message History in the forum.

## Deploying

```
npm install
npx wrangler deploy
npx wrangler secret put DISCORD_TOKEN      # likewise the other two
npx wrangler tail                          # its logs
```
