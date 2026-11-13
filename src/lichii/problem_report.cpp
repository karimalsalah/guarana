// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/problem_report.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPair>
#include <QPlainTextEdit>
#include <QSysInfo>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "common/cpu_features.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/scm_rev.h"
#include "frontend_common/problem_report.h"
#include "qt_common/util/compress.h"

namespace ProblemReport {
namespace {

using namespace FrontendCommon::ProblemReport;

constexpr std::string_view IssueForms =
    "https://github.com/karimalsalah/guarana/issues/new?template=";
// GitHub turns longer links away; everything is in the zip.
constexpr std::size_t MaxLinkLength = 7000;
// Tried when the system will not open the longer one.
constexpr std::size_t ShortLinkLength = 2000;
// What each log keeps of its start and end, so that the zip stays under the 25 MB GitHub takes.
constexpr std::size_t LogHead = 4 * 1024 * 1024;
constexpr std::size_t LogTail = 20 * 1024 * 1024;

struct Kind {
    const char* form;
    const char* title_prefix;
    const char* label;
};

// The forms in .github/ISSUE_TEMPLATE.
constexpr std::array<Kind, 4> Kinds{{
    {"game_bug.yml", "[GBug] ", QT_TRANSLATE_NOOP("QObject", "A game does not play right")},
    {"visual_bug.yml", "[VBug] ", QT_TRANSLATE_NOOP("QObject", "A game does not look right")},
    {"crash.yml", "[Crash] ", QT_TRANSLATE_NOOP("QObject", "Guarana or a game crashed or froze")},
    {"ui_bug.yml", "[UIBug] ", QT_TRANSLATE_NOOP("QObject", "Guarana's windows or menus")},
}};
constexpr int GameBug = 0;
constexpr int Crash = 2;
constexpr int UiBug = 3;

struct LogFile {
    std::string name;
    std::string text;
};

/// The whole log, or its start and end when it is longer than both.
std::string ReadLog(const std::filesystem::path& path) {
    QFile file{QString::fromStdU16String(path.u16string())};
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const qint64 size = file.size();
    if (size <= static_cast<qint64>(LogHead + LogTail)) {
        return file.readAll().toStdString();
    }
    const QByteArray head = file.read(static_cast<qint64>(LogHead));
    if (!file.seek(size - static_cast<qint64>(LogTail))) {
        return head.toStdString();
    }
    const QByteArray tail = file.read(static_cast<qint64>(LogTail));
    return JoinEnds(std::string_view{head.constData(), static_cast<std::size_t>(head.size())},
                    std::string_view{tail.constData(), static_cast<std::size_t>(tail.size())},
                    static_cast<std::size_t>(size) - LogHead - LogTail);
}

QString FromUtf8(const std::string& text) {
    return QString::fromStdString(text);
}

QString BuildText() {
    const QString version = QString::fromUtf8(Common::g_build_version).trimmed();
    const QString revision = QStringLiteral("%1-%2").arg(QString::fromUtf8(Common::g_scm_branch),
                                                         QString::fromUtf8(Common::g_scm_desc));
    return version.isEmpty() ? revision : QStringLiteral("%1 (%2)").arg(version, revision);
}

} // Anonymous namespace

void Run(QWidget* parent, const Context& context) {
    LOG_INFO(Frontend, "Problem report: collecting the logs and the system details");
    // Writes out what is buffered, so that the zip has this session's last lines. Logging goes on.
    Common::Log::Stop();

    const auto log_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    std::vector<LogFile> logs;
    for (const char* name :
         {LOG_FILE, LOG_FILE ".old.txt", "eden_log_2.txt", "eden_log_2.txt.old.txt"}) {
        std::error_code error;
        const auto path = log_dir / name;
        if (std::filesystem::is_regular_file(path, error)) {
            logs.push_back({.name = name, .text = ReadLog(path)});
        }
    }
    const auto find_log = [&logs](std::string_view name) -> std::string_view {
        for (const LogFile& log : logs) {
            if (log.name == name) {
                return log.text;
            }
        }
        return {};
    };
    const std::string_view current = find_log(LOG_FILE);
    const std::string_view previous = find_log(LOG_FILE ".old.txt");

    // The game: this session's, else the last one a log booted.
    QString game;
    QString short_title;
    if (context.session) {
        const auto& session = *context.session;
        short_title = session.title;
        game = QStringLiteral("%1 (%2)")
                   .arg(session.title, QStringLiteral("%1")
                                           .arg(session.program_id, 16, 16, QLatin1Char('0'))
                                           .toUpper());
        if (!session.game_version.isEmpty()) {
            game += QStringLiteral(", version %1").arg(session.game_version);
        }
    } else {
        std::optional<BootedGame> booted = LastBootedGame(current);
        if (!booted) {
            booted = LastBootedGame(previous);
        }
        if (booted) {
            short_title = FromUtf8(booted->title);
            for (const QString& suffix :
                 {QStringLiteral(" (64-bit)"), QStringLiteral(" (32-bit)")}) {
                if (short_title.endsWith(suffix)) {
                    short_title.chop(suffix.size());
                }
            }
            game = QStringLiteral("%1 (%2)")
                       .arg(short_title, QStringLiteral("%1")
                                             .arg(booted->program_id, 16, 16, QLatin1Char('0'))
                                             .toUpper());
            if (!booted->version.empty()) {
                game += QStringLiteral(", version %1").arg(FromUtf8(booted->version));
            }
        }
    }

    // The GPU: what the renderer ran on in this session, else what a log says it ran on.
    QString gpu;
    if (context.session && !context.session->gpu.isEmpty()) {
        const auto& session = *context.session;
        gpu = session.gpu;
        if (!session.driver.isEmpty()) {
            gpu += QStringLiteral(", driver %1").arg(session.driver);
        }
        if (!session.graphics_api.isEmpty()) {
            gpu += QStringLiteral(" (%1)").arg(session.graphics_api);
        }
    } else {
        for (const std::string_view log : {current, previous}) {
            const auto device = LastValue(log, "Report: Device: ");
            if (!device) {
                continue;
            }
            gpu = FromUtf8(*device);
            if (const auto driver = LastValue(log, "Report: Driver: ")) {
                gpu += QStringLiteral(", driver %1").arg(FromUtf8(*driver));
            }
            if (const auto vulkan = LastValue(log, "Report: Vulkan: ")) {
                gpu += QStringLiteral(" (Vulkan %1)").arg(FromUtf8(*vulkan));
            }
            break;
        }
    }
    if (gpu.isEmpty() && !context.vulkan_devices.isEmpty()) {
        gpu = context.vulkan_devices.join(QStringLiteral("; "));
    }

    QString os = QSysInfo::prettyProductName();
    if (const auto logged = LastValue(current, "Init: Host OS: ")) {
        os = FromUtf8(*logged);
    }
    QString cpu;
#ifdef ARCHITECTURE_x86_64
    const auto& cpu_string = Common::g_cpu_caps.cpu_string;
    cpu = QString::fromLatin1(cpu_string, sizeof(cpu_string)).section(QChar{}, 0, 0).trimmed();
#endif
    if (cpu.isEmpty()) {
        cpu = QSysInfo::currentCpuArchitecture();
    }
    cpu += QStringLiteral(", %1 threads").arg(std::thread::hardware_concurrency());
    const QString ram = QStringLiteral("%1 GiB").arg(
        static_cast<double>(Common::GetMemInfo().TotalPhysicalMemory) / (1024.0 * 1024 * 1024),
        0, 'f', 1);
    const bool previous_crashed = previous.find("Host crash:") != std::string_view::npos;
    std::error_code dump_error;
    const bool crash_dump = std::filesystem::exists(log_dir / "eden_crash.dmp", dump_error);

    int kind = UiBug;
    if (previous_crashed && !context.session) {
        kind = Crash;
    } else if (!game.isEmpty()) {
        kind = GameBug;
    }

    QStringList details;
    details << QStringLiteral("Guarana: %1").arg(BuildText());
    details << QStringLiteral("OS: %1").arg(os);
    details << QStringLiteral("CPU: %1").arg(cpu);
    details << QStringLiteral("RAM: %1").arg(ram);
    details << QStringLiteral("GPU: %1").arg(
        gpu.isEmpty() ? QStringLiteral("not known, no game ran with these logs") : gpu);
    if (!game.isEmpty()) {
        details << QStringLiteral("Game: %1").arg(game);
    }
    if (context.session) {
        const auto& session = *context.session;
        if (!session.updates.isEmpty()) {
            details << QStringLiteral("Updates: %1").arg(session.updates);
        }
        if (!session.mods.isEmpty()) {
            details << QStringLiteral("Mods: %1").arg(session.mods);
        }
        if (!session.settings.isEmpty()) {
            details << session.settings;
        }
    }
    if (previous_crashed) {
        details << QStringLiteral("The last session ended in a host crash (the end of %1).")
                       .arg(QString::fromUtf8(LOG_FILE ".old.txt"));
    }

    std::vector<std::string> users;
    for (const char* name : {"USERNAME", "USER", "LOGNAME"}) {
        if (const QString value = qEnvironmentVariable(name); !value.isEmpty()) {
            users.push_back(value.toStdString());
        }
    }
    std::vector<std::string> hosts{QSysInfo::machineHostName().toStdString()};
    if (const QString computer = qEnvironmentVariable("COMPUTERNAME"); !computer.isEmpty()) {
        hosts.push_back(computer.toStdString());
    }
    const auto redactions = MakeRedactions(QDir::homePath().toStdString(), users, hosts);
    const auto scrub = [&redactions](std::string_view text) { return Scrub(text, redactions); };

    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Report a Problem"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(
        QObject::tr("Guarana puts the logs of this session and the last one, with what it knows "
                    "about this PC and the game, in a zip next to the log folder. Your user "
                    "name, home folder and computer name are taken out. Then it opens that "
                    "folder, and a GitHub issue form with a summary filled in, where you attach "
                    "the zip. Guarana sends nothing itself."),
        &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    auto* kinds = new QComboBox(&dialog);
    for (const Kind& entry : Kinds) {
        kinds->addItem(QObject::tr(entry.label));
    }
    kinds->setCurrentIndex(kind);
    form->addRow(QObject::tr("Problem:"), kinds);
    auto* summary = new QLineEdit(&dialog);
    summary->setMaxLength(120);
    summary->setPlaceholderText(QObject::tr("For example: black screen after the title menu"));
    form->addRow(QObject::tr("In a few words:"), summary);
    auto* steps = new QPlainTextEdit(&dialog);
    steps->setPlaceholderText(QObject::tr("What did you do, what happened, and what did you "
                                          "expect?"));
    form->addRow(QObject::tr("Steps:"), steps);
    auto* found =
        new QLabel(FromUtf8(scrub(details.join(QLatin1Char('\n')).toStdString())), &dialog);
    found->setWordWrap(true);
    found->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QObject::tr("Included:"), found);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    buttons->addButton(QObject::tr("Create Report"), QDialogButtonBox::AcceptRole);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(600, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const Kind& chosen = Kinds[static_cast<std::size_t>(kinds->currentIndex())];
    const QString summary_text = summary->text().trimmed();
    const QString steps_text = steps->toPlainText().trimmed();

    const QString zip_name = QStringLiteral("lichii-report-%1.zip")
                                 .arg(QDateTime::currentDateTime().toString(
                                     QStringLiteral("yyyyMMdd-HHmmss")));
    const QString attach =
        QStringLiteral("Attach %1 from the reports folder that Guarana opened.").arg(zip_name);

    QStringList info;
    info << QStringLiteral("Problem: %1").arg(QString::fromUtf8(chosen.label));
    if (!summary_text.isEmpty()) {
        info << QStringLiteral("Summary: %1").arg(summary_text);
    }
    info << details;
    if (crash_dump) {
        info << QStringLiteral("%1 is in the log folder and not in this zip: it can hold "
                               "personal data. Attach it only if a developer asks.")
                    .arg(QStringLiteral("eden_crash.dmp"));
    }
    QStringList included;
    for (const LogFile& log : logs) {
        included << QStringLiteral("%1 (%2 bytes)")
                        .arg(FromUtf8(log.name))
                        .arg(static_cast<qulonglong>(log.text.size()));
    }
    info << QStringLiteral("Logs: %1")
                .arg(included.isEmpty() ? QStringLiteral("none")
                                        : included.join(QStringLiteral(", ")));
    if (!steps_text.isEmpty()) {
        info << QString{} << QStringLiteral("Steps:") << steps_text;
    }

    QList<QPair<QString, QByteArray>> files;
    const std::string system_info = info.join(QLatin1Char('\n')).toStdString() + '\n';
    files.emplaceBack(QStringLiteral("system_info.txt"),
                      QByteArray::fromStdString(scrub(system_info)));
    for (const LogFile& log : logs) {
        files.emplaceBack(FromUtf8(log.name), QByteArray::fromStdString(scrub(log.text)));
    }

    const auto report_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "reports";
    const QString folder = QString::fromStdU16String(report_dir.u16string());
    const QString zip_path = QDir{folder}.filePath(zip_name);
    if (!QtCommon::Compress::compressBuffers(zip_path, files)) {
        LOG_ERROR(Frontend, "Problem report: {} could not be written", zip_name.toStdString());
        QMessageBox::warning(parent, QObject::tr("Report a Problem"),
                             QObject::tr("Guarana could not write %1.")
                                 .arg(QDir::toNativeSeparators(zip_path)));
        return;
    }
    LOG_INFO(Frontend, "Problem report: wrote {} with {} files to the reports folder",
             zip_name.toStdString(), files.size());

    QString title = QString::fromUtf8(chosen.title_prefix);
    if (!short_title.isEmpty()) {
        title += QStringLiteral("%1: %2").arg(short_title, summary_text);
    } else {
        title += summary_text;
    }
    QStringList link_details = details;
    link_details << QString{} << attach;
    const std::string steps_value =
        steps_text.isEmpty() ? std::string{"1. \n2. \n3. "} : steps_text.toStdString();
    const std::string logs_note =
        "(Cut to fit the link: the rest is in system_info.txt in the zip.) " +
        attach.toStdString();
    const std::string form_link = std::string{IssueForms} + chosen.form;
    const std::vector<IssueField> fields{
        {.name = "title", .value = scrub(title.trimmed().toStdString())},
        {.name = "build", .value = scrub(BuildText().toStdString())},
        {.name = "steps",
         .value = scrub(steps_value),
         .shortened_note = "(Cut to fit the link: the whole text is in system_info.txt in the "
                           "zip.)"},
        {.name = "logs",
         .value = scrub(link_details.join(QLatin1Char('\n')).toStdString()),
         .shortened_note = logs_note},
    };
    const auto open_link = [](const std::string& link) {
        return QDesktopServices::openUrl(QUrl::fromEncoded(QByteArray::fromStdString(link)));
    };

    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    std::string link = IssueUrl(form_link, fields, MaxLinkLength);
    bool opened = open_link(link);
    if (!opened && link.size() > ShortLinkLength) {
        // Some Windows shells refuse links over about 2000 characters.
        link = IssueUrl(form_link, fields, ShortLinkLength);
        opened = open_link(link);
    }
    if (!opened) {
        QGuiApplication::clipboard()->setText(QString::fromStdString(link));
        QMessageBox::information(parent, QObject::tr("Report a Problem"),
                                 QObject::tr("Guarana could not open the browser. The link to the "
                                             "filled-in issue form is on the clipboard: paste it "
                                             "into a browser, then attach %1 from the folder "
                                             "that opened.")
                                     .arg(zip_name));
    }
}

} // namespace ProblemReport
