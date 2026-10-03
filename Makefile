# --- optional HTTP server --------------------------------------------
# Build with:  make WITH_HTTP=1
# Build clean: make            (default: no HTTP server)
WITH_HTTP ?= 0

CC=gcc
BUILD_COMMIT=$(shell git rev-parse --short=9 HEAD 2>/dev/null || printf unknown)
BASE_CFLAGS=-O3 -march=native -std=c11 -Wall -Wextra -pthread -fsanitize=address -fno-omit-frame-pointer 
NUMERIC_SOURCE_HASH=$(shell sha256sum xenolith.c xenolith.cl | sha256sum | cut -d' ' -f1)
NUMERIC_CFLAGS_HASH=$(shell printf '%s' '$(BASE_CFLAGS)' | sha256sum | cut -d' ' -f1)
LDFLAGS += -fsanitize=address
CFLAGS=$(BASE_CFLAGS) -I$(CURDIR)/http/src -DXE_BUILD_COMMIT='"$(BUILD_COMMIT)"' \
	-DXE_NUMERIC_SOURCE_HASH='"$(NUMERIC_SOURCE_HASH)"' \
	-DXE_NUMERIC_CFLAGS_HASH='"$(NUMERIC_CFLAGS_HASH)"'
LDLIBS=-lm -lze_loader 

ifeq ($(WITH_HTTP),1)
    HTTP_SRC = http/src/xenolith_server.c \
               http/src/openai_api.c \
               http/src/anthropic_api.c \
               http/src/sse_stream.c \
               http/src/xenolith_backend.c
    HTTP_OBJ  = $(HTTP_SRC:.c=.o)
	CFLAGS += -DXE_WITH_HTTP=1
    CPPFLAGS += -DXE_WITH_HTTP=1
	CFLAGS += -I$(CURDIR) -I$(CURDIR)/http/include -I$(CURDIR)/http/src
    CPPFLAGS += -I$(CURDIR) -I$(CURDIR)/http/include -I$(CURDIR)/http/src
    LDLIBS   += -lmicrohttpd -ljansson -lpthread
else
    HTTP_OBJ  =
endif

GPU_SPV=xenolith_gpu.spv
GPU_OBJ=xenolith_gpu_spv.o

xenolith: main.o xenolith.o format.o json.o profile.o kvstore.o conversation.o wire.o serve.o $(GPU_OBJ) $(HTTP_OBJ)
	$(CC) $(CFLAGS) -o $@ main.o xenolith.o format.o json.o profile.o kvstore.o conversation.o wire.o serve.o $(GPU_OBJ) $(HTTP_OBJ) $(LDLIBS)

main.o: main.c xenolith.h profile.h serve.h conversation.h kvstore.h
xenolith.o: xenolith.c xenolith.cl xenolith.h xenolith_internal.h format.h
format.o: format.c format.h
json.o: json.c json.h
profile.o: profile.c profile.h xenolith.h format.h json.h
kvstore.o: kvstore.c kvstore.h xenolith.h format.h
conversation.o: conversation.c conversation.h kvstore.h xenolith.h format.h
wire.o: wire.c wire.h xenolith.h xenolith_internal.h kvstore.h conversation.h profile.h format.h json.h
serve.o: serve.c serve.h wire.h xenolith.h kvstore.h conversation.h profile.h json.h
http/src/%.o: http/src/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -f *.o xenolith tests/certify tests/test_kv tests/test_decode \
		tests/test_model_decode tests/test_fixture_decode bench/bench_decode \
		tests/test_prefill_projection tests/test_prefill_qkv tests/test_prefill_swa \
		tests/test_prefill_dense tests/test_prefill_layer tests/test_prefill_session \
		tests/test_prefill_session_drop tests/test_prefill_session_safe \
		tests/test_session_sync tests/test_format tests/test_snapshot \
		tests/test_snapshot_model \
		tests/test_kvstore tests/test_conversation \
		tests/test_conversation_model \
		tests/test_json tests/test_profile tests/test_wire \
		tests/test_wire_model tests/test_serve tests/test_serve_model \
		bench/bench_attention bench/bench_tg bench/bench_b3b bench/bench_b3b_8e \
		bench/bench_guard bench/compare_pp_tg_xe bench/compare_pp_tg_tokens \
		bench/bench_b3b.spv bench/bench_b3b_adlp.spv \
		bench/bench_prefill_gemm bench/bench_prefill_gemm_down \
		bench/bench_prefill_gemm.spv $(GPU_SPV) $(TEST_TOOLS) $(HTTP_OBJ)

