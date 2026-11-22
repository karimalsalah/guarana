// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QTest>
#include <QWidget>

#include "lichii/guarana_ui.h"

class FrameTargetToastTests : public QObject {
    Q_OBJECT

private slots:
    void SeparateWindowAndDefaultAnchor();
};

void FrameTargetToastTests::SeparateWindowAndDefaultAnchor() {
    QWidget main_window;
    main_window.setGeometry(20, 20, 200, 200);
    main_window.show();
    Guarana::Toast toast(&main_window);
    const auto expected_position = [&toast](const QWidget& window) {
        const QRect area(window.mapToGlobal(QPoint(0, 0)), window.size());
        return QPoint(area.center().x() - toast.width() / 2,
                      area.bottom() - toast.height() - area.height() / 12);
    };

    {
        QWidget game_window;
        game_window.setGeometry(250, 180, 300, 300);
        game_window.show();
        QApplication::processEvents();
        toast.ShowMessage(QStringLiteral("Frame target: 40 FPS"), 2500, &game_window);
        QCOMPARE(toast.pos(), expected_position(game_window));
        QVERIFY(toast.pos() != expected_position(main_window));
        QVERIFY(toast.isVisible());
    }

    // A per-message anchor must not replace or leave a dangling pointer in the default anchor.
    toast.ShowMessage(QStringLiteral("Session hint"));
    QCOMPARE(toast.pos(), expected_position(main_window));
    QVERIFY(toast.isVisible());
}

QTEST_MAIN(FrameTargetToastTests)
#include "frame_target_toast.moc"
