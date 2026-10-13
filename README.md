# Telegram Bot API

The Telegram Bot API provides an HTTP API for creating [Telegram Bots](https://core.telegram.org/bots).

If you've got any questions about bots or would like to report an issue with your bot, kindly contact us at [@BotSupport](https://t.me/BotSupport) in Telegram.

Please note that only global Bot API issues that affect all bots are suitable for this repository.

## Custom Fork Optimizations & Upgrades

This repository tracks upstream `tdlib/telegram-bot-api` and currently includes **Bot API 10.3** plus a small set of self-hosting features:

1. **Operations Dashboard (`:8082/stats`)**
   * Responsive dark dashboard with server health, memory, request activity, bot uptime, pending updates, delivery mode, and collapsible diagnostics.
   * Bot tokens are never rendered in HTML, action URLs, or the legacy text/TSV stats view.
   * Per-bot controls support logout, setting/updating a webhook URL, optional secret token, max connections, allowed updates, dropping pending updates, and switching back to polling.
   * A `Logout all` action is available for terminating every active local bot session.
   * Controls submit HTTP POST requests without placing webhook secrets in URLs. Webhook operations display the actual Bot API result; logout controls acknowledge the asynchronous logout request.
   * Stats responses use `Cache-Control: no-store` and related browser hardening headers.
   * The original text stats format remains available with `format=text`, `text=1`, or `tsv=1`.

2. **Built-in MCP endpoint (`:8081/mcp`)**
   * Implements stateless JSON-over-HTTP MCP with current `2026-07-28` discovery/tool calls and compatibility with `2025-11-25` initialization clients.
   * Exposes exactly two tools: `send_message` and `edit_message`.
   * The default Telegram bot and destination chat are configured with environment variables. Clients cannot override them or read the bot token; successful message results include Telegram's usual message and chat metadata.
   * Optional bearer authentication can protect the MCP endpoint.

   Environment variables:

   ```sh
   TELEGRAM_MCP_BOT_TOKEN=123456:bot-token
   TELEGRAM_MCP_CHAT_ID=-1001234567890
   # Optional but recommended when /mcp is reachable by untrusted clients:
   TELEGRAM_MCP_BEARER_TOKEN=change-me
   ```

   For modern clients, include `MCP-Protocol-Version: 2026-07-28`, `Mcp-Method`, and `params._meta` with `io.modelcontextprotocol/protocolVersion` and `io.modelcontextprotocol/clientCapabilities`. Tool calls also include `Mcp-Name`. Legacy clients can use `initialize` with `protocolVersion: 2025-11-25` and send subsequent requests with the matching protocol header. Responses use JSON; this endpoint does not open an SSE stream.

   For compatibility with older clients, omitted `_meta` fields and mirrored headers are accepted; supplied values are validated. The JSON body must still contain exactly one complete request object.

3. **Self-hosting defaults**
   * Zero-argument runtime defaults remain optimized for the local deployment used by this fork:
     - `--http-port=8081`
     - `--http-stat-port=8082`
     - `--http-ip-address=0.0.0.0`
     - `--dir=/opt/telegram-bot-api-data` (or `$TELEGRAM_WORK_DIR`)
     - `--temp-dir=/tmp/telegram-bot-api` (or `$TELEGRAM_TEMP_DIR`)
     - `--local`

4. **GitHub Actions build + GHCR image**
   * Release build on Ubuntu 24.04 with recursive TDLib checkout, binary verification, and an uploaded Linux x86_64 artifact.
   * The Ubuntu/glibc x86_64 artifact remains unchanged. The Docker image is built independently with Alpine/musl in a multi-stage build, compiled with `-j4`, stripped, and published as a compact `linux/amd64` image suitable for Docker on OpenWrt x86_64 hosts.
   * Published tags include `latest` and an immutable `sha-<commit>` tag. Pull requests build the image for validation but do not publish it.

   Example OpenWrt/Docker deployment:

   ```sh
   docker pull ghcr.io/aimeeacker/telegram-bot-api:latest
   docker run -d --name telegram-bot-api --restart unless-stopped \
     -p 8081:8081 -p 8082:8082 \
     -e TELEGRAM_API_ID=YOUR_API_ID \
     -e TELEGRAM_API_HASH=YOUR_API_HASH \
     -e TELEGRAM_MCP_BOT_TOKEN=YOUR_BOT_TOKEN \
     -e TELEGRAM_MCP_CHAT_ID=YOUR_CHAT_ID \
     -v telegram-bot-api-data:/opt/telegram-bot-api-data \
     ghcr.io/aimeeacker/telegram-bot-api:latest
   ```

## Table of Contents
- [Installation](#installation)
- [Dependencies](#dependencies)
- [Usage](#usage)
- [Documentation](#documentation)
- [Moving a bot to a local server](#switching)
- [Moving a bot from one local server to another](#moving)
- [License](#license)

<a name="installation"></a>
## Installation

The simplest way to build and install `Telegram Bot API server` is to use our [Telegram Bot API server build instructions generator](https://tdlib.github.io/telegram-bot-api/build.html).
If you do that, you'll only need to choose the target operating system to receive the complete build instructions.

In general, you need to install all `Telegram Bot API server` [dependencies](#dependencies) and compile the source code using CMake:

```sh
git clone --recursive https://github.com/tdlib/telegram-bot-api.git
cd telegram-bot-api
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target install
```

<a name="dependencies"></a>
## Dependencies
To build and run `Telegram Bot API server` you will need:

* OpenSSL
* zlib
* C++17 compatible compiler (e.g., Clang 5.0+, GCC 7.0+, MSVC 19.1+ (Visual Studio 2017.7+), Intel C++ Compiler 19+) (build only)
* gperf (build only)
* CMake (3.10+, build only)

<a name="usage"></a>
## Usage

Use `telegram-bot-api --help` to receive the list of all available options of the Telegram Bot API server.

The only mandatory options are `--api-id` and `--api-hash`. You must obtain your own `api_id` and `api_hash`
as described in https://core.telegram.org/api/obtaining_api_id and specify them using the `--api-id` and `--api-hash` options
or the `TELEGRAM_API_ID` and `TELEGRAM_API_HASH` environment variables.

To enable Bot API features not available at `https://api.telegram.org`, specify the option `--local`. In the local mode the Bot API server allows to:
* Download files without a size limit.
* Upload files up to 2000 MB.
* Upload files using their local path and [the file URI scheme](https://en.wikipedia.org/wiki/File_URI_scheme).
* Use an HTTP URL for the webhook.
* Use any local IP address for the webhook.
* Use any port for the webhook.
* Set *max_webhook_connections* up to 100000.
* Receive the absolute local path as a value of the *file_path* field without the need to download the file after a *getFile* request.

The Telegram Bot API server accepts only HTTP requests, so a TLS termination proxy needs to be used to handle remote HTTPS requests.

By default the Telegram Bot API server is launched on the port 8081, which can be changed using the option `--http-port`.

<a name="documentation"></a>
## Documentation
See [Bots: An introduction for developers](https://core.telegram.org/bots) for a brief description of Telegram Bots and their features.

See the [Telegram Bot API documentation](https://core.telegram.org/bots/api) for a description of the Bot API interface and a complete list of available classes, methods and updates.

See the [Telegram Bot API server build instructions generator](https://tdlib.github.io/telegram-bot-api/build.html) for detailed instructions on how to build the Telegram Bot API server.

Subscribe to [@BotNews](https://t.me/botnews) to be the first to know about the latest updates and join the discussion in [@BotTalk](https://t.me/bottalk).

<a name="switching"></a>
## Moving a bot to a local server

To guarantee that your bot will receive all updates, you must deregister it with the `https://api.telegram.org` server by calling the method [logOut](https://core.telegram.org/bots/api#logout).
After the bot is logged out, you can replace the address to which the bot sends requests with the address of your local server and use it in the usual way.
If the server is launched in `--local` mode, make sure that the bot can correctly handle absolute file paths in response to `getFile` requests.

<a name="moving"></a>
## Moving a bot from one local server to another

If the bot is logged in on more than one server simultaneously, there is no guarantee that it will receive all updates.
To move a bot from one local server to another you can use the method [logOut](https://core.telegram.org/bots/api#logout) to log out on the old server before switching to the new one.

If you want to avoid losing updates between logging out on the old server and launching on the new server, you can remove the bot's webhook using the method
[deleteWebhook](https://core.telegram.org/bots/api#deletewebhook), then use the method [close](https://core.telegram.org/bots/api#close) to close the bot instance.
After the instance is closed, locate the bot's subdirectory in the working directory of the old server by the bot's user ID, move the subdirectory to the working directory of the new server
and continue sending requests to the new server as usual.

<a name="license"></a>
## License
`Telegram Bot API server` source code is licensed under the terms of the Boost Software License. See [LICENSE_1_0.txt](http://www.boost.org/LICENSE_1_0.txt) for more information.
