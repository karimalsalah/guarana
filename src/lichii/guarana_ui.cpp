// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/guarana_ui.h"

#include <QBoxLayout>
#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QList>
#include <QPair>
#include <QPushButton>
#include <QScreen>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include "common/scm_rev.h"
#include "qt_common/config/uisettings.h"

namespace Guarana {

namespace {

constexpr int MaxRememberedMonitors = 6;

QString ScreenKey(const QScreen* screen) {
    if (screen == nullptr) {
        return {};
    }
    const QString raw = screen->name() + QLatin1Char('/') + screen->model() + QLatin1Char('/') +
                        screen->serialNumber();
    return QString::fromLatin1(QUrl::toPercentEncoding(raw));
}

QList<QPair<QString, QString>> ParseStore(const std::string& store) {
    QList<QPair<QString, QString>> entries;
    const QStringList parts =
        QString::fromStdString(store).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const qsizetype bar = part.indexOf(QLatin1Char('|'));
        if (bar > 0 && bar + 1 < part.size()) {
            entries.append({part.left(bar), part.mid(bar + 1)});
        }
    }
    return entries;
}

QString ShortcutText(const char* name, bool controller) {
    for (const auto& shortcut : UISettings::values.shortcuts) {
        if (shortcut.name == name) {
            const std::string& text =
                controller ? shortcut.shortcut.controller_keyseq : shortcut.shortcut.keyseq;
            QString result = QString::fromStdString(text);
            result.replace(QLatin1Char('+'), QStringLiteral(" + "));
            return result;
        }
    }
    return {};
}

} // namespace

QIcon AppIcon() {
    return QIcon(QStringLiteral(":/img/guarana.ico"));
}

bool UseCleanTitle() {
    return !Common::g_is_dev_build;
}

QString CleanWindowTitle(std::string_view game) {
    const QString name = QStringLiteral("Guarana");
    if (game.empty()) {
        return name;
    }
    return name + QStringLiteral(" | ") +
           QString::fromUtf8(game.data(), static_cast<qsizetype>(game.size()));
}

void RememberGeometry(const QWidget* window, std::string& store) {
    const QString key = ScreenKey(window->screen());
    if (key.isEmpty()) {
        return;
    }
    auto entries = ParseStore(store);
    entries.removeIf([&](const auto& entry) { return entry.first == key; });
    entries.prepend({key, QString::fromLatin1(window->saveGeometry().toBase64())});
    while (entries.size() > MaxRememberedMonitors) {
        entries.removeLast();
    }
    QStringList parts;
    for (const auto& entry : entries) {
        parts << entry.first + QLatin1Char('|') + entry.second;
    }
    store = parts.join(QLatin1Char(';')).toStdString();
}

bool RestoreGeometry(QWidget* window, const std::string& store) {
    for (const auto& entry : ParseStore(store)) {
        for (const QScreen* screen : QGuiApplication::screens()) {
            if (ScreenKey(screen) == entry.first) {
                return window->restoreGeometry(QByteArray::fromBase64(entry.second.toLatin1()));
            }
        }
    }
    return false;
}

QString MenuHintText() {
    const QString key = ShortcutText("Exit Fullscreen", false);
    const QString pad = ShortcutText("Fullscreen", true);
    if (!key.isEmpty() && !pad.isEmpty()) {
        return QCoreApplication::translate("Guarana", "%1 leaves fullscreen and shows the menu. On a controller: %2.")
            .arg(key, pad);
    }
    if (!key.isEmpty() || !pad.isEmpty()) {
        return QCoreApplication::translate("Guarana", "%1 leaves fullscreen and shows the menu.")
            .arg(key.isEmpty() ? pad : key);
    }
    return {};
}

QString LauncherSessionHintText() {
    // In a launcher session the leave-fullscreen key stops the game (MainWindow's "Exit
    // Fullscreen" hotkey); the controller has no such combo, so it names the stop combo.
    const QString key = ShortcutText("Exit Fullscreen", false);
    const QString pad = ShortcutText("Stop Emulation", true);
    if (!key.isEmpty() && !pad.isEmpty()) {
        return QCoreApplication::translate("Guarana", "%1 stops the game and returns to the launcher. On a controller: %2.")
            .arg(key, pad);
    }
    if (!key.isEmpty() || !pad.isEmpty()) {
        return QCoreApplication::translate("Guarana", "%1 stops the game and returns to the launcher.")
            .arg(key.isEmpty() ? pad : key);
    }
    return {};
}

