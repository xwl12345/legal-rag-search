// 法律 RAG 检索引擎 — 毕业答辩 PPT 构建脚本
const pptxgen = require("pptxgenjs");

const P = new pptxgen();
P.layout = "LAYOUT_WIDE"; // 13.33 × 7.5
P.author = "毕业答辩";
P.title = "法律文档智能检索引擎";

// ── 调色板（法律藏青 + 铜金）──
const BG_DARK = "14213D", PRIMARY = "1F3A5F", ACCENT = "B7791F";
const TEXT = "1F2937", MUTED = "64748B", TINT = "F1F4F8", LINE = "C9D3E0";
const WHITE = "FFFFFF", SUB = "8FA3BF"; // 深底上的次要文字
const F = "Microsoft YaHei";
const W = 13.33, H = 7.5, M = 0.5;

const T = (o) => Object.assign({ fontFace: F }, o || {});
const title = (s, txt) => s.addText(txt, T({ x: M, y: 0.38, w: W - 2 * M, h: 0.62, fontSize: 30, bold: true, color: PRIMARY, margin: 0 }));
const kicker = (s, txt) => s.addText(txt, T({ x: M, y: 0.08, w: 6, h: 0.26, fontSize: 12, color: ACCENT, bold: true, charSpacing: 3, margin: 0 }));
const srcNote = (s, txt) => s.addText(txt, T({ x: M, y: 7.08, w: W - 2 * M, h: 0.3, fontSize: 11, color: MUTED, margin: 0 }));
const hair = (s, x, y, w) => s.addShape(P.shapes.LINE, { x, y, w, h: 0, line: { color: LINE, width: 1 } });

// ═════════ S1 封面（深色）═════════
{
  const s = P.addSlide(); s.background = { color: BG_DARK };
  s.addText("毕 业 设 计 答 辩", T({ x: 1.1, y: 1.15, w: 6, h: 0.4, fontSize: 15, color: ACCENT, bold: true, charSpacing: 6, margin: 0 }));
  s.addText("法律文档智能检索引擎", T({ x: 1.05, y: 1.75, w: 11.2, h: 1.15, fontSize: 52, bold: true, color: WHITE, margin: 0 }));
  s.addText("基于 BM25 + 向量混合检索与 DeepSeek 大模型的 RAG 系统", T({ x: 1.1, y: 3.05, w: 11, h: 0.5, fontSize: 19, color: SUB, margin: 0 }));
  hair(s, 1.1, 4.1, 4.2);
  const meta = [["答辩人", "＿＿＿＿"], ["学号", "＿＿＿＿＿＿"], ["专业", "＿＿＿＿＿＿"], ["指导教师", "＿＿＿＿"]];
  meta.forEach((m, i) => {
    s.addText(m[0], T({ x: 1.1 + i * 2.85, y: 4.4, w: 2.7, h: 0.3, fontSize: 12, color: SUB, margin: 0 }));
    s.addText(m[1], T({ x: 1.1 + i * 2.85, y: 4.72, w: 2.7, h: 0.4, fontSize: 16, color: WHITE, bold: true, margin: 0 }));
  });
  s.addText([
    { text: "C++17 / Qt 桌面应用", options: {} },
    { text: "   ·   ", options: { color: ACCENT } },
    { text: "cppjieba 法律词典", options: {} },
    { text: "   ·   ", options: { color: ACCENT } },
    { text: "DeepSeek 流式生成", options: {} },
  ], T({ x: 1.1, y: 6.35, w: 11, h: 0.4, fontSize: 14, color: SUB, margin: 0 }));
  s.addNotes("开场 30 秒：一句话立意——做一个懂法律文书的检索系统，回答基于真实文书、可溯源。不念标题。");
}

// ═════════ S2 提纲 ═════════
{
  const s = P.addSlide(); s.background = { color: WHITE };
  kicker(s, "CONTENTS"); title(s, "汇报提纲");
  const items = [
    ["01", "选题背景与需求", "法律文书检索的三个痛点 → RAG 方案"],
    ["02", "系统架构", "混合检索器 + 法律定制索引 + 流式生成"],
    ["03", "核心技术", "安全分块 · 法律词典 · 融合排序 · 双模式路由"],
    ["04", "验证与实验", "45 项测试 · 量化评测 · 真实文书实战"],
    ["05", "总结与展望", "不足的诚实清单与改进路线"],
  ];
  items.forEach((it, i) => {
    const y = 1.35 + i * 1.12;
    s.addText(it[0], T({ x: 1.0, y, w: 1.3, h: 0.9, fontSize: 40, bold: true, color: i === 3 ? ACCENT : "B9C4D4", margin: 0 }));
    s.addText(it[1], T({ x: 2.5, y: y + 0.05, w: 9.5, h: 0.45, fontSize: 21, bold: true, color: TEXT, margin: 0 }));
    s.addText(it[2], T({ x: 2.5, y: y + 0.52, w: 9.5, h: 0.35, fontSize: 13, color: MUTED, margin: 0 }));
    if (i < 4) hair(s, 1.0, y + 1.0, 11.3);
  });
}

