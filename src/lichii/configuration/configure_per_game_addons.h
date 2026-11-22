// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include <QFutureWatcher>
#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

#include "core/file_sys/vfs/vfs_types.h"
#include "frontend_common/mod_manager.h"

namespace Core {
class System;
}

class QGraphicsScene;
class QStandardItem;
class QStandardItemModel;
class QTreeView;
class QVBoxLayout;

namespace Ui {
class ConfigurePerGameAddons;
}

class ConfigurePerGameAddons : public QWidget {
    Q_OBJECT

public:
    /// PATCH_KEY is the name the settings store, set when the row shows another text.
    /// PATCH_INFO is what the box under the list says about the row.
    enum PatchData { NUMERIC_VERSION = Qt::UserRole, PATCH_LOCATION, PATCH_KEY, PATCH_INFO };

    explicit ConfigurePerGameAddons(Core::System& system_, QWidget* parent = nullptr);
    ~ConfigurePerGameAddons() override;

    /// Save all button configurations to settings file
    void ApplyConfiguration();

    void LoadFromFile(FileSys::VirtualFile file_);
    void SetFile(FileSys::VirtualFile file_);

    void SetTitleId(u64 id);
    void ReloadAfterModStore(const QSet<QString>& store_toggled);

signals:
    void BrowseModsRequested();

public slots:
    void InstallMods(const QStringList& mods);
    /// `source` is what the player picked, for mod.json: the zip's name. Empty takes the folder's.
    void InstallModPath(const QString& path, const QString& fallbackName = {},
                        const QString& source = {});

    void InstallModFolder();
    void InstallModZip();
    void OpenModsFolder();

    void AddonDeleteRequested(QList<QModelIndex> selected);

protected:
    void showContextMenu(const QPoint& pos);

private:
    static constexpr int LOADED_CHECK_STATE = PATCH_LOCATION + 1;

    static QString AddonKey(const QStandardItem& item);
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void LoadConfiguration();
    void OnItemChanged(QStandardItem* item);
    void UpdateInfo();
    void ScanModConflicts();
    void UpdateModConflicts();

    std::unique_ptr<Ui::ConfigurePerGameAddons> ui;
    FileSys::VirtualFile file;
    u64 title_id{};

    QVBoxLayout* layout;
    QTreeView* tree_view;
    QStandardItemModel* item_model;

    std::vector<QList<QStandardItem*>> list_items;
    std::vector<QStandardItem*> update_items;
    /// The built-in patches that did not apply when the game last started, one line each.
    QString builtin_problems;
    /// What the box under the list says while no row with more to say is selected.
    QString info_hint;

    /// Of the import in progress: the name the player chose, when it is one mod, and what was
    /// picked.
    QString install_name;
    QString install_source;

    /// The game files more than one mod replaces, whatever is ticked: found off this thread.
    QFutureWatcher<std::vector<FrontendCommon::ModFileConflict>> conflict_watcher;
    std::vector<FrontendCommon::ModFileConflict> mod_conflicts;

    Core::System& system;
};