Toast::Toast(QWidget* anchor_) : QLabel(nullptr), anchor{anchor_} {
    setObjectName(QStringLiteral("GuaranaToast"));
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                   Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setAlignment(Qt::AlignCenter);
    setStyleSheet(QStringLiteral(
        "QLabel#GuaranaToast { background-color: rgba(11, 16, 29, 235); color: #eef1f6; "
        "border: 1px solid #2a3652; border-radius: 8px; padding: 8px 14px; }"));
    hide_timer.setSingleShot(true);
    connect(&hide_timer, &QTimer::timeout, this, &QWidget::hide);
}

void Toast::ShowMessage(const QString& text, int duration_ms, QWidget* message_anchor) {
    if (text.isEmpty()) {
        return;
    }
    setText(text);
    adjustSize();
    const QWidget* placement = message_anchor != nullptr ? message_anchor : anchor;
    const QRect area = placement->isVisible()
                           ? QRect(placement->mapToGlobal(QPoint(0, 0)), placement->size())
                           : QGuiApplication::primaryScreen()->geometry();
    move(area.center().x() - width() / 2, area.bottom() - height() - area.height() / 12);
    show();
    raise();
    hide_timer.start(duration_ms);
}

EmptyLibraryPanel::EmptyLibraryPanel(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("GuaranaEmptyPanel"));
    setMinimumWidth(480);
    setMaximumWidth(560);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    heading = new QLabel(this);
    heading->setObjectName(QStringLiteral("GuaranaHeading"));
    heading->setWordWrap(true);
    intro = new QLabel(this);
    intro->setObjectName(QStringLiteral("GuaranaHelp"));
    intro->setWordWrap(true);
    keys_text = new QLabel(this);
    keys_text->setObjectName(QStringLiteral("GuaranaHelp"));
    keys_text->setWordWrap(true);
    links = new QLabel(this);
    links->setObjectName(QStringLiteral("GuaranaHelp"));
    links->setWordWrap(true);
    links->setTextFormat(Qt::RichText);
    links->setOpenExternalLinks(true);

    add_folder = new QPushButton(this);
    add_folder->setDefault(true);
    keys_button = new QPushButton(this);
    firmware_button = new QPushButton(this);
    connect(add_folder, &QPushButton::clicked, this, &EmptyLibraryPanel::AddFolder);
    connect(keys_button, &QPushButton::clicked, this, &EmptyLibraryPanel::InstallKeys);
    connect(firmware_button, &QPushButton::clicked, this, &EmptyLibraryPanel::InstallFirmware);

    auto* key_row = new QHBoxLayout;
    key_row->addWidget(keys_button);
    key_row->addWidget(firmware_button);
    key_row->addStretch();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(12);
    layout->addWidget(heading);
    layout->addWidget(intro);
    layout->addWidget(add_folder, 0, Qt::AlignLeft);
    layout->addSpacing(6);
    layout->addWidget(keys_text);
    layout->addLayout(key_row);
    layout->addSpacing(6);
    layout->addWidget(links);

    setFocusProxy(add_folder);
    RetranslateUI();
}

void EmptyLibraryPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }
    QFrame::changeEvent(event);
}

void EmptyLibraryPanel::RetranslateUI() {
    heading->setText(tr("Welcome to Guarana"));
    intro->setText(tr("Add the folder that holds your games and they show up here. Guarana plays "
                      "games you own and dumped yourself."));
    add_folder->setText(tr("Add your games folder"));
    keys_text->setText(tr("Install your own keys and firmware, dumped from a console you own. "
                          "Guarana does not come with them."));
    keys_button->setText(tr("Install keys"));
    firmware_button->setText(tr("Install firmware"));
    links->setText(tr("<a href=\"%1\">Read the guide</a> &nbsp;&middot;&nbsp; <a "
                      "href=\"%2\">Controller setup</a> &nbsp;&middot;&nbsp; <a "
                      "href=\"%3\">Join the Discord</a>")
                       .arg(QString::fromLatin1(GuideUrl), QString::fromLatin1(ControllerUrl),
                            QString::fromLatin1(DiscordUrl)));
}

} // namespace Guarana