// ═════════ S3 背景与痛点 ═════════
{
  const s = P.addSlide(); s.background = { color: WHITE };
  kicker(s, "BACKGROUND"); title(s, "选题背景：法律文书检索的三个痛点");
  const rows = [
    ["01", "词汇鸿沟", "用户说「公司把钱吞了」，文书写的是「职务侵占罪」——口语与法术语之间，关键词检索跨不过去"],
    ["02", "检索粒度粗", "无法按案号 / 法院 / 案件类型 / 年份过滤，「这些案件一共有多少起」这类跨文档问题无从回答"],
    ["03", "只有链接没有答案", "命中几十份长文书后，用户仍需自行阅读、归纳结论，阅读成本高且容易遗漏"],
  ];
  rows.forEach((r, i) => {
    const y = 1.4 + i * 1.28;
    s.addText(r[0], T({ x: 0.9, y: y + 0.06, w: 0.95, h: 0.8, fontSize: 32, bold: true, color: "C4CEDC", margin: 0 }));
    s.addText(r[1], T({ x: 2.05, y, w: 10.2, h: 0.45, fontSize: 20, bold: true, color: PRIMARY, margin: 0 }));
    s.addText(r[2], T({ x: 2.05, y: y + 0.5, w: 10.2, h: 0.6, fontSize: 14.5, color: TEXT, margin: 0 }));
    if (i < 2) hair(s, 0.9, y + 1.12, 11.5);
  });
  s.addShape(P.shapes.RECTANGLE, { x: 0.9, y: 5.55, w: 11.5, h: 1.1, fill: { color: TINT } });
  s.addText([
    { text: "解决思路：RAG 检索增强生成", options: { bold: true, color: ACCENT, fontSize: 17 } },
    { text: "  ——  回答必须基于检索到的真实文书：可溯源、抑制幻觉、语料即插即用", options: { color: TEXT, fontSize: 15 } },
  ], T({ x: 1.2, y: 5.55, w: 11, h: 1.1, valign: "middle", margin: 0 }));
  s.addNotes("三个痛点各配一个例子；最后一句话立意。控制在 1 分钟。");
}

