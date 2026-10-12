#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T9 法条速查 —— 法条库 SQLite 构建脚本。

把 data/law_corpus/*.json（由 tools/parse_law_html.py 从官方发布页原始
快照解析而来）构建为 data/law_corpus.db，随程序分发。

JSON 是唯一事实来源；本库文件是派生产物，改动语料后必须重跑本脚本。
可重复执行（整库重建，幂等）。

表结构：
  laws(id, name UNIQUE, amendment, source_desc, source_url, fetched_at)
  articles(id, law_id FK, article_no, article_text, UNIQUE(law_id, article_no))

用法（在仓库根目录）：
  python tools/build_law_db.py
"""
from __future__ import annotations

import json
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPUS_DIR = ROOT / "data" / "law_corpus"
DB_PATH = ROOT / "data" / "law_corpus.db"


def main() -> int:
    if DB_PATH.exists():
        DB_PATH.unlink()  # 整库重建，保证与 JSON 严格一致

    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute("""
        CREATE TABLE laws(
            id          INTEGER PRIMARY KEY,
            name        TEXT NOT NULL UNIQUE,
            amendment   TEXT NOT NULL,
            source_desc TEXT NOT NULL,
            source_url  TEXT NOT NULL,
            fetched_at  TEXT NOT NULL
        )""")
    conn.execute("""
        CREATE TABLE articles(
            id           INTEGER PRIMARY KEY,
            law_id       INTEGER NOT NULL REFERENCES laws(id),
            article_no   TEXT NOT NULL,
            article_text TEXT NOT NULL,
            UNIQUE(law_id, article_no)
        )""")
    conn.execute("""
        CREATE INDEX idx_articles_law_no ON articles(law_id, article_no)
    """)

    total_articles = 0
    for json_path in sorted(CORPUS_DIR.glob("*.json")):
        doc = json.loads(json_path.read_text(encoding="utf-8"))
        if doc.get("problems"):
            print(f"!! {json_path.name} 存在未解决的校验问题，拒绝入库：")
            for p in doc["problems"]:
                print(f"   - {p}")
            return 1
        cur = conn.execute(
            "INSERT INTO laws(name, amendment, source_desc, source_url, fetched_at)"
            " VALUES(?,?,?,?,?)",
            (doc["law_name"], doc["amendment"], doc["source_desc"],
             doc["source_url"], doc["fetched_at"]))
        law_id = cur.lastrowid
        rows = [(law_id, a["no"], a["text"]) for a in doc["articles"]]
        conn.executemany(
            "INSERT INTO articles(law_id, article_no, article_text) VALUES(?,?,?)",
            rows)
        total_articles += len(rows)
        print(f"  {doc['law_name']}: {len(rows)} 条")

    conn.commit()

    laws = conn.execute("SELECT COUNT(*) FROM laws").fetchone()[0]
    arts = conn.execute("SELECT COUNT(*) FROM articles").fetchone()[0]
    conn.close()
    print(f"OK: {laws} 部法律 / {arts} 条 -> {DB_PATH}")
    return 0 if (laws > 0 and arts == total_articles) else 1


if __name__ == "__main__":
    sys.exit(main())