$(GPU_SPV): xenolith.cl
	# Xe-LP is the SPIR-V feature baseline; native code is compiled at startup.
	ocloc compile -file $< -device xe-lp:xe-lpgplus -spv_only -output xenolith_gpu \
		-output_no_suffix -out_dir . -options '-cl-std=CL3.0' -q

$(GPU_OBJ): $(GPU_SPV)
	ld -r -b binary -o $@ $<
	objcopy --rename-section .data=.rodata,alloc,load,readonly,data,contents $@

LLAMA_DIR=../llama.cpp
LLAMA_BUILD=$(LLAMA_DIR)/build-cpu
LLAMA_LIBDIR=$(LLAMA_BUILD)/bin
LLAMA_CFLAGS=-I$(LLAMA_DIR)/include -I$(LLAMA_DIR)/ggml/include
LLAMA_LDFLAGS=-L$(LLAMA_LIBDIR) -Wl,-rpath,$(abspath $(LLAMA_LIBDIR))
LLAMA_LDLIBS=-lllama -lggml -lggml-base

TEST_CFLAGS=-O2 -std=c11 -Wall -Wextra -pthread

UNIT_TESTS=tests/test_format tests/test_json tests/test_decode tests/test_conversation
GPU_TESTS=tests/test_kv tests/test_snapshot tests/test_kvstore tests/test_prefill_session
PERSISTENCE_TESTS=tests/test_snapshot_model tests/test_conversation_model tests/test_session_sync
PREFILL_TESTS=tests/test_prefill_projection tests/test_prefill_qkv tests/test_prefill_swa \
	tests/test_prefill_dense tests/test_prefill_layer tests/test_prefill_session
ENGINE_TESTS=tests/test_snapshot tests/test_snapshot_model tests/test_kvstore \
	tests/test_conversation_model tests/test_kv tests/test_decode tests/test_model_decode \
	tests/test_fixture_decode $(PREFILL_TESTS) tests/test_prefill_session_drop \
	tests/test_prefill_session_safe tests/test_session_sync

$(ENGINE_TESTS): xenolith_internal.h format.h
tests/test_wire tests/test_wire_model tests/test_serve: xenolith.h conversation.h kvstore.h profile.h json.h
tests/test_serve: serve.h
tests/test_profile: json.h

VOCAB_DIR=$(LLAMA_DIR)/models
MODEL?=
GOLDEN_PROMPTS=short long mixed ws nl nlonly json
ORACLE_PROMPTS=short long

tests/certify: tests/certify.c xenolith.o format.o profile.o json.o xenolith.h profile.h $(GPU_OBJ)
	$(CC) $(TEST_CFLAGS) -I. -o $@ tests/certify.c xenolith.o format.o profile.o json.o $(GPU_OBJ) $(LDLIBS)

tests/test_format: tests/test_format.c format.o format.h
	$(CC) $(TEST_CFLAGS) -I. -o $@ tests/test_format.c format.o -pthread

tests/test_json: tests/test_json.c json.o json.h
	$(CC) $(TEST_CFLAGS) -I. -o $@ tests/test_json.c json.o -pthread -lm

tests/test_profile: tests/test_profile.c profile.o profile.h json.o format.o xenolith.o xenolith.h $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_profile.c profile.o json.o format.o xenolith.o $(GPU_OBJ) $(LDLIBS)

tests/test_wire: tests/test_wire.c wire.o wire.h profile.o json.o conversation.o kvstore.o format.o xenolith.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_wire.c wire.o profile.o json.o conversation.o kvstore.o format.o xenolith.o $(GPU_OBJ) $(LDLIBS)

tests/test_wire_model: tests/test_wire_model.c wire.o wire.h profile.o json.o conversation.o kvstore.o format.o xenolith.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_wire_model.c wire.o profile.o json.o conversation.o kvstore.o format.o xenolith.o $(GPU_OBJ) $(LDLIBS)

tests/test_serve: tests/test_serve.c serve.o serve.h wire.o wire.h profile.o json.o conversation.o kvstore.o format.o xenolith.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_serve.c serve.o wire.o profile.o json.o conversation.o kvstore.o format.o xenolith.o $(GPU_OBJ) $(LDLIBS)

tests/test_serve_model: tests/test_serve_model.c json.o json.h xenolith
	$(CC) $(TEST_CFLAGS) -I. -o $@ tests/test_serve_model.c json.o -pthread -lm

tests/test_snapshot: tests/test_snapshot.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_snapshot.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_snapshot_model: tests/test_snapshot_model.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_snapshot_model.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_kvstore: tests/test_kvstore.c xenolith.c xenolith.h kvstore.o kvstore.c kvstore.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_kvstore.c kvstore.o format.o $(GPU_OBJ) $(LDLIBS)