// ═════════ S4 系统架构 ═════════
{
  const s = P.addSlide(); s.background = { color: WHITE };
  kicker(s, "ARCHITECTURE"); title(s, "系统架构：四层流水线");
  const BX = 0.74, BW = 11.85;
  const band = (y, h, label) => {
    s.addShape(P.shapes.RECTANGLE, { x: BX, y, w: BW, h, fill: { color: TINT } });
    s.addText(label, T({ x: BX + 0.15, y: y + h / 2 - 0.2, w: 1.75, h: 0.4, fontSize: 15, bold: true, color: PRIMARY, margin: 0 }));
  };
  const chip = (x, y, w, h, txt, opts) => {
    s.addShape(P.shapes.ROUNDED_RECTANGLE, Object.assign({ x, y, w, h, fill: { color: WHITE }, line: { color: LINE, width: 1 }, rectRadius: 0.06 }, opts || {}));
    s.addText(txt, T({ x, y, w, h, fontSize: 12.5, color: TEXT, align: "center", valign: "middle", margin: 0.02 }));
  };
  const arrow = (x, y1, y2) => s.addShape(P.shapes.LINE, { x, y: y1, w: 0, h: y2 - y1, line: { color: PRIMARY, width: 2, endArrowType: "triangle" } });

  band(1.1, 0.78, "界面层");
  chip(2.62, 1.26, 9.7, 0.48, "Qt Widgets 桌面应用 — 搜索 / 案件类型·法院·年份三维筛选 / 检索结果 + AI 回答双栏");
  arrow(6.67, 1.9, 2.14);
  band(2.16, 1.42, "检索层");
  chip(2.62, 2.55, 3.0, 0.72, "BM25 排序\nk1=1.5  b=0.75");
  s.addShape(P.shapes.LINE, { x: 5.62, y: 2.91, w: 0.5, h: 0, line: { color: PRIMARY, width: 2, endArrowType: "triangle" } });
  chip(6.14, 2.55, 2.6, 0.72, "加权融合\n0.4 × BM25 + 0.6 × 余弦", { line: { color: ACCENT, width: 1.5 } });
  s.addShape(P.shapes.LINE, { x: 8.74, y: 2.91, w: 0.5, h: 0, line: { color: PRIMARY, width: 2, beginArrowType: "triangle" } });
  chip(9.26, 2.55, 3.05, 0.72, "向量余弦引擎\nEmbedding API · 可降级");
  arrow(6.67, 3.58, 3.82);
  band(3.84, 1.42, "索引层");
  chip(2.62, 4.32, 2.35, 0.72, "UTF-8 安全分块\n512B / 重叠 50");
  chip(5.15, 4.32, 2.35, 0.72, "cppjieba 分词\n+350 法律词典");
  chip(7.68, 4.32, 2.35, 0.72, "倒排索引\nchunk 级 postings");
  chip(10.21, 4.32, 2.11, 0.72, "元数据提取\n案号·法院·日期·类型");
  arrow(6.67, 5.26, 5.5);
  band(5.52, 1.2, "生成层");
  chip(2.62, 5.92, 4.6, 0.6, "OCR 三级级联（扫描件兜底）\n文本层 → RapidOCR → Tesseract → Paddle");
  chip(7.5, 5.92, 4.82, 0.6, "DeepSeek Chat\nSSE 流式 + 法律四段式 Prompt 模板");
  srcNote(s, "端到端：导入 → 分块 → 分词 → 建索引 → 检索 → 路由 → 流式生成；核心算法（解析/分块/索引/BM25/融合）全部手写实现");
  s.addNotes("1.5 分钟：自底向上讲一条链路即可，不要逐框念。强调核心算法手写 + 无重型外部依赖。");
}

// ═════════ S5 核心技术① 文档摄入 ═════════
{
  const s = P.addSlide(); s.background = { color: WHITE };
  kicker(s, "TECHNIQUE 01"); title(s, "文档摄入：为真实 PDF 而设计的链路");
  const steps = [
    ["1", "格式路由", "TXT/MD/CSV/JSON/XML 直读；PDF 走自研解析器（FlateDecode/ASCII85）"],
    ["2", "OCR 三级级联", "文本层为空 → RapidOCR → Tesseract → PaddleOCR；Python 环境自动探测"],
    ["3", "UTF-8 安全分块", "512 字节 + 50 重叠；切前回退 UTF-8 边界防切断汉字，预算后半段找自然断点"],
    ["4", "法律分词", "cppjieba + 350 条法律词典（10 大类），「知识产权法院」等复合术语不切散"],
    ["5", "元数据提取", "手写字节扫描提取案号 / 法院 / 日期（含中文数字）/ 类型 / 当事人 / 程序"],
  ];
  steps.forEach((st, i) => {
    const y = 1.32 + i * 1.06;
    s.addShape(P.shapes.OVAL, { x: 0.95, y: y + 0.07, w: 0.62, h: 0.62, fill: { color: i === 2 ? ACCENT : PRIMARY } });
    s.addText(st[0], T({ x: 0.95, y: y + 0.07, w: 0.62, h: 0.62, fontSize: 22, bold: true, color: WHITE, align: "center", valign: "middle", margin: 0 }));
    s.addText(st[1], T({ x: 1.85, y, w: 3.1, h: 0.75, fontSize: 18, bold: true, color: PRIMARY, valign: "middle", margin: 0 }));
    s.addText(st[2], T({ x: 5.1, y, w: 7.35, h: 0.75, fontSize: 13.5, color: TEXT, valign: "middle", margin: 0 }));
    if (i < 4) hair(s, 1.85, y + 0.92, 10.6);
  });
  s.addText([
    { text: "实测：", options: { bold: true, color: PRIMARY } },
    { text: "21 篇模拟文书 → 118 块；元数据提取 案号 21/21 · 日期 21/21 · 类型 21/21；真实判决书秒级导入，20 页扫描件 OCR 全文可检索", options: { color: TEXT } },
  ], T({ x: 0.95, y: 6.62, w: 11.4, h: 0.45, fontSize: 13, margin: 0 }));
  s.addNotes("重点讲 2 和 3：OCR 级联是真实文书实测逼出来的设计；分块讲'字节预算+UTF-8 边界'这个细节最能体现深度。");
}

