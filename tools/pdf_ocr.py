# -*- coding: utf-8 -*-
"""
PDF OCR 文本提取脚本
适用于图片型/扫描件 PDF，将每页渲染为图像后通过 OCR 识别文字。

引擎优先级（自动检测）：
  1. Tesseract OCR（轻量，需手动安装 + chi_sim 中文语言包）
  2. PaddleOCR（准确率高，需 pip install paddlepaddle paddleocr）

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
# 主入口 — 自动选择可用引擎
# ═══════════════════════════════════════════════════════════════

def extract_text_with_ocr(pdf_path: str) -> str:
    """对 PDF 执行 OCR，自动选择可用引擎"""
    if not os.path.exists(pdf_path):
        print(f"[OCR] File not found: {pdf_path}", file=sys.stderr)
        return ""

    # 优先尝试 Tesseract（更快、更轻）
    text = _ocr_tesseract(pdf_path)
    if text.strip():
        return text

    # 回退到 PaddleOCR
    text = _ocr_paddleocr(pdf_path)
    if text.strip():
        return text

    return ""


def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: python pdf_ocr.py <pdf_file_path>", file=sys.stderr)
        sys.exit(1)

    pdf_path = sys.argv[1]
    text = extract_text_with_ocr(pdf_path)

    if text.strip():
        print(text)
    else:
        print(
            "[OCR] No text extracted. Please check:\n"
            "  - PDF contains readable images (not blank pages)\n"
            "  - OCR engine is installed (see script header for setup)",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
