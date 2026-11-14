// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QString>
#include <QTimer>

class QPushButton;
class QWidget;

/// Guarana's window and play-mode behaviour, kept apart from the rest of the frontend.
namespace Guarana {

inline constexpr const char* GuideUrl =
    "https://karimalsalah.github.io/guarana/play-ocarina-of-time-switch-2-on-pc.html";
inline constexpr const char* ControllerUrl =
    "https://karimalsalah.github.io/guarana/controller-setup.html";
inline constexpr const char* DiscordUrl = "https://discord.gg/urz7vMdRM";

QIcon AppIcon();

/// Release builds show "Guarana" and the game, without build hashes (About keeps them).
bool UseCleanTitle();
QString CleanWindowTitle(std::string_view game);

/// Remembers a window's geometry per monitor in `store` ("monitor|base64;monitor|base64").
void RememberGeometry(const QWidget* window, std::string& store);
/// Restores the newest entry whose monitor is connected. False when there is none.
bool RestoreGeometry(QWidget* window, const std::string& store);

/// "Esc shows the menu. On a controller: Home + B." from the configured hotkeys.
QString MenuHintText();
/// "Esc stops the game and returns to the launcher. On a controller: L + Plus + Minus." for a
/// launcher session, which has no menu.
QString LauncherSessionHintText();

/// A short message in a small frameless window above the game; never takes focus or input.
class Toast : public QLabel {
    Q_OBJECT

public:
    explicit Toast(QWidget* anchor);
    void ShowMessage(const QString& text, int duration_ms = 4500);

private:
    QWidget* anchor;
    QTimer hide_timer;
};

/// The game list's first-run panel.
class EmptyLibraryPanel : public QFrame {
    Q_OBJECT

public:
    explicit EmptyLibraryPanel(QWidget* parent = nullptr);

signals:
    void AddFolder();
    void InstallKeys();
    void InstallFirmware();

protected:
    void changeEvent(QEvent* event) override;

private:
    void RetranslateUI();

    QLabel* heading = nullptr;
    QLabel* intro = nullptr;
    QLabel* keys_text = nullptr;
    QLabel* links = nullptr;
    QPushButton* add_folder = nullptr;
    QPushButton* keys_button = nullptr;
    QPushButton* firmware_button = nullptr;
};

} // namespace Guarana
