// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

#include "common/guarana_modes.h"
#include "common/settings_enums.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

/// The Guarana page: the settings players change most, in plain words. Every setting stays where it
/// was; this page reads and writes the same values.
class ConfigureGuarana : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureGuarana(bool powered_on, QWidget* parent = nullptr);
    ~ConfigureGuarana() override;

    void ApplyConfiguration();

signals:
    void OpenControlsRequested();
    void ReportProblemRequested();

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    void SelectMode(Common::Guarana::Mode mode);
    void UpdateModeHelp();

    Common::Guarana::Mode shown_mode = Common::Guarana::Mode::Custom;
    Common::Guarana::Mode applied_mode = Common::Guarana::Mode::Custom;
    Settings::ResolutionSetup shown_resolution{};
    // Set by the user and kept until the dialog closes: the Graphics page writes its own copy of
    // these values on every apply, so this page writes again after it each time.
    bool mode_touched = false;
    bool frame_gen_touched = false;
    bool resolution_touched = false;

    QPushButton* mode_buttons[3]{};
    QLabel* mode_heading = nullptr;
    QLabel* mode_help = nullptr;
    QLabel* mode_note = nullptr;
    QCheckBox* frame_gen = nullptr;
    QLabel* frame_gen_heading = nullptr;
    QLabel* frame_gen_help = nullptr;
    QComboBox* resolution = nullptr;
    QLabel* resolution_heading = nullptr;
    QLabel* resolution_help = nullptr;
    QLabel* controller_heading = nullptr;
    QLabel* controller_help = nullptr;
    QPushButton* controller_button = nullptr;
    QLabel* report_heading = nullptr;
    QLabel* report_help = nullptr;
    QPushButton* report_button = nullptr;
};
