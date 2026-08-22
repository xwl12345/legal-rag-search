# -*- coding: utf-8 -*-
"""
PDF 文本提取脚本（统一入口）

提取策略（自动级联）：
  1. PyMuPDF 文本层提取 — 支持中文 CID/CJK 字体编码，适用于文字型 PDF
  2. Tesseract OCR — 扫描件/图片型 PDF 回退方案
  3. PaddleOCR — 更高准确率的 OCR 回退

用法：
    python pdf_ocr.py <pdf_file_path>

输出：提取的纯文本（UTF-8），失败时返回空。

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
环境配置（选一种即可）：

  方案 A — Tesseract（推荐，轻量）：
    1. 下载安装 Tesseract OCR：
       https://github.com/UB-Mannheim/tesseract/wiki
    2. 下载中文语言包 chi_sim.traineddata：
       https://github.com/tesseract-ocr/tessdata/raw/main/chi_sim.traineddata
       放到 Tesseract 安装目录的 tessdata 文件夹下
    3. pip install pytesseract PyMuPDF

  方案 B — PaddleOCR（准确率更高，约 500MB）：
    1. pip install paddlepaddle paddleocr PyMuPDF
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
"""

import sys
import os
import fitz  # PyMuPDF — PDF 页面渲染


# ═══════════════════════════════════════════════════════════════
# Tesseract OCR 引擎
# ═══════════════════════════════════════════════════════════════

def _ocr_tesseract(pdf_path: str) -> str:
    """使用 Tesseract OCR 提取 PDF 文本"""
    try:
        import pytesseract
    except ImportError:
        return ""

    # 自动检测 Tesseract 安装路径
    tesseract_paths = [
        r"C:\Program Files\Tesseract-OCR\tesseract.exe",
        r"C:\Program Files (x86)\Tesseract-OCR\tesseract.exe",
        r"D:\Tesseract-OCR\tesseract.exe",
    ]
    for tp in tesseract_paths:
        if os.path.exists(tp):
            pytesseract.pytesseract.tesseract_cmd = tp
            break

    try:
        doc = fitz.open(pdf_path)
    except Exception as e:
        print(f"[OCR] Cannot open PDF: {e}", file=sys.stderr)
        return ""

    all_text: list[str] = []
    total_pages = doc.page_count

    for page_idx in range(total_pages):
        try:
            page = doc[page_idx]
            # 150 DPI — 平衡速度和质量
            pix = page.get_pixmap(dpi=150)
            img_bytes = pix.tobytes("png")

            # PIL Image 包装（pytesseract 需要 PIL Image 或 numpy array）
            from PIL import Image
            import io as _io
            img = Image.open(_io.BytesIO(img_bytes))

            # chi_sim = 简体中文, eng = 英文（同时识别）
            text = pytesseract.image_to_string(
                img, lang="chi_sim+eng", config="--psm 6"
            )

            text = text.strip()
            if text:
                all_text.append(text)
        except Exception as e:
            # 可能缺少中文语言包
            if "chi_sim" in str(e):
                print(
                    "[OCR] Chinese language pack not found.\n"
                    "      Download chi_sim.traineddata from:\n"
                    "      https://github.com/tesseract-ocr/tessdata/raw/main/chi_sim.traineddata\n"
                    "      Place it in Tesseract's tessdata folder.",
                    file=sys.stderr,
                )
            else:
                print(f"[OCR] Page {page_idx + 1} error: {e}", file=sys.stderr)
            continue

    doc.close()
    return "\n\n".join(all_text)


# ═══════════════════════════════════════════════════════════════
# PaddleOCR 引擎（备用）
# ═══════════════════════════════════════════════════════════════

def _ocr_paddleocr(pdf_path: str) -> str:
    """使用 PaddleOCR 提取 PDF 文本"""
    try:
        from paddleocr import PaddleOCR
    except ImportError:
        return ""

    try:
        ocr = PaddleOCR(lang="ch", use_angle_cls=True, show_log=False)
        doc = fitz.open(pdf_path)
    except Exception as e:
        print(f"[OCR] PaddleOCR init failed: {e}", file=sys.stderr)
        return ""

    all_text: list[str] = []
    total_pages = doc.page_count

    for page_idx in range(total_pages):
        try:
            page = doc[page_idx]
            pix = page.get_pixmap(dpi=150)
            img_bytes = pix.tobytes("png")

            result = ocr.ocr(img_bytes, cls=True)

            page_lines: list[str] = []
            if result and result[0]:
                for line_info in result[0]:
                    text = line_info[1][0]
                    page_lines.append(text)

            if page_lines:
                all_text.append("\n".join(page_lines))
        except Exception as e:
            print(f"[OCR] Page {page_idx + 1} error: {e}", file=sys.stderr)
            continue

    doc.close()
    return "\n\n".join(all_text)


# ═══════════════════════════════════════════════════════════════
# PyMuPDF 文本层提取（支持 CJK/CID 字体）
# ═══════════════════════════════════════════════════════════════

def _extract_text_layer(pdf_path: str) -> str:
    """使用 PyMuPDF 提取 PDF 文本层（支持中文 CID 字体编码）"""
    try:
        doc = fitz.open(pdf_path)
    except Exception as e:
        print(f"[PDF] Cannot open: {e}", file=sys.stderr)
        return ""

    all_text: list[str] = []
    for page_idx in range(doc.page_count):
        try:
            text = doc[page_idx].get_text()
            text = text.strip()
            if text:
                all_text.append(text)
        except Exception:
            pass
    doc.close()
    return "\n\n".join(all_text)


# ═══════════════════════════════════════════════════════════════
# 主入口 — PDF 文本提取（PyMuPDF → OCR 回退）
# ═══════════════════════════════════════════════════════════════

def extract_text(pdf_path: str) -> str:
    """从 PDF 提取文本：优先文本层，回退 OCR"""
    if not os.path.exists(pdf_path):
        print(f"[PDF] File not found: {pdf_path}", file=sys.stderr)
        return ""

    # 第一步：尝试 PyMuPDF 文本层提取（支持 CJK/CID 字体）
    text = _extract_text_layer(pdf_path)
    if text.strip():
        return text

    # 第二步：文本层为空 → 扫描件，回退到 OCR
    text = _ocr_tesseract(pdf_path)
    if text.strip():
        return text

    # 第三步：PaddleOCR
    text = _ocr_paddleocr(pdf_path)
    if text.strip():
        return text

    return ""


def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: python pdf_ocr.py <pdf_file_path>", file=sys.stderr)
        sys.exit(1)

    # Windows: 强制 stdout 使用 UTF-8，避免 GBK 编码错误导致 C++ QProcess
    # 读取到截断/乱码的文本
    if sys.platform == "win32":
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    pdf_path = sys.argv[1]
    text = extract_text(pdf_path)

    if text.strip():
        print(text)
    else:
        print(
            "[PDF] No text extracted. Please check:\n"
            "  - PDF contains readable content (not blank pages)\n"
            "  - OCR engine is installed for scanned documents (see script header for setup)",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
