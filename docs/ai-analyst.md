# AI analyst

`tools/analyst.py` reads a finished run and writes an engineering review:
a verdict on v2, the problems it found, fixes to make, and the next tests to run.

It is built so the AI cannot make things up unnoticed:

1. **Rules find the evidence.** Every disagreement episode is detected by
   fixed rules from `compare.csv` (the same rules as the report).
2. **The AI only sees that evidence** (`analyst_evidence.json`) and must cite
   episode ids such as `E3` for every claim about the lap.
3. **Every number it writes is checked** against the evidence. Numbers that
   are not there, and episode ids that don't exist, are listed at the bottom
   of the review as unverified.
4. **It never touches the car.** It runs after the lap, on the log.

## Option 0: what we actually ran (free, local, no Ollama)

On 23 November we ran the analyst with **Qwen2.5-3B-Instruct** (Apache-2.0, GGUF
Q4_K_M from Hugging Face) served by `llama-cpp-python`, which speaks the same
OpenAI-style API. About 6 GB of RAM, CPU only:

```bash
pip install "llama-cpp-python[server]"
curl -L -o qwen2.5-3b-instruct-q4_k_m.gguf \
  https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf
python3 -m llama_cpp.server --model qwen2.5-3b-instruct-q4_k_m.gguf --n_ctx 4096 --n_batch 128 --port 8099 \
  --model_alias qwen2.5-3b-instruct &
python3 tools/analyst.py out/shadow --provider openai --url http://127.0.0.1:8099/v1 \
  --model qwen2.5-3b-instruct --timeout 1500
```

The checker only verifies **numbers and episode ids**; the model's reasoning can still
be wrong, which is why the review is advice to an engineer and never touches the
verdict or the car.

## Option A: free and local (Ollama in Ubuntu)

```bash
curl -fsSL https://ollama.com/install.sh | sh
ollama pull qwen2.5:7b          # about 4.7 GB; use qwen2.5:3b if your laptop is slow
cd ~/shadowlap && . .venv/bin/activate
python3 tools/analyst.py out/shadow
python3 tools/report.py out     # the review appears in report.html
```

On a laptop without a strong GPU, a 7B model takes a minute or two.

## Option B: free cloud key (Google Gemini)

Get a free API key from Google AI Studio, then:

```bash
export ANALYST_API_KEY=your-key-here
python3 tools/analyst.py out/shadow --provider openai \
  --url https://generativelanguage.googleapis.com/v1beta/openai --model gemini-2.0-flash
```

Check Google's current free-tier model names and limits; they change.

## Files written

| File | What |
|---|---|
| `out/<run>/analyst_evidence.json` | exactly what the AI was given |
| `out/<run>/analyst.md` | the review, with the automatic check at the bottom |
| `out/<run>/analyst.json` | the same plus cited ids and unverified numbers, for tooling |

Run `python3 tools/analyst.py out/shadow --evidence-only` to see the evidence
without calling any AI.
