// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <QWidget>
#include "common/common_types.h"
#include "lichii/configuration/configuration_shared.h"

namespace Core {
class System;
}

namespace Ui {
class ConfigureGraphicsAdvanced;
}

namespace ConfigurationShared {
class Builder;
}

class ConfigureGraphicsAdvanced : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    /// `program_id_` is the game whose properties the page belongs to, nullopt on the global page.
    explicit ConfigureGraphicsAdvanced(
        const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
        const ConfigurationShared::Builder& builder, QWidget* parent = nullptr,
        std::optional<u64> program_id_ = std::nullopt);
    ~ConfigureGraphicsAdvanced() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

    void ExposeComputeOption();

private:
    void Setup(const ConfigurationShared::Builder& builder);
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    std::unique_ptr<Ui::ConfigureGraphicsAdvanced> ui;

    const Core::System& system;
    const std::optional<u64> program_id;

    std::vector<std::function<void(bool)>> apply_funcs;

    QWidget* checkbox_enable_compute_pipelines{};
};
