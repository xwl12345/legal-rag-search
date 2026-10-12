#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T9 法条速查 —— 法条语料解析脚本。

从 data/law_corpus/_raw/ 下的官方发布页原始 HTML 快照逐条解析条文，
输出结构化 JSON 到 data/law_corpus/。原始快照保留在仓库中作为文本来源证据。

红线（开发工作计划 T9 第一红线）：法条文本只能来自公开权威版本，
本脚本只做「切条 + 清理空白」两件事，绝不改写任何条文文字。
若解析结果与原始页不一致，以原始页为准。

用法（在仓库根目录）：
  .venv 之外的运行方式均可，例如：
  python tools/parse_law_html.py
"""
from __future__ import annotations

import json
import re
import sys
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RAW_DIR = ROOT / "data" / "law_corpus" / "_raw"
OUT_DIR = ROOT / "data" / "law_corpus"

# 条号标记：仅识别「段首」的 第X条（条内互引如「依照本法第二十四条」
# 不在段首，不会被误切）
ARTICLE_START = re.compile(r"^(第[一二三四五六七八九十百千零]+条)\s*")

# 待解析的法律：原始快照 → 元数据。fetched_at 为本机抓取日期。
SOURCES = [
    {
        "file": "ldf.html",
        "encoding": "utf-8",
        "out": "labor_law.json",
        "law_name": "中华人民共和国劳动法",
        "expect": ("第一条", "第一百零七条"),
        "expect_count": 107,
        "meta": {
            "amendment": "1994年7月5日第八届全国人民代表大会常务委员会第八次会议通过 根据2009年8月27日第十一届全国人民代表大会常务委员会第十次会议《关于修改部分法律的决定》第一次修正 根据2018年12月29日第十三届全国人民代表大会常务委员会第七次会议《关于修改〈中华人民共和国劳动法〉等七部法律的决定》第二次修正",
            "source_desc": "北京市人民政府门户网站「政策文件」栏目转载的中国人大网发布文本（现行有效）",
            "source_url": "https://www.beijing.gov.cn/zhengce/zhengcefagui/qtwj/201912/t20191219_1309776.html",
            "fetched_at": "2026-10-12",
        },
    },
    {
        "file": "lht.html",
        "encoding": "utf-8",
        "out": "labor_contract_law.json",
        "law_name": "中华人民共和国劳动合同法",
        "expect": ("第一条", "第九十八条"),
        "expect_count": 98,
        "meta": {
            "amendment": "2007年6月29日第十届全国人民代表大会常务委员会第二十八次会议通过 根据2012年12月28日第十一届全国人民代表大会常务委员会第三十次会议《关于修改〈中华人民共和国劳动合同法〉的决定》修正",
            "source_desc": "北京市人民政府门户网站「政策文件」栏目转载的中国人大网发布文本（现行有效）",
            "source_url": "https://www.beijing.gov.cn/zhengce/zhengcefagui/qtwj/201307/t20130701_780700.html",
            "fetched_at": "2026-10-12",
        },
    },
    {
        "file": "mfd_utf8.html",
        "encoding": "utf-8",
        "out": "civil_code_marriage.json",
        "law_name": "中华人民共和国民法典（第五编 婚姻家庭）",
        "expect": ("第一千零四十条", "第一千一百一十八条"),
        "expect_count": 79,  # 1040..1118 连续
        "meta": {
            "amendment": "2020年5月28日第十三届全国人民代表大会第三次会议通过",
            "source_desc": "人民网－人民日报发布的《中华人民共和国民法典》全文（2020-06-02，主席令第四十五号公布文本）第五编",
            "source_url": "http://cpc.people.com.cn/n1/2020/0602/c419242-31731828-5.html",
            "fetched_at": "2026-10-12",
        },
    },
]


class ParagraphExtractor(HTMLParser):
    """抽取正文区段中的 <p> 段落文本（保段落结构）。"""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.paragraphs: list[str] = []
        self._in_p = False
        self._buf: list[str] = []

    def handle_starttag(self, tag: str, attrs) -> None:
        if tag == "p":
            self._in_p = True
            self._buf = []

    def handle_endtag(self, tag: str) -> None:
        if tag == "p" and self._in_p:
            self._in_p = False
            text = "".join(self._buf)
            self.paragraphs.append(text)

    def handle_data(self, data: str) -> None:
        if self._in_p:
            self._buf.append(data)


def clean(text: str) -> str:
    """清理空白：全角空格/nbsp 归一、去首尾空白；不改动任何文字。"""
    text = text.replace("\u3000", " ").replace("\xa0", " ")
    text = re.sub(r"[ \t]+", " ", text)
    return text.strip()


def extract_paragraphs(html: str) -> list[str]:
    p = ParagraphExtractor()
    p.feed(html)
    return [clean(x) for x in p.paragraphs if clean(x)]


CN_NUM = "[一二三四五六七八九十百零]"


def to_int(no: str) -> int | None:
    """中文条号转数字（仅用于连续性校验；正文存储仍用原文条号）。"""
    digits = {"零": 0, "一": 1, "二": 2, "三": 3, "四": 4, "五": 5,
              "六": 6, "七": 7, "八": 8, "九": 9}
    units = {"十": 10, "百": 100, "千": 1000}
    total, num = 0, 0
    for ch in no:
        if ch in digits:
            num = digits[ch]
        elif ch in units:
            unit = units[ch]
            total += (num or 1) * unit
            num = 0
        else:
            return None
    return total + num


def parse_law(src: dict) -> dict:
    raw = (RAW_DIR / src["file"]).read_text(encoding=src["encoding"])
    paras = extract_paragraphs(raw)

    articles: list[dict] = []
    current: dict | None = None
    for para in paras:
        m = ARTICLE_START.match(para)
        if m:
            if current:
                articles.append(current)
            no = m.group(1)
            current = {"no": no, "text": para[m.end():].strip()}
        elif current is not None:
            current["text"] = (current["text"] + "\n" + para).strip()
    if current:
        articles.append(current)

    # ── 校验：首末条号 + 条号连续性 ──
    nos = [a["no"] for a in articles]
    problems: list[str] = []
    if nos and nos[0] != src["expect"][0]:
        problems.append(f"首条号 {nos[0]} != 期望 {src['expect'][0]}")
    if nos and nos[-1] != src["expect"][1]:
        problems.append(f"末条号 {nos[-1]} != 期望 {src['expect'][1]}")
    if len(nos) != src["expect_count"]:
        problems.append(f"条数 {len(nos)} != 期望 {src['expect_count']}")
    nums = [to_int(n.removeprefix("第").removesuffix("条")) for n in nos]
    if any(v is None for v in nums):
        problems.append("存在无法解析的条号")
    else:
        gaps = [(nums[i], nums[i + 1]) for i in range(len(nums) - 1)
                if nums[i + 1] != nums[i] + 1]
        if gaps:
            problems.append(f"条号不连续: {gaps}")
    if problems:
        for a in articles:
            if not a["text"]:
                problems.append(f"{a['no']} 正文为空")
                break

    doc = {"law_name": src["law_name"], **src["meta"],
           "article_count": len(articles), "articles": articles,
           "problems": problems}
    return doc


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    ok = True
    for src in SOURCES:
        doc = parse_law(src)
        out_path = OUT_DIR / src["out"]
        out_path.write_text(json.dumps(doc, ensure_ascii=False, indent=1),
                            encoding="utf-8")
        status = "OK" if not doc["problems"] else "PROBLEM"
        print(f"[{status}] {src['law_name']}: {doc['article_count']} 条 -> {out_path.name}")
        for p in doc["problems"]:
            ok = False
            print(f"       !! {p}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
