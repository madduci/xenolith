# What is Xenolith
Xenolith is a pure C inference engine designed and optimized for Intel Xe-LP iGPUs (11th–13th gen mobile), 32 GB RAM, Linux, and a single target model (Gemma 4 26B-A4B QAT). Intel Xe-LPG and Xe-LPG+ iGPUs are also enabled as experimental targets.
This project exists because I need it. I want to extract the most I can from my laptop, and so it's tuned for exactly that:
HP Envy 17, i7-13700H (6 P-core Raptor Lake-H), DDR4-3200 dual channel (51.2 GB/).
Systems like mine are very interesting for two reasons:

- First, because they are very common setups, broadly adopted for both work and personal use.
- Second reason is that they have unified memory but with a very restrained bus.

This work is inspired by antirez's DwarfStar.

# Why should you use Xenolith
If you have the target spec, Xenolith could be interesting because it is specifically designed to take the most out of this hardware.
In fact, I noticed that general engines like llama.cpp (honestly, the only one I tried) has this problem: either you run it on the CPU or on the GPU. I mean, there are some options that enable to offload some layer to one and some to the other, but that's not the point. The thing is that it makes more sense to separate the two based on the two phases: prefill and decode.
We know that prefill is mostly compute-bound and decode is bandwidth-bound, so it makes sense to exploit the GPU parallelization during the prefill and use CPU during decode. Why don't use GPU also for decode? Because, for some reason, on this hardware CPU can exploit more bandwidth.
So Xenolith is built to do exactly that, and we can see some non-extraordinary results in the benchmarks:

| Token/s | Xenolith | llama.cpp CPU | llama.cpp Vulkan |
|---|---:|---:|---:|
| pp512 | **204,27** | 70,59 | 189,07 |
| pp2048 | **171,08** | 51,11 | 160,67 |
| tg128 | **18,39** | 14,66 | 11,96 |

These are medians of three measurements per row on the target laptop. The llama.cpp CPU column uses one profile throughout: six generation threads, twenty batch threads, CPUs 0–19, mmap, and no BLAS. The input is the same fixed synthetic token sequence for all engines; `tg128` starts from an empty context. The measured samples and instructions for checking or repeating the comparison are in [bench/compare_pp_tg.md](bench/compare_pp_tg.md). Results from a new run can vary with system conditions.

So you can see that, while with llama.cpp you have to choose which one to prioritize between prefill and decode, with Xenolith you have a more balanced setup.

Again, if you look at the decode and prefill improvements separately, you can see that there is no incredible jump, but if you look combined, it can be very useful for a better local experience on this limited hardware.

I want to specify that I didn't write the kernels myself, but I used a mix of Fable and Sol to autoresearch and autoimprove them.

# Design choices
First of all, I decided to not implement an internal agent/harness because that's another whole piece of software that needs attention and care. Also, harnesses are the new IDEs, so very personal, and everyone should use the one he prefers.

So now we need a way to connect your harness to Xenolith. The standard way is a stateless chat/completions HTTP server that was designed to handle massive multi-user requests to a central server. This is a totally different use case than the one we are handling: single-machine, single-user inference.
I thought that, in this context, it would be better to create a stateful protocol so that the harness could talk to the engine in a session-aware way like an integrated agent would.
So now we don't have to send all the history, but we can just "append" the user message. Also, we can make the harness aware of such thing that are valuable in local inference, like the progress of a prefill or the cost of a rewind.

But this currently means that you have to fork your harness and make it talk the Xenolith protocol or write an adapter like I did.
In fact, I also made [Xenopi](https://github.com/simoneiacomino/xenopi), a pi distribution that ships an adapter with pi that translates its requests to Xenolith protocol. This is just a simple example I did to test the engine and the protocol.

# Limits
Current observed limits are right now in the prefill kernel that doesn't scale as well as the Vulkan one. In fact, we can start see a regression in pp8192 where Vulkan becomes faster. But nothing that isn't improvable by spending some tokens on it.

# Build and run

On Arch install the build tools, Level Zero, and Intel's GPU runtime:

```sh
sudo pacman -S --needed gcc make binutils coreutils level-zero-headers level-zero-loader intel-compute-runtime
```

Get Unsloth's [Gemma 4 26B-A4B IT QAT UD-Q4_K_XL GGUF](https://huggingface.co/unsloth/gemma-4-26B-A4B-it-qat-GGUF/blob/main/gemma-4-26B-A4B-it-qat-UD-Q4_K_XL.gguf) then build and run:

```sh
make
./xenolith run /path/to/gemma-4-26B-A4B-it-qat-UD-Q4_K_XL.gguf -p "Hello" -n 64
```

## Optional OpenAI-like API

It is possible to build also an OpenAI-like API for using xenolith with other tools, e.g. claude code.

### Features

- `POST /v1/chat/completions` — OpenAI Chat Completions (streaming + buffered)
- `POST /v1/messages` — Anthropic Messages API (streaming + buffered)
- `GET  /v1/models` — OpenAI model list
- `GET  /health` — liveness probe
- Full CORS support so browser clients work out of the box
- Small footprint: libmicrohttpd + Jansson only

### Build

Install first microhttpd and jahsson libraries:

```sh
sudo apt install libmicrohttpd-dev libjansson-dev build-essential
``` 

then build with

```sh
make WITH_HTTP=1
```

You can run it as

```sh
./xenolith http /path/to/gemma-4-26B-A4B-it-qat-UD-Q4_K_XL.gguf --port 8080
```