// ═════════ S6 核心技术② 混合检索与路由 ═════════
{
  const s = P.addSlide(); s.background = { color: WHITE };
  kicker(s, "TECHNIQUE 02"); title(s, "混合检索与聚合/聚焦双模式路由");
  // 左：公式 + 双路说明
  s.addShape(P.shapes.RECTANGLE, { x: 0.9, y: 1.35, w: 5.9, h: 2.5, fill: { color: TINT } });
  s.addText("最终得分 = 0.4 × BM25 + 0.6 × 余弦", T({ x: 1.2, y: 1.6, w: 5.3, h: 0.5, fontSize: 19, bold: true, color: PRIMARY, margin: 0 }));
  s.addText("( 各自按路内最大值归一化 )", T({ x: 1.2, y: 2.12, w: 5.3, h: 0.3, fontSize: 12, color: MUTED, margin: 0 }));
  s.addText([
    { text: "BM25：精确词项匹配，法律术语强判别", options: { breakLine: true } },
    { text: "向量：语义泛化，弥合「诈骗 vs 诈骗罪」类词汇失配", options: {} },
  ], T({ x: 1.2, y: 2.55, w: 5.4, h: 1.1, fontSize: 13.5, color: TEXT, paraSpaceAfter: 6, margin: 0 }));
  s.addShape(P.shapes.RECTANGLE, { x: 0.9, y: 4.05, w: 5.9, h: 1.15, fill: { color: WHITE }, line: { color: ACCENT, width: 1.5 } });
  s.addText([
    { text: "优雅降级", options: { bold: true, color: ACCENT, fontSize: 15, breakLine: true } },
    { text: "Embedding 服务不可用时自动退化为纯 BM25，检索功能零损失——被回归测试覆盖的一等路径", options: { fontSize: 12.5, color: TEXT } },
  ], T({ x: 1.15, y: 4.15, w: 5.5, h: 0.95, paraSpaceAfter: 4, margin: 0 }));
  s.addText("评测佐证：易混案由区分 3/3 全对（职务侵占 ≠ 损害公司利益）", T({ x: 0.95, y: 5.5, w: 5.9, h: 0.7, fontSize: 13, color: TEXT, margin: 0 }));
  // 右：双模式路由
  s.addText("聚合 / 聚焦 双模式路由", T({ x: 7.2, y: 1.35, w: 5.2, h: 0.4, fontSize: 17, bold: true, color: PRIMARY, margin: 0 }));
  s.addShape(P.shapes.ROUNDED_RECTANGLE, { x: 7.2, y: 1.85, w: 5.2, h: 0.55, fill: { color: PRIMARY }, rectRadius: 0.06 });
  s.addText("用户查询", T({ x: 7.2, y: 1.85, w: 5.2, h: 0.55, fontSize: 14, color: WHITE, align: "center", valign: "middle", margin: 0 }));
  s.addShape(P.shapes.LINE, { x: 9.8, y: 2.4, w: 0, h: 0.35, line: { color: PRIMARY, width: 2, endArrowType: "triangle" } });
  s.addText("「这些案件 / 汇总 / 一共 …」短语匹配（窄表防误判）", T({ x: 7.2, y: 2.78, w: 5.2, h: 0.3, fontSize: 11.5, color: MUTED, margin: 0 }));
  s.addShape(P.shapes.RECTANGLE, { x: 7.2, y: 3.15, w: 2.5, h: 1.7, fill: { color: TINT } });
  s.addText([
    { text: "聚合模式", options: { bold: true, fontSize: 15, color: ACCENT, breakLine: true } },
    { text: "跨文档统计问题\ntopK 扩至 50\n每文档 ≤2 块去重\n注入全库元数据", options: { fontSize: 11.5 } },
  ], T({ x: 7.32, y: 3.25, w: 2.26, h: 1.5, paraSpaceAfter: 3, margin: 0 }));
  s.addShape(P.shapes.RECTANGLE, { x: 9.9, y: 3.15, w: 2.5, h: 1.7, fill: { color: TINT } });
  s.addText([
    { text: "聚焦模式", options: { bold: true, fontSize: 15, color: PRIMARY, breakLine: true } },
    { text: "常规检索\n保持融合排序\n注入命中文档\n元数据", options: { fontSize: 11.5 } },
  ], T({ x: 10.02, y: 3.25, w: 2.26, h: 1.5, paraSpaceAfter: 3, margin: 0 }));
  s.addText("实测「这些案件一共有多少起」→ 5 条跨文档多样化结果，每文档 ≤2 块", T({ x: 7.2, y: 5.05, w: 5.2, h: 0.6, fontSize: 12.5, color: TEXT, margin: 0 }));
  srcNote(s, "权重为编译期常量，可网格搜索标定；聚合短语表刻意取窄，避免误伤聚焦查询");
  s.addNotes("公式讲动机：法律语料术语密度高 BM25 强，给语义路留 0.6。降级机制讲成鲁棒性亮点。");
}