tests/test_conversation: tests/test_conversation.c conversation.o conversation.h kvstore.o kvstore.h xenolith.o xenolith.h format.o format.h $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_conversation.c conversation.o kvstore.o xenolith.o format.o $(GPU_OBJ) $(LDLIBS)

tests/test_conversation_model: tests/test_conversation_model.c xenolith.c xenolith.h conversation.o conversation.h kvstore.o kvstore.h format.o format.h $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_conversation_model.c conversation.o kvstore.o format.o $(GPU_OBJ) $(LDLIBS)

tests/test_kv: tests/test_kv.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_kv.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_decode: tests/test_decode.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_decode.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_model_decode: tests/test_model_decode.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_model_decode.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_prefill_projection: tests/test_prefill_projection.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_prefill_projection.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_prefill_qkv: tests/test_prefill_qkv.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_prefill_qkv.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_prefill_swa: tests/test_prefill_swa.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_prefill_swa.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_prefill_dense: tests/test_prefill_dense.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_prefill_dense.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_prefill_layer: tests/test_prefill_layer.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_prefill_layer.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_prefill_session: tests/test_prefill_session.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_prefill_session.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_prefill_session_drop: tests/test_prefill_session.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -DXE_REPACK_DROP_SOURCE -I. -o $@ tests/test_prefill_session.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_prefill_session_safe: tests/test_prefill_session.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -DXE_REPACK_DROP_SOURCE -DXE_CTX=65536 -I. -o $@ tests/test_prefill_session.c format.o $(GPU_OBJ) $(LDLIBS)

tests/test_session_sync: tests/test_session_sync.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_session_sync.c format.o $(GPU_OBJ) $(LDLIBS)

bench/bench_decode: bench/bench_decode.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ bench/bench_decode.c format.o $(GPU_OBJ) $(LDLIBS)

bench/bench_attention: bench/bench_attention.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ bench/bench_attention.c format.o $(GPU_OBJ) $(LDLIBS)

bench/bench_tg: bench/bench_tg.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ bench/bench_tg.c format.o $(GPU_OBJ) $(LDLIBS)

bench/bench_guard: bench/bench_guard.c
	$(CC) $(TEST_CFLAGS) -o $@ $<

bench/compare_pp_tg_xe: bench/compare_pp_tg.c xenolith.o format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ bench/compare_pp_tg.c xenolith.o format.o $(GPU_OBJ) $(LDLIBS)

bench/compare_pp_tg_tokens: bench/compare_pp_tg_tokens.c
	$(CC) $(BASE_CFLAGS) -o $@ $<

bench/bench_b3b: bench/bench_b3b.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ bench/bench_b3b.c format.o $(GPU_OBJ) $(LDLIBS)

bench/bench_b3b_8e: bench/bench_b3b.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -DXE_WORKERS=8 -DXE_WORKER_ECORES -I. \
		-o $@ bench/bench_b3b.c format.o $(GPU_OBJ) $(LDLIBS)

bench/bench_b3b.spv: bench/bench_b3b.cl
	ocloc compile -file $< -device xe-lp -spv_only -output bench_b3b \
		-output_no_suffix -out_dir bench -options '-cl-std=CL3.0' -q

bench-b3b: bench/bench_b3b bench/bench_b3b.spv

bench-b14: bench/bench_b3b bench/bench_b3b_8e bench/bench_b3b.spv

bench/bench_prefill_gemm: bench/bench_prefill_gemm.c
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LDLIBS)

bench/bench_prefill_gemm_down: bench/bench_prefill_gemm.c
	$(CC) $(TEST_CFLAGS) -DBENCH_MOE_N=2816 -DBENCH_MOE_BLOCKS=22 \
		-o $@ $< $(LDLIBS)

bench/bench_prefill_gemm.spv: bench/bench_prefill_gemm.cl
	ocloc compile -file $< -device xe-lp -spv_only \
		-output bench_prefill_gemm -output_no_suffix -out_dir bench \
		-options '-cl-std=CL3.0' -q

bench-prefill-gemm: bench/bench_prefill_gemm bench/bench_prefill_gemm_down \
	bench/bench_prefill_gemm.spv

tests/test_fixture_decode: tests/test_fixture_decode.c xenolith.c xenolith.h format.o $(GPU_OBJ)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_fixture_decode.c format.o $(GPU_OBJ) $(LDLIBS)

check-kv: tests/test_kv
	./tests/test_kv

check-unit: $(UNIT_TESTS)
	@set -e; for test in $(UNIT_TESTS); do ./$$test; done

