// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <QIcon>
#include <QLabel>
#include <QStringList>
#include <fmt/ranges.h>
#include "common/scm_rev.h"
#include "guarana_contributors.h"
#include "ui_aboutdialog.h"
#include "lichii/about_dialog.h"

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent), ui{std::make_unique<Ui::AboutDialog>()} {
    static const std::string build_id = std::string{Common::g_build_id};
    static const std::string lichii_build =
        fmt::format("{} | {} | {}", std::string{Common::g_build_name},
                    std::string{Common::g_build_version}, std::string{Common::g_compiler_id});

    const auto override_build =
        fmt::format(fmt::runtime(std::string(Common::g_title_bar_format_idle)), build_id);
    const auto lichii_build_version = override_build.empty() ? lichii_build : override_build;

    ui->setupUi(this);
    // Try and request the icon from Qt theme (Linux?)
    const QIcon lichii_logo = QIcon::fromTheme(QStringLiteral("io.github.zortos293.lichii"),
                                           QIcon(QStringLiteral(":/img/lichii.ico")));
    if (!lichii_logo.isNull()) {
        ui->labelLogo->setPixmap(lichii_logo.pixmap(112));
    }
    ui->labelLichii->setPixmap(QIcon(QStringLiteral(":/img/lichii_wordmark.png")).pixmap(113, 48));
    ui->labelLichii->setAccessibleName(QStringLiteral("Lichii"));
    ui->verticalLayout_3->setContentsMargins(32, 28, 32, 20);
    ui->horizontalLayout->setSpacing(32);
    ui->verticalLayout->setSpacing(10);
    ui->labelBuildInfo->setText(
        ui->labelBuildInfo->text().arg(QString::fromStdString(lichii_build_version),
                                       QString::fromUtf8(Common::g_build_date).left(10)));

    AddGuaranaContributors();
}

void AboutDialog::AddGuaranaContributors() {
    constexpr int max_shown = 40;
    QStringList names;
    int total = 0;
    for (const char* const* name = GuaranaCredits::kContributors; *name != nullptr; ++name) {
        ++total;
        if (names.size() < max_shown) {
            // Names are plain text: escape each one before it goes into the rich text label.
            names << QString::fromUtf8(*name).toHtmlEscaped();
        }
    }
    if (total == 0) {
        return;
    }
    QString text = QStringLiteral("<p><b>Guarana contributors</b> (%1): %2")
                       .arg(total)
                       .arg(names.join(QStringLiteral(", ")));
    if (total > names.size()) {
        text += tr(" and %1 more").arg(total - names.size());
    }
    text += QStringLiteral(
        "<br/><a style=\"text-decoration:none\" "
        "href=\"https://github.com/karimalsalah/guarana/blob/main/CONTRIBUTORS.md\">"
        "Your name could be here: contribute</a></p>");
    auto* label = new QLabel(text, this);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setObjectName(QStringLiteral("labelGuaranaContributors"));
    ui->verticalLayout->insertWidget(ui->verticalLayout->indexOf(ui->labelLinks) + 1, label);
}

AboutDialog::~AboutDialog() = default;