// ═════════ S7 核心技术③ 法律问答生成 ═════════
{
  const s = P.addSlide(); s.background = { color: WHITE };
  kicker(s, "TECHNIQUE 03"); title(s, "可信法律问答：结构化 Prompt + 流式生成");
  s.addText("法律上下文自动检测 → 切换四段式模板", T({ x: 0.9, y: 1.3, w: 6, h: 0.4, fontSize: 16, bold: true, color: PRIMARY, margin: 0 }));
  const secs = [
    ["一、案件概述", "当事人 / 案号 / 程序"],
    ["二、法律分析", "逐点引用检索到的条文与事实"],
    ["三、结论", "基于文书的明确回应"],
    ["四、参考来源", "强制标注【来源 N】与案号"],
  ];
  secs.forEach((sec, i) => {
    const y = 1.85 + i * 0.92;
    s.addShape(P.shapes.RECTANGLE, { x: 0.9, y, w: 5.9, h: 0.78, fill: { color: i === 3 ? "FBF3E4" : TINT } });
    s.addText(sec[0], T({ x: 1.15, y, w: 2.2, h: 0.78, fontSize: 15, bold: true, color: PRIMARY, valign: "middle", margin: 0 }));
    s.addText(sec[1], T({ x: 3.45, y, w: 3.2, h: 0.78, fontSize: 12.5, color: TEXT, valign: "middle", margin: 0 }));
  });
  s.addText("界面固定附「仅供参考，不构成法律意见」免责声明", T({ x: 0.9, y: 5.65, w: 6, h: 0.35, fontSize: 12.5, color: MUTED, margin: 0 }));
  // 右：幻觉防护
  s.addText("四层幻觉防护", T({ x: 7.4, y: 1.3, w: 5, h: 0.4, fontSize: 16, bold: true, color: PRIMARY, margin: 0 }));
  const guards = [
    ["RAG 架构", "回答强制基于检索到的真实文书片段"],
    ["引用约束", "系统提示要求标注案号与【来源 N】"],
    ["低温生成", "temperature = 0.3，降低发散"],
    ["可溯源", "左侧结果列表展示每条来源与相关度得分"],
  ];
  guards.forEach((g, i) => {
    const y = 1.85 + i * 0.98;
    s.addShape(P.shapes.ROUNDED_RECTANGLE, { x: 7.4, y, w: 5.0, h: 0.84, fill: { color: TINT }, rectRadius: 0.05 });
    s.addText(g[0], T({ x: 7.62, y: y + 0.08, w: 4.6, h: 0.32, fontSize: 13.5, bold: true, color: ACCENT, margin: 0 }));
    s.addText(g[1], T({ x: 7.62, y: y + 0.42, w: 4.6, h: 0.34, fontSize: 12, color: TEXT, margin: 0 }));
  });
  s.addText("SSE 流式解析：只消费完整行、残行留缓冲，逐字上屏", T({ x: 0.9, y: 6.15, w: 6, h: 0.35, fontSize: 12.5, color: MUTED, margin: 0 }));
  s.addNotes("把'参考来源'一格讲重：这是可信性的核心设计，答辩演示时会看到真实输出。");
}