check-gpu: $(GPU_TESTS)
	./tests/test_kv
	./tests/test_snapshot
	./tests/test_kvstore
	./tests/test_prefill_session route-reset

check: require-model tests/certify tests/test_model_decode
	$(MAKE) check-unit
	$(MAKE) check-gpu
	./tests/test_model_decode "$(MODEL)"
	XENOLITH_MODEL="$(MODEL)" ./tests/certify

check-persistence: require-model $(PERSISTENCE_TESTS)
	@set -e; for test in $(PERSISTENCE_TESTS); do ./$$test "$(MODEL)"; done

check-prefill: require-model $(PREFILL_TESTS) tests/test_fixture_decode
	@set -e; for test in $(PREFILL_TESTS); do ./$$test "$(MODEL)"; done
	./tests/test_fixture_decode "$(MODEL)"

check-all: require-model
	$(MAKE) check
	$(MAKE) check-persistence
	$(MAKE) check-prefill
	$(MAKE) check-wire
	$(MAKE) check-serve

check-serve: require-model xenolith tests/test_serve tests/test_serve_model
	./tests/test_serve "$(MODEL)"
	./tests/test_serve_model "$(MODEL)"

check-wire: require-model xenolith tests/test_json tests/test_profile tests/test_wire \
		tests/test_wire_model
	./tests/test_json
	./tests/test_profile "$(MODEL)"
	./tests/test_wire "$(MODEL)"
	./tests/test_wire_model "$(MODEL)"

golden: require-model test-tools
	mkdir -p tests/golden tests/fixtures
	@set -e; stage=$$(mktemp -d tests/.golden.XXXXXX); \
	trap 'rm -f "$$stage"/*; rmdir "$$stage"' EXIT; \
	trap 'exit 1' HUP INT TERM; \
	for p in $(GOLDEN_PROMPTS); do \
		tools/tokenize_prompt "$(MODEL)" tests/prompts/$$p.txt > "$$stage/$$p.ids"; \
	done; \
	for p in $(ORACLE_PROMPTS); do \
		ids=$$(tools/tokenize_prompt "$(MODEL)" tests/prompts/$$p.txt); \
		tools/dump_logits "$(MODEL)" "$$ids" "$$stage/$$p.logits"; \
	done; \
	cp "$(VOCAB_DIR)/ggml-vocab-gemma-4.gguf.inp" "$$stage/ggml-vocab-gemma-4.gguf.inp"; \
	cp "$(VOCAB_DIR)/ggml-vocab-gemma-4.gguf.out" "$$stage/ggml-vocab-gemma-4.gguf.out"; \
	for p in $(GOLDEN_PROMPTS); do mv "$$stage/$$p.ids" tests/golden/$$p.ids; done; \
	for p in $(ORACLE_PROMPTS); do mv "$$stage/$$p.logits" tests/golden/$$p.logits; done; \
	mv "$$stage/ggml-vocab-gemma-4.gguf.inp" tests/fixtures/ggml-vocab-gemma-4.gguf.inp; \
	mv "$$stage/ggml-vocab-gemma-4.gguf.out" tests/fixtures/ggml-vocab-gemma-4.gguf.out

TEST_TOOLS=tools/dump_logits tools/tokenize_prompt tools/dump_layers tools/compare_logits

test-tools: $(TEST_TOOLS)

tools/dump_logits: tools/dump_logits.c
	$(CC) $(TEST_CFLAGS) $(LLAMA_CFLAGS) -o $@ $< $(LLAMA_LDFLAGS) $(LLAMA_LDLIBS) $(LDLIBS)

tools/dump_layers: tools/dump_layers.c
	$(CC) $(TEST_CFLAGS) $(LLAMA_CFLAGS) -o $@ $< $(LLAMA_LDFLAGS) $(LLAMA_LDLIBS) $(LDLIBS)

tools/tokenize_prompt: tools/tokenize_prompt.c
	$(CC) $(TEST_CFLAGS) $(LLAMA_CFLAGS) -o $@ $< $(LLAMA_LDFLAGS) $(LLAMA_LDLIBS) $(LDLIBS)

tools/compare_logits: tools/compare_logits.c
	$(CC) $(TEST_CFLAGS) -o $@ $< -lm

test-tools-clean:
	rm -f $(TEST_TOOLS)

require-model:
	@test -f "$(MODEL)" || { printf '%s\n' 'Set MODEL to the path of the Gemma 4 GGUF file.' >&2; exit 2; }

.PHONY: require-model clean check check-unit check-gpu check-persistence check-prefill check-all \
	check-kv check-wire check-serve golden test-tools test-tools-clean \
	bench-b3b bench-b14 bench-prefill-gemm
