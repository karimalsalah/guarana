// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/configuration/configure_guarana.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "lichii/guarana_ui.h"
#include "qt_common/config/uisettings.h"

namespace {

using Common::Guarana::Mode;

std::filesystem::path UserSwitchFile() {
#ifdef _WIN32
    const auto base = Common::FS::GetAppDataRoamingDirectory();
#else
    const auto base = Common::FS::GetDataDirectory("XDG_CONFIG_HOME");
#endif
    return base.is_absolute() ? base / EDEN_DIR / "nx2_switches.txt" : std::filesystem::path{};
}

bool WriteSwitches(Mode mode) {
    const auto path = UserSwitchFile();
    if (path.empty()) {
        return false;
    }
    std::string text;
    {
        std::ifstream in{path, std::ios::binary};
        text.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    }
    const std::string updated =
        Common::Guarana::ReplaceManagedBlock(text, Common::Guarana::ModeSwitches(mode));
    if (updated == text) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << updated;
    return static_cast<bool>(out);
}

constexpr Settings::ResolutionSetup ResolutionChoices[] = {
    Settings::ResolutionSetup::Res1_2X, Settings::ResolutionSetup::Res3_4X,
    Settings::ResolutionSetup::Res1X,   Settings::ResolutionSetup::Res3_2X,
    Settings::ResolutionSetup::Res2X,   Settings::ResolutionSetup::Res3X,
    Settings::ResolutionSetup::Res4X,
};

QFrame* MakeCard(QWidget* parent, QVBoxLayout*& layout) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("GuaranaCard"));
    layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 14, 18, 16);
    layout->setSpacing(8);
    return card;
}

QLabel* MakeHeading(QWidget* parent) {
    auto* label = new QLabel(parent);
    QFont font = label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.1);
    label->setFont(font);
    return label;
}

QLabel* MakeHelp(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setObjectName(QStringLiteral("GuaranaHelp"));
    label->setWordWrap(true);
    return label;
}

} // namespace

ConfigureGuarana::ConfigureGuarana(bool powered_on, QWidget* parent) : QWidget(parent) {
    setAccessibleName(tr("Guarana"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(12);
    QVBoxLayout* card = nullptr;

    // Mode
    root->addWidget(MakeCard(this, card));
    mode_heading = MakeHeading(this);
    card->addWidget(mode_heading);
    auto* mode_row = new QHBoxLayout;
    for (auto*& button : mode_buttons) {
        button = new QPushButton(this);
        button->setCheckable(true);
        mode_row->addWidget(button);
    }
    mode_row->addStretch();
    card->addLayout(mode_row);
    mode_help = MakeHelp(this);
    card->addWidget(mode_help);
    mode_note = MakeHelp(this);
    card->addWidget(mode_note);
    constexpr Mode modes[] = {Mode::Smooth, Mode::Quality, Mode::Battery};
    for (int i = 0; i < 3; ++i) {
        connect(mode_buttons[i], &QPushButton::clicked, this, [this, i, modes](bool checked) {
            SelectMode(checked ? modes[i] : Mode::Custom);
        });
    }

    // Frame generation
    root->addWidget(MakeCard(this, card));
    frame_gen_heading = MakeHeading(this);
    card->addWidget(frame_gen_heading);
    frame_gen = new QCheckBox(this);
    card->addWidget(frame_gen);
    frame_gen_help = MakeHelp(this);
    card->addWidget(frame_gen_help);

    // Resolution
    root->addWidget(MakeCard(this, card));
    resolution_heading = MakeHeading(this);
    card->addWidget(resolution_heading);
    resolution = new QComboBox(this);
    resolution->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    card->addWidget(resolution, 0, Qt::AlignLeft);
    resolution_help = MakeHelp(this);
    card->addWidget(resolution_help);

    // Controller
    root->addWidget(MakeCard(this, card));
    controller_heading = MakeHeading(this);
    card->addWidget(controller_heading);
    controller_help = MakeHelp(this);
    card->addWidget(controller_help);
    controller_button = new QPushButton(this);
    card->addWidget(controller_button, 0, Qt::AlignLeft);
    connect(controller_button, &QPushButton::clicked, this,
            &ConfigureGuarana::OpenControlsRequested);

    // Report a problem
    root->addWidget(MakeCard(this, card));
    report_heading = MakeHeading(this);
    card->addWidget(report_heading);
    report_help = MakeHelp(this);
    card->addWidget(report_help);
    report_button = new QPushButton(this);
    card->addWidget(report_button, 0, Qt::AlignLeft);
    connect(report_button, &QPushButton::clicked, this, &ConfigureGuarana::ReportProblemRequested);

    root->addStretch();

    applied_mode = shown_mode =
        Common::Guarana::ModeFromString(UISettings::values.guarana_mode.GetValue());
    frame_gen->setChecked(Settings::values.frame_gen.GetValue());
    shown_resolution = Settings::values.resolution_setup.GetValue();
    // The resolution cannot change while a game runs, as on the Graphics page
    resolution->setEnabled(!powered_on);
    connect(frame_gen, &QCheckBox::toggled, this, [this] { frame_gen_touched = true; });
    connect(resolution, &QComboBox::activated, this, [this] { resolution_touched = true; });

    RetranslateUI();
}

ConfigureGuarana::~ConfigureGuarana() = default;

void ConfigureGuarana::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }
    QWidget::changeEvent(event);
}