// ═════════ S8 现场演示（深色）═════════
{
  const s = P.addSlide(); s.background = { color: BG_DARK };
  s.addText("LIVE DEMO", T({ x: 1.1, y: 1.5, w: 6, h: 0.4, fontSize: 15, color: ACCENT, bold: true, charSpacing: 6, margin: 0 }));
  s.addText("现场演示", T({ x: 1.05, y: 2.0, w: 8, h: 1.0, fontSize: 48, bold: true, color: WHITE, margin: 0 }));
  const bu = () => ({ code: "25B8", indent: 12 });
  s.addText([
    { text: "混合检索排序 — 专利案 Top-1/2，逐条 BM25 分项得分", options: { bullet: bu(), breakLine: true } },
    { text: "元数据筛选 — 案件类型实时收敛结果", options: { bullet: bu(), breakLine: true } },
    { text: "聚合模式路由 — 跨文档统计 + 文档去重", options: { bullet: bu(), breakLine: true } },
    { text: "真实判决书导入 — 文字型 PDF 秒级入库", options: { bullet: bu(), breakLine: true } },
    { text: "流式法律问答 — 四段式回答 + 来源引用 + 免责声明", options: { bullet: bu() } },
  ], T({ x: 1.1, y: 3.3, w: 10.5, h: 2.6, fontSize: 17, color: "D8E1EE", paraSpaceAfter: 10, margin: 0 }));
  s.addText("扫描件 OCR 演示（约 3 分钟/20 页）已备录屏；断网时检索功能全程本地可用", T({ x: 1.1, y: 6.4, w: 11, h: 0.4, fontSize: 13, color: SUB, margin: 0 }));
  s.addNotes("演示约 3 分钟，严格按 defense_process.md 的 9 步脚本：先念查询词再看结果。");
}

// ═════════ S9 功能验证 ═════════
{
  const s = P.addSlide(); s.background = { color: WHITE };
  kicker(s, "VERIFICATION"); title(s, "功能验证：三层验证体系");
  const cards = [
    ["45 / 45", "自动化测试", "解析 · 分块 · 分词 · 倒排索引 · BM25 · 降级检索 · PDF · 法律词典 · 元数据 · E2E + 回归测试"],
    ["8 / 8", "GUI 端到端验证", "导入 21 篇→118 块 · 聚焦/聚合检索 · 三维筛选 · OCR 诊断 · 无 Key 降级引导"],
    ["2 份", "真实文书全流程", "文字型判决书秒级入库；20 页扫描件 OCR 后可检索、可引用作答"],
  ];
  cards.forEach((c, i) => {
    const x = 0.9 + i * 3.95;
    s.addShape(P.shapes.RECTANGLE, { x, y: 1.4, w: 3.6, h: 2.5, fill: { color: TINT } });
    s.addText(c[0], T({ x, y: 1.62, w: 3.6, h: 0.85, fontSize: 44, bold: true, color: i === 2 ? ACCENT : PRIMARY, align: "center", margin: 0 }));
    s.addText(c[1], T({ x, y: 2.52, w: 3.6, h: 0.4, fontSize: 16, bold: true, color: TEXT, align: "center", margin: 0 }));
    s.addText(c[2], T({ x: x + 0.25, y: 2.95, w: 3.1, h: 0.9, fontSize: 11.5, color: MUTED, margin: 0 }));
  });
  s.addText("真实文书实测暴露 5 个工程问题 — 全部根因定位并修复", T({ x: 0.9, y: 4.25, w: 11.5, h: 0.45, fontSize: 18, bold: true, color: PRIMARY, margin: 0 }));
  const fixes = [
    ["CID 字体 PDF 文本层为空", "OCR 级联兜底（PyMuPDF 完整提取）"],
    ["商店占位 python 遮蔽真实解释器", "官方安装位置优先探测"],
    ["扫描件 OCR 期间界面假死 3 分钟", "事件循环等待，窗口保持响应"],
    ["HTTPS 报 TLS initialization failed", "部署 OpenSSL 1.1 运行库"],
    ["AI 回答收尾报 substr 越界", "重写 SSE 解析，只消费完整行"],
  ];
  fixes.forEach((f, i) => {
    const y = 4.85 + i * 0.44;
    s.addText("▪", T({ x: 0.95, y, w: 0.3, h: 0.35, fontSize: 12, color: ACCENT, margin: 0 }));
    s.addText(f[0], T({ x: 1.3, y, w: 5.4, h: 0.38, fontSize: 13, color: TEXT, margin: 0 }));
    s.addText("→  " + f[1], T({ x: 6.9, y, w: 5.6, h: 0.38, fontSize: 13, color: PRIMARY, margin: 0 }));
    if (i < 4) hair(s, 0.95, y + 0.4, 11.4);
  });
  srcNote(s, "每项根因分析已沉淀至 TROUBLESHOOTING.md（#17–#21），核心路径均有回归测试守护");
  s.addNotes("如果被追问细节：SSE 越界那个最有讲头——差一错误，流断在行中间才触发。");
}

