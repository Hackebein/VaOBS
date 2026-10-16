// SPDX-License-Identifier: GPL-2.0-or-later
#include "dock.hpp"
#include <QApplication>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
VaobsDockHost host{};
QPointer<QWidget> panel;
QPointer<QTimer> timer;
bool enabled = false;

void create_dock()
{
    if (!enabled || panel) return;
    auto *widget = new QWidget;
    auto *layout = new QVBoxLayout(widget);
    auto *list = new QTreeWidget(widget);
    list->setObjectName("vaobs_sources");
    list->setColumnCount(2); list->setHeaderLabels({"Source", "Status"});
    list->setRootIsDecorated(false);
    list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    list->header()->setStretchLastSection(true);
    auto *buttons = new QHBoxLayout;
    auto *next = new QPushButton("Next stream", widget);
    next->setObjectName("vaobs_next"); next->setVisible(false); next->setEnabled(false);
    auto *retry = new QPushButton("Retry streams", widget);
    retry->setObjectName("vaobs_retry"); retry->setEnabled(false);
    auto *settings = new QPushButton("Properties", widget);
    settings->setObjectName("vaobs_settings"); settings->setEnabled(false);
    buttons->addWidget(next); buttons->addWidget(retry); buttons->addWidget(settings);
    auto *version = new QLabel(widget); version->setObjectName("vaobs_version");
    version->setTextInteractionFlags(Qt::TextSelectableByMouse); version->setWordWrap(true);
    layout->addWidget(list); layout->addLayout(buttons); layout->addWidget(version);
    auto selected_uuid = [list] {
        return list->currentItem() ? list->currentItem()->data(0, Qt::UserRole).toString().toUtf8() : QByteArray{};
    };
    auto update_buttons = [=] {
        const auto *item = list->currentItem();
        retry->setEnabled(item != nullptr); settings->setEnabled(item != nullptr);
        const bool can_next = item && item->data(0, Qt::UserRole + 1).toBool();
        next->setVisible(can_next); next->setEnabled(can_next);
    };
    QObject::connect(list, &QTreeWidget::currentItemChanged, widget, update_buttons);
    QObject::connect(next, &QPushButton::clicked, widget, [=] {
        const auto uuid = selected_uuid();
        if (host.next && !uuid.isEmpty() && !host.next(uuid.constData())) {
            next->setVisible(false); next->setEnabled(false);
        }
    });
    QObject::connect(retry, &QPushButton::clicked, widget, [=] {
        const auto uuid = selected_uuid();
        if (host.retry && !uuid.isEmpty() && !host.retry(uuid.constData())) {
            list->currentItem()->setText(1, "Unavailable");
            list->currentItem()->setToolTip(1, "This source is no longer available.");
        }
    });
    QObject::connect(settings, &QPushButton::clicked, widget, [=] {
        const auto uuid = selected_uuid(); if (host.settings && !uuid.isEmpty()) host.settings(uuid.constData());
    });
    auto refresh = [=, previous_data = QByteArray{}]() mutable {
        if (!host.read_status) return;
        QByteArray data;
        // Source creation/removal can change the size between calls.
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto needed = host.read_status(nullptr, 0);
            if (!needed || needed > 1024 * 1024) return;
            data.resize(qsizetype(needed));
            if (host.read_status(data.data(), size_t(data.size())) <= size_t(data.size())) break;
            data.clear();
        }
        if (data.isEmpty()) return;
        if (data == previous_data) return;
        const auto document = QJsonDocument::fromJson(data.constData());
        if (!document.isObject()) return;
        previous_data = data;
        const auto root = document.object();
        const auto previous = selected_uuid();
        const QSignalBlocker blocked(list);
        list->clear();
        QTreeWidgetItem *selected = nullptr;
        for (const auto &entry : root["sources"].toArray()) {
            const auto row = entry.toObject();
            auto *item = new QTreeWidgetItem(list, {row["name"].toString(), row["title"].toString()});
            item->setData(0, Qt::UserRole, row["uuid"].toString());
            item->setData(0, Qt::UserRole + 1, row["can_next"].toBool());
            QString text = row["detail"].toString();
            if (!row["stream"].toString().isEmpty()) text += "\nStream: " + row["stream"].toString();
            item->setToolTip(0, text); item->setToolTip(1, text);
            if (row["uuid"].toString().toUtf8() == previous) selected = item;
        }
        if (!selected && list->topLevelItemCount()) selected = list->topLevelItem(0);
        list->setCurrentItem(selected);
        update_buttons();
        version->setText("VaOBS " + root["version"].toString() + " (" + root["build"].toString() + ")");
    };
    auto *clock = new QTimer(widget);
    QObject::connect(clock, &QTimer::timeout, widget, refresh);
    if (!host.add_dock("vaobs-status", "VaOBS", widget)) { delete widget; return; }
    panel = widget; timer = clock; clock->start(500); refresh();
}

void stop_dock()
{
    enabled = false;
    if (timer) timer->stop();
    if (panel && host.remove_dock) host.remove_dock("vaobs-status");
    host = {}; panel = nullptr; timer = nullptr;
}
}

extern "C" __declspec(dllexport) bool vaobs_dock_start(const VaobsDockHost *callbacks)
{
    if (!qobject_cast<QApplication *>(QCoreApplication::instance()) || !callbacks || callbacks->abi_version != 2 || !callbacks->read_status || !callbacks->next ||
        !callbacks->add_dock || !callbacks->remove_dock) return false;
    host = *callbacks; enabled = true;
    QMetaObject::invokeMethod(qApp, create_dock, Qt::QueuedConnection);
    return true;
}

extern "C" __declspec(dllexport) void vaobs_dock_stop()
{
    if (!QCoreApplication::instance()) return;
    if (QThread::currentThread() == qApp->thread()) stop_dock();
    else QMetaObject::invokeMethod(qApp, stop_dock, Qt::BlockingQueuedConnection);
}