void ConfigureGuarana::RetranslateUI() {
    setAccessibleName(tr("Guarana"));
    mode_heading->setText(tr("Play mode"));
    mode_buttons[0]->setText(tr("Smooth"));
    mode_buttons[1]->setText(tr("Quality"));
    mode_buttons[2]->setText(tr("Battery"));
    UpdateModeHelp();

    frame_gen_heading->setText(tr("Frame generation"));
    frame_gen->setText(tr("Make extra frames for smoother motion"));
    frame_gen_help->setText(
        tr("Adds frames between the game's own, so motion looks smoother when the game runs "
           "below your screen's refresh rate. It costs some graphics power."));

    resolution_heading->setText(tr("Resolution"));
    resolution_help->setText(
        tr("Lower is faster on integrated graphics. 1x is the game's own sharpness; higher draws "
           "more pixels than the game asks for."));
    const QString labels[] = {tr("0.5x, fastest"), tr("0.75x"), tr("1x, native"), tr("1.5x"),
                              tr("2x"),            tr("3x"),    tr("4x, sharpest")};
    resolution->clear();
    int selected = -1;
    for (int i = 0; i < 7; ++i) {
        resolution->addItem(labels[i], static_cast<int>(ResolutionChoices[i]));
        if (ResolutionChoices[i] == shown_resolution) {
            selected = i;
        }
    }
    if (selected < 0) {
        resolution->addItem(tr("Set in Graphics"), static_cast<int>(shown_resolution));
        selected = resolution->count() - 1;
    }
    resolution->setCurrentIndex(selected);

    controller_heading->setText(tr("Controller"));
    controller_help->setText(
        tr("Xbox, DualSense, Switch Pro and 8BitDo controllers are set up on the Controls page. "
           "By default, holding Home and pressing B on a controller toggles fullscreen."));
    controller_button->setText(tr("Set up controllers"));

    report_heading->setText(tr("Report a problem"));
    report_help->setText(
        tr("Gathers your logs and PC details into one zip, without your user name or folders, and "
           "opens the issue form. Guarana sends nothing by itself."));
    report_button->setText(tr("Report a problem"));
}

void ConfigureGuarana::SelectMode(Mode mode) {
    shown_mode = mode;
    mode_touched = true;
    UpdateModeHelp();
}

void ConfigureGuarana::UpdateModeHelp() {
    mode_buttons[0]->setChecked(shown_mode == Mode::Smooth);
    mode_buttons[1]->setChecked(shown_mode == Mode::Quality);
    mode_buttons[2]->setChecked(shown_mode == Mode::Battery);
    switch (shown_mode) {
    case Mode::Smooth:
        mode_help->setText(tr("Even frames over a high number: every frame lands on your "
                              "display's refresh, extra frames are predicted from the game's "
                              "motion, and frame pacing is set to Auto."));
        break;
    case Mode::Quality:
        mode_help->setText(tr("Nothing holds the graphics back. Best on power, with a higher "
                              "resolution below."));
        break;
    case Mode::Battery:
        mode_help->setText(tr("Keeps the processor from taking power the graphics need and quiets "
                              "background work, for longer play on a laptop or handheld."));
        break;
    case Mode::Custom:
        mode_help->setText(tr("Pick a mode, or leave it to set everything yourself on the other "
                              "pages."));
        break;
    }
    mode_note->setText(shown_mode != applied_mode
                           ? tr("A mode takes effect the next time you start Guarana.")
                           : QString{});
    mode_note->setVisible(!mode_note->text().isEmpty());
}

void ConfigureGuarana::ApplyConfiguration() {
    // Written last, and only when changed here, so the Graphics page's copy of a value cannot undo
    // this page and this page cannot undo a change made there.
    if (mode_touched) {
        if (!WriteSwitches(shown_mode)) {
            QMessageBox::warning(this, tr("Guarana"),
                                 tr("The mode could not be saved: nx2_switches.txt in your "
                                    "Guarana folder is not writable."));
            return;
        }
        UISettings::values.guarana_mode = std::string{Common::Guarana::ModeToString(shown_mode)};
        if (shown_mode == Mode::Smooth) {
            Settings::values.frame_pacing_mode.SetValue(Settings::FramePacingMode::Target_Auto);
        }
        if (shown_mode != applied_mode) {
            LOG_INFO(Frontend, "Guarana mode: {}", Common::Guarana::ModeToString(shown_mode));
        }
        applied_mode = shown_mode;
        UpdateModeHelp();
    }
    if (frame_gen_touched) {
        Settings::values.frame_gen.SetValue(frame_gen->isChecked());
    }
    if (resolution_touched && resolution->isEnabled()) {
        shown_resolution =
            static_cast<Settings::ResolutionSetup>(resolution->currentData().toInt());
        Settings::values.resolution_setup.SetValue(shown_resolution);
    }
}
