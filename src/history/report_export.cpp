#include "history/report_export.h"

#include <QFile>
#include <QPrinter>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTextDocumentWriter>
#include <QTextStream>
#include <QtGlobal>

namespace history {
namespace {

QString toQ(const std::string& s) { return QString::fromStdString(s); }

constexpr int kScoreDigits = 3;

/// 报告为打印文档：把模型输出里的 Markdown 标记剥离后再排版——与 T13 前检索页
/// 的显示语义一致（T14-S5 起覆盖常用全套：表格、行内代码、链接/图片、加粗/斜体、
/// 无序列表、引用、行首 # 标题）。
/// 处理策略：
///   - 表格分隔线行 / 水平线（仅由 | : - 和空白组成且含 ≥3 个 -）整行删除；
///     数据行的 | 转为空格（方案 a：表格降级为可读段落，不做 HTML 表格）；
///   - 链接 [文本](URL) / 图片 ![替代](URL) 只留可见文本——打印文档里 URL 不可点；
///   - 无序列表标记 - / * / + 转圆点（保留缩进）；有序列表 1. 本就可读，不动；
///   - 已知限制：不成对的 * 与单个 _ 一律不剥——保护正文标识符（如 case_admin_001）
///     与算式符号；表格内若真有此类字符同样保留。
/// 原始 Markdown 仍完整保存在历史库与 .md 导出中，此处只影响报告观感。
QString plainAnswer(const std::string& raw) {
    QString out = toQ(raw);
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    out.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    // 逐行块级处理：分隔线/水平线删除、竖线转空格、列表与引用标记
    static const QRegularExpression kDividerLine(
        QStringLiteral("^[|:\t \\-]+$"));
    static const QRegularExpression kUnorderedList(
        QStringLiteral("^([ \\t]*)[-*+][ \\t]+"));
    static const QRegularExpression kBlockQuote(
        QStringLiteral("^([ \\t]*)>[ \\t]?"));
    QStringList lines = out.split(QLatin1Char('\n'));
    for (QString& line : lines) {
        if (line.contains(kDividerLine) && line.count(QLatin1Char('-')) >= 3) {
            line.clear();  // 表格分隔线（|---|---|）或水平线（---）：整行删
            continue;
        }
        line.replace(QLatin1Char('|'), QStringLiteral("  "));
        line.replace(kUnorderedList, QStringLiteral("\\1• "));
        line.replace(kBlockQuote, QStringLiteral("\\1"));
    }
    out = lines.join(QLatin1Char('\n'));

    // 行内标记：图片先于链接（否则链接规则会吃掉 [alt](url) 留下孤立 !）
    out.replace(QRegularExpression(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)")),
                QStringLiteral("\\1"));
    out.replace(QRegularExpression(QStringLiteral("\\[([^\\]]+)\\]\\([^)]*\\)")),
                QStringLiteral("\\1"));
    out.remove(QStringLiteral("**"));
    out.replace(QRegularExpression(QStringLiteral("__([^_\n]+)__")),
                QStringLiteral("\\1"));
    out.replace(QRegularExpression(QStringLiteral("\\*([^*\n]+)\\*")),
                QStringLiteral("\\1"));
    out.replace(QRegularExpression(QStringLiteral("`([^`\n]*)`")),
                QStringLiteral("\\1"));
    out.replace(QRegularExpression(QStringLiteral("(^|\\n)#{1,6}\\s+")),
                QStringLiteral("\\1"));
    return out;
}

/// HTML 转义 + 折叠换行（QTextDocument 的 <p> 不渲染裸换行）。
/// 顺序关键：先转义再插 <br/>，否则标签会被转成 &lt;br/&gt; 显示出来
QString escText(const std::string& raw) {
    QString out = toQ(raw).toHtmlEscaped();
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    out.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    out.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
    return out;
}

/// 单行纯文本转义（表格单元格 / 标题等，不保留换行）
QString escCell(const std::string& raw) {
    QString out = toQ(raw);
    out.replace(QLatin1Char('\r'), QLatin1Char(' '));
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return out.trimmed().toHtmlEscaped();
}

QString scoreText(double score) {
    return QString::number(score, 'f', kScoreDigits);
}

/// 渲染前统一入口：setHtml + 返回文档（两写出函数共用，保证排版同源）
void loadReport(QTextDocument& doc, const std::string& reportHtml) {
    doc.setHtml(QString::fromStdString(reportHtml));
}

}  // namespace

std::string toReportHtml(const HistoryRecord& record) {
    QString html;
    QTextStream out(&html);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");
#else
    out.setEncoding(QStringConverter::Utf8);
#endif

    out << QStringLiteral(
        "<html><head><meta charset=\"utf-8\"></head>"
        "<body style=\"font-family: 'SimSun'; font-size: 10.5pt;\">");

    out << QStringLiteral("<h1 align=\"center\">%1</h1>")
               .arg(escCell("检索问答报告"));
    out << QStringLiteral("<hr/>");

    // 元信息：编号 / 时间 / 命中块数 / 回答状态（中断如实标注）
    out << QStringLiteral(
        "<p>记录编号：#%1　　提问时间：%2　　命中块数：%3</p>")
               .arg(record.id)
               .arg(escCell(record.createdAt))
               .arg(record.hitCount());
    if (record.interrupted) {
        const QString reason = record.note.empty()
                                   ? QStringLiteral("生成过程未正常结束")
                                   : toQ(record.note);
        out << QStringLiteral(
            "<p><b><font color=\"#B00000\">回答状态：⚠ 未完成（%1）"
            "</font></b>　回答以下内容为中断时刻已产出的部分。</p>")
                   .arg(escCell(reason.toStdString()));
    } else {
        out << QStringLiteral("<p>回答状态：完整</p>");
    }

    // 一、检索问题
    out << QStringLiteral("<h2>一、检索问题</h2>");
    out << QStringLiteral("<p>%1</p>").arg(escText(record.query));

    // 二、命中文书表格（兼作来源清单；历史只存 docId/块号/得分，无案号元数据）
    out << QStringLiteral("<h2>二、命中文书</h2>");
    if (record.sources.empty()) {
        out << QStringLiteral("<p>（本次回答没有引用任何文本块）</p>");
    } else {
        out << QStringLiteral(
            "<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" "
            "width=\"100%\">"
            "<tr><th>#</th><th>文书</th><th>块号</th><th>相关度</th></tr>");
        for (size_t i = 0; i < record.sources.size(); ++i) {
            const auto& item = record.sources[i];
            out << QStringLiteral(
                "<tr><td align=\"center\">%1</td><td>%2</td>"
                "<td align=\"center\">%3</td><td align=\"center\">%4</td></tr>")
                       .arg(static_cast<int>(i + 1))
                       .arg(escCell(item.docId))
                       .arg(item.chunkIndex)
                       .arg(scoreText(item.finalScore));
        }
        out << QStringLiteral("</table>");
    }

    // 三、命中段落节选（分块引用；快照 160 字符截断，明确标注）
    out << QStringLiteral("<h2>三、命中段落节选</h2>");
    if (record.sources.empty()) {
        out << QStringLiteral("<p>（无）</p>");
    } else {
        for (const auto& item : record.sources) {
            out << QStringLiteral(
                "<p><b>%1</b>　块 %2　相关度 %3　<font color=\"#666666\">"
                "（节选，完整原文以文档库为准）</font></p>"
                "<blockquote>%4</blockquote>")
                       .arg(escCell(item.docId))
                       .arg(item.chunkIndex)
                       .arg(scoreText(item.finalScore))
                       .arg(escText(item.snippet));
        }
    }

    // 四、AI 回答（中断时标题带「未完成」）
    if (record.interrupted) {
        out << QStringLiteral("<h2>四、AI 回答（未完成）</h2>");
    } else {
        out << QStringLiteral("<h2>四、AI 回答</h2>");
    }
    if (record.answer.empty()) {
        out << QStringLiteral("<p>（无回答内容）</p>");
    } else {
        out << QStringLiteral("<p>%1</p>").arg(escText(plainAnswer(record.answer).toStdString()));
    }

    // 尾注
    out << QStringLiteral(
        "<hr/><p style=\"font-size: 9pt; color: #666666;\">"
        "命中段落为问答当时的入库快照节选（至多 160 字符），完整原文请以文档库为准；"
        "相关度为检索阶段融合得分，保留 3 位小数。</p>");

    out << QStringLiteral("</body></html>");
    out.flush();
    return html.toStdString();
}

bool writeOdtFile(const QString& path, const std::string& reportHtml,
                  QString* errorOut) {
    if (path.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("导出路径为空");
        }
        return false;
    }

    QTextDocument doc;
    loadReport(doc, reportHtml);

    QTextDocumentWriter writer(path, QByteArrayLiteral("odf"));
    if (!writer.write(&doc)) {
        if (errorOut) {
            *errorOut = QStringLiteral(
                "无法写入 ODT 文件：%1（路径无效、磁盘已满或权限不足）").arg(path);
        }
        return false;
    }
    return true;
}

bool writePdfFile(const QString& path, const std::string& reportHtml,
                  QString* errorOut) {
    if (path.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("导出路径为空");
        }
        return false;
    }

    QTextDocument doc;
    loadReport(doc, reportHtml);

    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(path);

    doc.print(&printer);

    // QTextDocument::print() 不返回成功与否，用产物兜底校验：
    // 文件存在且非空才算成功（目录不存在/无权限时 Qt 静默失败，只能这样发现）
    QFile produced(path);
    if (!produced.exists() || produced.size() == 0) {
        if (errorOut) {
            *errorOut = QStringLiteral(
                "无法写入 PDF 文件：%1（路径无效、磁盘已满或权限不足）").arg(path);
        }
        return false;
    }
    return true;
}

}  // namespace history
