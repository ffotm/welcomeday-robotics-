
import json
import os
import textwrap
import threading
import time
import unicodedata

import requests

# ---------------- CONFIG ----------------
OPENROUTER_KEY = os.environ.get("OPENROUTER_API_KEY")  
MODELS = ["anthropic/claude-haiku-5.5"]
REQUEST_TIMEOUT = 15                      
SERIAL_PORT = os.environ.get("SERIAL_PORT")
USE_KEYBOARD = False    
MAX_RECORD_SECONDS = 15
TEXT_COLS = 21             

ALLOWED_ACTIONS = {"none", "wave", "nod", "come", "stop"}
ALLOWED_FACES = {"neutral", "happy", "sad", "surprised"}

SYSTEM_PROMPT = """You are the voice assistant of a small competition robot.
Rules:
- Reply in the same language the user speaks (Arabic, French or English).
- Keep answers short: 1-2 sentences, they will be spoken aloud.
- You can trigger one robot action per reply, only from: none, wave, nod, come, stop.
- You show a face expression on the robot's screen, only from: neutral, happy, sad, surprised.
- Never invent actions. If the user asks for something you cannot do, say so.
- Respond ONLY with JSON, no markdown, in this exact shape:
  {"say": "text to speak", "action": "none", "face": "neutral"}"""

history = []  # last few turns, so it keeps context


# ---------------- LLM ----------------
def ask_llm(messages):
    body = {
        "model": MODELS[0],
        "max_tokens": 300,
        "messages": [{"role": "system", "content": SYSTEM_PROMPT}] + messages,
    }
    if len(MODELS) > 1:
        body["models"] = MODELS  # server-side fallback between models
    r = requests.post(
        "https://openrouter.ai/api/v1/chat/completions",
        headers={"Authorization": f"Bearer {OPENROUTER_KEY or ''}",
                 "Content-Type": "application/json"},
        json=body,
        timeout=REQUEST_TIMEOUT,
    )
    r.raise_for_status()
    return r.json()["choices"][0]["message"]["content"]


def parse_reply(raw):
    """Turn model text into a safe dict. Never trust the model's output blindly."""
    raw = raw.strip().removeprefix("```json").removeprefix("```").removesuffix("```").strip()
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        data = {"say": raw}
    action = data.get("action", "none")
    face = data.get("face", "neutral")
    return {
        "say": str(data.get("say", ""))[:300],
        "action": action if action in ALLOWED_ACTIONS else "none",
        "face": face if face in ALLOWED_FACES else "neutral",
    }


def think(user_text):
    history.append({"role": "user", "content": user_text})
    messages = history[-10:]
    try:
        raw = ask_llm(messages)
    except Exception as e:
        print(f"[brain] request failed: {type(e).__name__}: {e}")
        if "401" in str(e):
            print("[brain] 401 means OpenRouter rejected the key. Check OPENROUTER_API_KEY in THIS terminal.")
        raw = '{"say": "Sorry, I cannot reach my brain right now.", "action": "none", "face": "sad"}'
    reply = parse_reply(raw)
    history.append({"role": "assistant", "content": reply["say"]})
    return reply


# ---------------- HARDWARE LINK ----------------
class Hardware:
    """Serial link to the board (real, or fake over a socket). Without a port it only prints."""

    def __init__(self, port):
        self.ser = None
        self.lock = threading.Lock()
        if port:
            import serial
            self.ser = serial.serial_for_url(port, baudrate=115200, timeout=0.1)
            self._wait_ready()
        else:
            print("[hw] SERIAL_PORT is None: no board, commands are only printed")

    def _wait_ready(self):
        # A real Arduino resets when the port opens; the sketch prints READY once it has started.
        deadline = time.time() + 6
        while time.time() < deadline:
            if self.ser.in_waiting and self.ser.readline().decode(errors="ignore").strip() == "READY":
                print("[hw] board connected and answered READY")
                return
            time.sleep(0.05)
        print("[hw] WARNING: the port opened but the board never answered READY. "
              "Is the latest sketch uploaded? Is this the right port?")

    def send(self, line):
        print(f"[HW <-] {line}")
        if self.ser:
            with self.lock:
                self.ser.write((line + "\n").encode("ascii", "replace"))

    def read_event(self):
        """Non-blocking. Returns 'down' / 'up' when the board reports a button change."""
        if self.ser and self.ser.in_waiting:
            line = self.ser.readline().decode(errors="ignore").strip()
            if line.startswith("BTN:"):
                return line[4:]
        return None

    def flush_input(self):
        if self.ser:
            self.ser.reset_input_buffer()


# ---------------- RESPONSE TEXT FOR THE OLED ----------------
def clean_text(text):
    """Strip accents, replace non-ASCII with '?'. Returns (clean_text, bad_chars, total_chars)."""
    out, bad, total = [], 0, 0
    for ch in unicodedata.normalize("NFKD", text):
        if unicodedata.category(ch) == "Mn":  # combining accent marks
            continue
        if not ch.isspace():
            total += 1
        if ord(ch) < 128 and ch.isprintable():
            out.append(ch)
        elif not ch.isspace():
            out.append("?")
            bad += 1
        else:
            out.append(" ")
    return " ".join("".join(out).split()), bad, total