// ═════════ S10 量化评测 ═════════
{
  const s = P.addSlide(); s.background = { color: WHITE };
  kicker(s, "EVALUATION"); title(s, "量化评测：检索质量指标");
  s.addChart(P.charts.BAR, [{
    name: "得分", labels: ["Hit@5", "Recall@10", "MRR"], values: [0.957, 0.957, 0.913],
  }], {
    x: 0.7, y: 1.45, w: 6.3, h: 4.6, barDir: "col",
    chartColors: [PRIMARY, PRIMARY, ACCENT], varyColors: true,
    chartArea: { fill: { color: "FFFFFF" } },
    catAxisLabelColor: MUTED, valAxisLabelColor: MUTED,
    catAxisLabelFontFace: F, valAxisLabelFontFace: F,
    valAxisMaxVal: 1.0, valAxisMinVal: 0, valAxisMajorUnit: 0.25,
    valGridLine: { color: "E2E8F0", size: 0.5 }, catGridLine: { style: "none" },
    showValue: true, dataLabelPosition: "outEnd", dataLabelColor: TEXT,
    dataLabelFormatCode: "0.000",
    dataLabelFontFace: F, dataLabelFontSize: 13, showLegend: false,
  });
  const pts = [
    ["23 条标注查询集", "案情描述 / 法律术语 / 易混区分 / 多相关，人工 qrels 标注，文档级排名"],
    ["易混案由区分 3/3", "职务侵占 ≠ 损害公司利益；商标行政无效 ≠ 商标民事侵权"],
    ["口语改写可命中", "「借钱不还被起诉」相关文书排名第 2"],
    ["一个有价值的失败", "「电信诈骗」未命中：口语「诈骗/数额较大」vs 文书「诈骗罪/数额特别巨大」——词汇失配，正是引入向量语义检索的动机"],
  ];
  pts.forEach((p, i) => {
    const y = 1.5 + i * 1.32;
    s.addText(p[0], T({ x: 7.35, y, w: 5.1, h: 0.4, fontSize: 15.5, bold: true, color: i === 3 ? ACCENT : PRIMARY, margin: 0 }));
    s.addText(p[1], T({ x: 7.35, y: y + 0.42, w: 5.1, h: 0.8, fontSize: 12.5, color: TEXT, margin: 0 }));
  });
  srcNote(s, "评测程序 test/eval_retrieval.cpp · 逐查询明细见 docs/defense/evaluation.md · 语料 21 篇 / 118 块");
  s.addNotes("先报三个数，再重点讲失败案例——它把'不足'讲成了'设计依据'。");
}

// ═════════ S11 真实文书实战 ═════════
{
  const s = P.addSlide(); s.background = { color: WHITE };
  kicker(s, "REAL-WORLD"); title(s, "真实文书实战：从模拟语料到生产级检验");
  const tl = [
    ["文字型判决书 PDF", "WPS 导出 · CID 字体", "9 页全文提取 → 检索命中被告公司名", " PRIMARY"],
    ["20 页扫描件起诉状", "15.8 MB · 纯图片页", "OCR 全文可检索 · 元数据自动提取 · AI 引用作答", ""],
  ];
  tl.forEach((t, i) => {
    const x = 0.9 + i * 6.0;
    s.addShape(P.shapes.RECTANGLE, { x, y: 1.35, w: 5.6, h: 1.5, fill: { color: TINT } });
    s.addText(t[0], T({ x: x + 0.25, y: 1.5, w: 5.1, h: 0.4, fontSize: 17, bold: true, color: PRIMARY, margin: 0 }));
    s.addText(t[1], T({ x: x + 0.25, y: 1.92, w: 5.1, h: 0.32, fontSize: 12, color: MUTED, margin: 0 }));
    s.addText(t[2], T({ x: x + 0.25, y: 2.28, w: 5.1, h: 0.5, fontSize: 13, color: TEXT, margin: 0 }));
  });
  s.addText("实测暴露 → 根因定位 → 修复沉淀（5 个工程问题）", T({ x: 0.9, y: 3.25, w: 11.5, h: 0.45, fontSize: 18, bold: true, color: PRIMARY, margin: 0 }));
  const flow = [
    ["发现", "真实环境才出现的失败：CID 字体、商店占位 python、OCR 假死、TLS 缺库、SSE 越界"],
    ["定位", "逐词核对分词结果、逐层检查 DLL 依赖、实测单页 OCR 耗时 8 秒"],
    ["修复", "OCR 级联 + 标准位置探测 + 事件循环等待 + OpenSSL 部署 + 标准流解析"],
    ["沉淀", "TROUBLESHOOTING.md #17–#21 + 回归测试守护核心路径"],
  ];
  flow.forEach((f, i) => {
    const y = 3.85 + i * 0.78;
    s.addShape(P.shapes.OVAL, { x: 0.95, y: y + 0.05, w: 0.5, h: 0.5, fill: { color: i === 3 ? ACCENT : PRIMARY } });
    s.addText(String(i + 1), T({ x: 0.95, y: y + 0.05, w: 0.5, h: 0.5, fontSize: 16, bold: true, color: WHITE, align: "center", valign: "middle", margin: 0 }));
    s.addText(f[0], T({ x: 1.65, y, w: 1.2, h: 0.6, fontSize: 16, bold: true, color: TEXT, valign: "middle", margin: 0 }));
    s.addText(f[1], T({ x: 3.0, y, w: 9.3, h: 0.6, fontSize: 13.5, color: TEXT, valign: "middle", margin: 0 }));
  });
  srcNote(s, "模拟语料验证功能正确性，真实文书验证工程健壮性 —— 两者缺一不可");
  s.addNotes("这页回应'你的系统处理过真实文档吗'。五个修复的细节在答辩报告 §4.3。");
}

