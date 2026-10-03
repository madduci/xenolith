# xenolith-server

OpenAI- and Anthropic-compatible HTTP server for the
[Xenolith](https://github.com/simoneiacomino/xenolith) inference engine.

## Features

- `POST /v1/chat/completions` — OpenAI Chat Completions (streaming + buffered)
- `POST /v1/messages` — Anthropic Messages API (streaming + buffered)
- `GET  /v1/models` — OpenAI model list
- `GET  /health` — liveness probe
- Full CORS support so browser clients work out of the box
- Small footprint: libmicrohttpd + Jansson only

## Build

```sh
sudo apt install libmicrohttpd-dev libjansson-dev build-essential
make
make example
./example