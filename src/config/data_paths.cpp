#include "config/app_config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <mutex>

namespace config {

std::string dataFilePath(const char* fileName) {
    // 一次性迁移可能被 UI 线程（配置加载）与引擎线程（索引/历史库打开）
    // 并发触发，互斥保护；解析本身是幂等的。
    static std::mutex migrationMutex;
    std::lock_guard<std::mutex> lock(migrationMutex);

    const QString name = QString::fromUtf8(fileName);

    QString exeDir;
    if (QCoreApplication::instance()) {
        exeDir = QCoreApplication::applicationDirPath();
    } else {
        // 无应用实例（纯工具场景）：退回工作目录，行为等同旧版
        return std::string(fileName);
    }

    const QString target = exeDir + QLatin1Char('/') + name;
    if (QFileInfo::exists(target)) {
        return target.toStdString();
    }

    // 旧位置（工作目录）存在旧文件 → 一次性搬移；失败则沿用旧文件并告警
    const QString legacy = QDir::current().absoluteFilePath(name);
    if (QFileInfo::exists(legacy) && legacy != target) {
        if (QFile::rename(legacy, target)) {
            qInfo("数据文件 %s 已从工作目录迁移到程序目录", fileName);
        } else {
            qWarning("数据文件 %s 迁移到程序目录失败，继续沿用工作目录文件", fileName);
            return legacy.toStdString();
        }
    }
    return target.toStdString();
}

} // namespace config