// ═════════ S12 不足与展望 ═════════
{
  const s = P.addSlide(); s.background = { color: WHITE };
  kicker(s, "LIMITATIONS & FUTURE WORK"); title(s, "不足与展望");
  const rows = [
    ["索引持久化", "索引驻留内存，重启需重导（扫描件需重跑 OCR）", "落盘文档文本，启动时秒级重建内存索引，无需重复 OCR"],
    ["向量检索接入", "接口与融合框架就绪，embedding 提供方待可配置", "抽象 BaseURL/Model，接入智谱 / 硅基流动 / Ollama 本地模型"],
    ["排序精调", "无重排阶段；存在词汇失配失败案例", "交叉编码器重排 Top-50；法律同义词归一化（诈骗→诈骗罪）"],
    ["评测规模", "23 条标注查询为演示级规模", "扩充至 CAIL 等公开裁判文书数据集，报告置信区间"],
  ];
  rows.forEach((r, i) => {
    const y = 1.4 + i * 1.3;
    s.addText(r[0], T({ x: 0.9, y, w: 2.6, h: 0.45, fontSize: 18, bold: true, color: PRIMARY, margin: 0 }));
    s.addText(r[1], T({ x: 0.9, y: y + 0.48, w: 5.4, h: 0.62, fontSize: 12.5, color: MUTED, margin: 0 }));
    s.addShape(P.shapes.LINE, { x: 6.6, y: y + 0.15, w: 0, h: 0.85, line: { color: LINE, width: 1 } });
    s.addText(r[2], T({ x: 6.9, y: y + 0.1, w: 5.5, h: 1.0, fontSize: 13.5, color: TEXT, valign: "middle", margin: 0 }));
    if (i < 3) hair(s, 0.9, y + 1.16, 11.5);
  });
  s.addNotes("主动讲透第一条（持久化）+ 第二条（向量现状），把评委要挖的先说了。");
}

// ═════════ S13 结语（深色）═════════
{
  const s = P.addSlide(); s.background = { color: BG_DARK };
  s.addText("一条完整、可信、可溯源的法律 RAG 链路", T({ x: 1.1, y: 1.7, w: 11.1, h: 1.6, fontSize: 38, bold: true, color: WHITE, margin: 0 }));
  s.addText("文档摄入 → 法律定制索引 → 混合检索 → 意图路由 → 流式生成", T({ x: 1.1, y: 3.35, w: 11, h: 0.5, fontSize: 17, color: SUB, margin: 0 }));
  const stats = [["45", "自动化测试"], ["0.957", "Hit@5"], ["21+2", "模拟+真实文书"], ["5", "实测问题修复"]];
  stats.forEach((st, i) => {
    const x = 1.1 + i * 2.85;
    s.addText(st[0], T({ x, y: 4.35, w: 2.6, h: 0.75, fontSize: 40, bold: true, color: ACCENT, margin: 0 }));
    s.addText(st[1], T({ x, y: 5.12, w: 2.6, h: 0.35, fontSize: 13, color: SUB, margin: 0 }));
  });
  hair(s, 1.1, 6.1, 11.1);
  s.addText("恳请各位老师批评指正", T({ x: 1.1, y: 6.35, w: 11, h: 0.5, fontSize: 18, color: WHITE, margin: 0 }));
  s.addNotes("收尾 20 秒：念标题一句话 + 四个数字，然后致谢。");
}

P.writeFile({ fileName: "docs/defense/法律RAG检索引擎-答辩PPT.pptx" }).then(() => console.log("PPTX written"));
