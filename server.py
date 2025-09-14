# server.py
# Flask wrapper that runs the scheduler executable and returns JSON.
# Extracts top-level JSON from stdout and returns it.

from flask import Flask, request, send_from_directory, jsonify, make_response
from flask_cors import CORS
import subprocess, json, os
import regex as re

app = Flask(__name__, static_folder='.')
CORS(app)

# Prefer './result' but gracefully fall back to './lab4' if needed
SCHEDULER_CANDIDATES = ['./result', './lab4']

_JSON_BRACE_RE = re.compile(r'(\{(?:[^{}]|(?R))*\})', re.DOTALL)

def extract_json(text):
    if not text:
        return None
    try:
        return json.loads(text)
    except Exception:
        pass
    m = _JSON_BRACE_RE.search(text)
    if m:
        candidate = m.group(1)
        try:
            return json.loads(candidate)
        except Exception:
            cand2 = re.sub(r',\s*(\}|])', r'\1', candidate)
            try:
                return json.loads(cand2)
            except Exception:
                return None
    return None

@app.route('/')
def index():
    return send_from_directory('.', 'scheduler.html')

@app.route('/<path:filename>')
def static_files(filename):
    return send_from_directory('.', filename)

@app.route('/favicon.ico')
def favicon():
    return send_from_directory('.', 'favicon.ico')

@app.route('/schedule', methods=['POST'])
def schedule():
    data = request.get_json(force=True, silent=True) or {}
    input_text = data.get('input')

    # Also accept structured payloads (optional)
    if input_text is None:
        alg_chunk = data.get('algorithm_chunk') or data.get('algorithm') or ''
        last_inst = data.get('last_instant') or 0
        proc_list = data.get('processes')
        priority_order = data.get('priority_order', 'lower')
        if proc_list and isinstance(proc_list, list):
            lines = []
            op = data.get('operation','trace')
            lines.append(op)
            lines.append(alg_chunk if alg_chunk else "1")
            lines.append(str(last_inst if last_inst else 20))
            lines.append(str(len(proc_list)))
            lines.append(priority_order)
            for idx, p in enumerate(proc_list):
                name = p.get('name', chr(65 + idx))
                a = p.get('arrival')
                s = p.get('service')
                pr = p.get('priority', None)
                if pr is None:
                    lines.append(f"{name},{a},{s}")
                else:
                    lines.append(f"{name},{a},{s},{pr}")
            input_text = "\n".join(lines) + "\n"

    if input_text is None:
        return make_response(jsonify({"error": "no input provided"}), 400)

    # Try available scheduler binaries in order until one returns valid JSON
    attempts = []
    for exec_path in [p for p in SCHEDULER_CANDIDATES if os.path.exists(p) and os.access(p, os.X_OK)]:
        try:
            proc = subprocess.run([exec_path],
                                  input=input_text.encode('utf-8'),
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,
                                  timeout=20)
        except FileNotFoundError:
            attempts.append({"exec": exec_path, "error": "not found"})
            continue
        except subprocess.TimeoutExpired:
            attempts.append({"exec": exec_path, "error": "timeout"})
            continue
        except Exception as e:
            attempts.append({"exec": exec_path, "error": f"{type(e).__name__}: {e}"})
            continue

        stdout_text = proc.stdout.decode('utf-8', errors='replace')
        stderr_text = proc.stderr.decode('utf-8', errors='replace')

        parsed = extract_json(stdout_text)
        if parsed is not None:
            return jsonify(parsed)
        if stdout_text.startswith("'") and stdout_text.endswith("'"):
            inner = stdout_text[1:-1]
            parsed = extract_json(inner)
            if parsed is not None:
                return jsonify(parsed)

        # Save attempt details (truncated) and try next candidate
        attempts.append({
            "exec": exec_path,
            "returncode": proc.returncode,
            "stdout": stdout_text[:500],
            "stderr": stderr_text[:500]
        })

    if not attempts:
        return make_response(jsonify({"error": f"no scheduler binary found (looked for: {', '.join(SCHEDULER_CANDIDATES)})"}), 500)

    return make_response(jsonify({
        "error": "scheduler did not return valid JSON",
        "attempts": attempts
    }), 500)

if __name__ == '__main__':
    port = int(os.environ.get('PORT', 5001))
    app.run(host='0.0.0.0', port=port, debug=True)

    
