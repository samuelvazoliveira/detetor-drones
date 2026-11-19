"""Registo das deteções em SQLite: é daqui que sai a prova para a queixa."""
import json
import os
import sqlite3
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS events (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        REAL NOT NULL,          -- hora Unix (UTC) do Pi
    time_ok   INTEGER NOT NULL,       -- 1 se a hora do Pi estava acertada (RTC ou telemóvel)
    kind      TEXT NOT NULL,          -- 'alert' | 'track' | 'rf' | 'system'
    source    TEXT NOT NULL,          -- 's3' | 'c5' | 'sdr' | 'hub'
    drone_id  TEXT,
    data      TEXT NOT NULL           -- JSON
);
CREATE INDEX IF NOT EXISTS idx_events_drone ON events(drone_id, ts);
"""


class Store:
    def __init__(self, path: str):
        if path != ":memory:":
            os.makedirs(os.path.dirname(path), exist_ok=True)
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.executescript(SCHEMA)
        self.db.commit()

    def add(self, kind: str, source: str, data: dict, drone_id: str | None = None,
            time_ok: bool = True, ts: float | None = None) -> dict:
        ts = time.time() if ts is None else ts
        cur = self.db.execute(
            "INSERT INTO events (ts, time_ok, kind, source, drone_id, data) VALUES (?,?,?,?,?,?)",
            (ts, int(time_ok), kind, source, drone_id, json.dumps(data, ensure_ascii=False)))
        self.db.commit()
        return {"id": cur.lastrowid, "ts": ts, "time_ok": time_ok, "kind": kind,
                "source": source, "drone_id": drone_id, "data": data}

    def since(self, last_id: int, limit: int = 500, kinds=("alert", "track", "rf")) -> list[dict]:
        q = ("SELECT id, ts, time_ok, kind, source, drone_id, data FROM events "
             f"WHERE id > ? AND kind IN ({','.join('?' * len(kinds))}) ORDER BY id LIMIT ?")
        rows = self.db.execute(q, (last_id, *kinds, limit)).fetchall()
        return [{"id": r[0], "ts": r[1], "time_ok": bool(r[2]), "kind": r[3], "source": r[4],
                 "drone_id": r[5], "data": json.loads(r[6])} for r in rows]

    def last_id(self) -> int:
        r = self.db.execute("SELECT MAX(id) FROM events").fetchone()
        return r[0] or 0

    def count(self, kind: str) -> int:
        return self.db.execute("SELECT COUNT(*) FROM events WHERE kind=?", (kind,)).fetchone()[0]
