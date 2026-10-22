"""The setup journal: which build steps are done, and from which inputs.

Each step has a fingerprint of its inputs (file sums, tool versions,
arguments). A step whose last run finished with the same fingerprint, and
whose outputs are still there, is skipped; anything else runs again. So
closing setup at any point and starting it again carries on where it
stopped, and a changed input (a new release, a repaired file) redoes only
the steps that depend on it.

The journal is one JSON file, written whole to a temporary file and renamed,
so a crash never leaves half of it.
"""
import hashlib
import json
import os
import threading
import time
from pathlib import Path


def fingerprint(*parts) -> str:
    """A SHA-256 over the parts as canonical JSON (dicts sorted, paths as text)."""
    blob = json.dumps(parts, sort_keys=True, default=str, separators=(",", ":"))
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()


class StepFailed(RuntimeError):
    def __init__(self, step, message, log=None):
        super().__init__(f"{step}: {message}")
        self.step, self.message, self.log = step, message, log


class Journal:
    VERSION = 1

    def __init__(self, path):
        self.path = Path(path)
        self.data = {"version": self.VERSION, "steps": {}}
        self._lock = threading.RLock()      # the compile runs steps from several threads
        if self.path.is_file():
            try:
                d = json.loads(self.path.read_text(encoding="utf-8"))
                if d.get("version") == self.VERSION:
                    self.data = d
            except (OSError, ValueError):
                pass                        # unreadable: start over, every step runs again

    def _save(self):
        with self._lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_name(self.path.name + ".tmp")
            tmp.write_text(json.dumps(self.data, indent=1, sort_keys=True) + "\n", encoding="utf-8")
            os.replace(tmp, self.path)

    def state(self, name) -> dict:
        with self._lock:
            return dict(self.data["steps"].get(name, {}))

    def is_done(self, name, fp, outputs=()) -> bool:
        s = self.state(name)
        return s.get("state") == "done" and s.get("inputs") == fp and all(Path(o).exists() for o in outputs)

    def run(self, name, fp, fn, outputs=(), log=None, refingerprint=None):
        """Run fn() unless the step is done with these inputs. fn returns a dict
        of results to keep (or None). Returns (ran, results).

        refingerprint(results), when given, is the fingerprint recorded once fn
        is done, for a step that learns some of its inputs only by running (the
        headers a compile read). Steps may run from several threads at once; fn
        runs outside the lock."""
        with self._lock:
            if self.is_done(name, fp, outputs):
                return False, self.state(name).get("results", {})
            self.data["steps"][name] = {"state": "running", "inputs": fp, "start": time.time()}
            self._save()
        try:
            res = fn() or {}
            done_fp = refingerprint(res) if refingerprint else fp
        except StepFailed as e:
            with self._lock:
                self.data["steps"][name] = {"state": "failed", "inputs": fp, "message": e.message,
                                            "log": str(e.log or log or ""), "end": time.time()}
                self._save()
            raise
        except Exception as e:
            with self._lock:
                self.data["steps"][name] = {"state": "failed", "inputs": fp,
                                            "message": f"{type(e).__name__}: {e}", "log": str(log or ""),
                                            "end": time.time()}
                self._save()
            raise StepFailed(name, str(e), log) from e
        with self._lock:
            s = self.data["steps"][name]
            s.update(state="done", inputs=done_fp, results=res, end=time.time())
            self._save()
        return True, res

    def forget(self, name=None):
        """Mark one step (or all) as not done, so it runs again."""
        with self._lock:
            if name is None:
                self.data["steps"] = {}
            else:
                self.data["steps"].pop(name, None)
            self._save()