def text_pages(text):
    """Split text into 2-line pages of TEXT_COLS characters."""
    clean, bad, total = clean_text(text)
    if total and bad > total / 2:  # e.g. Arabic: the screen font cannot show it
        return [("(non-Latin text,", "see speaker)")]
    lines = textwrap.wrap(clean, TEXT_COLS) or [""]
    if len(lines) % 2:
        lines.append("")
    return [(lines[i], lines[i + 1]) for i in range(0, len(lines), 2)]


def show_pages(hw, text, per_page):
    time.sleep(0.15)  # let the board finish reading the previous commands
    pages = text_pages(text)
    for i, (a, b) in enumerate(pages):
        hw.send(f"SAY:{a}|{b}")
        if i < len(pages) - 1:
            time.sleep(per_page)


# ---------------- VOICE ----------------
_stt = None
_tts = None


def load_stt():
    global _stt
    try:
        from faster_whisper import WhisperModel
        print("[stt] loading speech model...")
        _stt = WhisperModel("base", compute_type="int8")  # multilingual
    except Exception as e:
        print(f"[stt] unavailable ({e}); typed input only")


def record_until_released(hw):
    """Records while the button is held (hardware) or until Enter is pressed (keyboard)."""
    import numpy as np
    import sounddevice as sd
    chunks = []

    def cb(indata, frames, t, status):
        chunks.append(indata.copy())

    with sd.InputStream(samplerate=16000, channels=1, dtype="float32", callback=cb):
        if hw.ser and not USE_KEYBOARD:
            start = time.time()
            while time.time() - start < MAX_RECORD_SECONDS:
                if hw.read_event() == "up":
                    break
                time.sleep(0.01)
        else:
            input("[mic] recording... press Enter to stop ")
    return np.concatenate(chunks).flatten() if chunks else None


def transcribe(audio):
    if _stt is None or audio is None or len(audio) < 4000:  # under ~0.25 s
        return ""
    segments, _ = _stt.transcribe(audio, beam_size=1)
    return " ".join(s.text for s in segments).strip()


def speak(text):
    global _tts
    print(f"[robot] {text}")
    try:
        import pyttsx3
        if _tts is None:
            _tts = pyttsx3.init()
        _tts.say(text)
        _tts.runAndWait()
    except Exception as e:
        print(f"[tts unavailable: {e}]")


# ---------------- MAIN LOOP ----------------
def flush_stdin():
    """Discard stray Enter presses (Linux/macOS). Harmless elsewhere."""
    try:
        import sys
        import termios
        termios.tcflush(sys.stdin, termios.TCIFLUSH)
    except Exception:
        pass


def wait_for_input(hw):
    """Returns ('quit'|'text'|'talk', payload)."""
    if hw.ser and not USE_KEYBOARD:
        hw.flush_input()
        while True:
            if hw.read_event() == "down":
                return "talk", None
            time.sleep(0.02)
    flush_stdin()
    typed = input("\n> type a message, Enter = record (Enter again = stop), q = quit: ").strip()
    if typed.lower() == "q":
        return "quit", None
    return ("talk", None) if typed == "" else ("text", typed)


def main():
    if not OPENROUTER_KEY:
        print("[config] OPENROUTER_API_KEY is not set in this terminal, so the AI cannot answer.")
    hw = Hardware(SERIAL_PORT)
    load_stt()
    hw.send("STATE:idle")
    while True:
        kind, payload = wait_for_input(hw)
        if kind == "quit":
            break

        if kind == "talk":
            hw.send("STATE:listening")
            text = transcribe(record_until_released(hw))
            print(f"[you] {text}")
            if not text:
                hw.send("STATE:error")  # sad face for a moment
                time.sleep(1.5)
                hw.send("STATE:idle")
                continue
        else:
            text = payload

        hw.send("STATE:thinking")
        reply = think(text)

        hw.send(f"FACE:{reply['face']}")
        if reply["action"] != "none":
            hw.send(f"ACTION:{reply['action']}")
        hw.send("STATE:speaking")  # the board animates the mouth while we talk

        # response text goes to the OLED page by page, paced to the estimated speech length
        pages = text_pages(reply["say"])
        est = max(2.0, len(reply["say"].split()) / 2.5)
        per_page = max(1.8, est / len(pages))
        started = time.time()
        t = threading.Thread(target=show_pages, args=(hw, reply["say"], per_page))
        t.start()
        speak(reply["say"])
        t.join()
        remaining = est - (time.time() - started)  # TTS missing/failing returns instantly
        if remaining > 0:
            time.sleep(remaining)
        hw.send("STATE:idle")


if __name__ == "__main__":
    main()