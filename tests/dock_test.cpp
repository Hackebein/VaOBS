// SPDX-License-Identifier: GPL-2.0-or-later
#include "dock.hpp"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLibrary>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <cstdio>
#include <cstring>

namespace {
QWidget *panel = nullptr;
QByteArray status;
bool retried = false, opened = false, removed = false;
QByteArray next_uuid;
size_t read_status(char *buffer, size_t capacity) {
    const auto required = size_t(status.size() + 1);
    if (buffer && capacity >= required) std::memcpy(buffer, status.constData(), required);
    return required;
}
bool add(const char *id, const char *, void *widget) {
    if (std::strcmp(id, "vaobs-status")) return false;
    panel = static_cast<QWidget *>(widget); return true;
}
void remove(const char *) { delete panel; panel = nullptr; removed = true; }
bool retry(const char *uuid) { retried = std::strcmp(uuid, "test-uuid") == 0; return retried; }
void settings(const char *uuid) { opened = std::strcmp(uuid, "test-uuid") == 0; }
bool next(const char *uuid) { next_uuid = uuid; return true; }
void set_state(const char *state, const char *title, const char *detail, bool can_next = false) {
    const QJsonObject row{{"uuid", "test-uuid"}, {"name", "VaOBS"}, {"state", state}, {"title", title},
        {"detail", detail}, {"stream", "https://example.test/stream"}, {"can_next", can_next}};
    status = QJsonDocument(QJsonObject{{"version", "dev"}, {"build", "test-build"}, {"sources", QJsonArray{row}}}).toJson();
}
bool require(bool condition, const char *message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message); return condition;
}
}
int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    if (argc != 2) return 2;
    QLibrary library(QString::fromLocal8Bit(argv[1]));
    if (!require(library.load(), "Load status dock library")) return 1;
    const auto start = reinterpret_cast<bool (*)(const VaobsDockHost *)>(library.resolve("vaobs_dock_start"));
    const auto stop = reinterpret_cast<void (*)()>(library.resolve("vaobs_dock_stop"));
    const VaobsDockHost host{2, read_status, retry, settings, add, remove, next};
    set_state("checking", "Scanning...", "Checking availability");
    auto incompatible = host; incompatible.abi_version = 1;
    if (!require(start && !start(&incompatible), "Reject an incompatible audio/UI interface")) return 1;
    if (!require(start && stop && start(&host), "Start status dock")) return 1;
    application.processEvents();
    if (!require(panel != nullptr, "Register dock through the frontend callback")) return 1;
    auto *list = panel->findChild<QTreeWidget *>("vaobs_sources");
    auto *next_button = panel->findChild<QPushButton *>("vaobs_next");
    if (!require(next_button && next_button->isHidden() && !next_button->isEnabled(), "Next is hidden with fewer than two available URLs")) return 1;
    if (!require(!panel->findChild<QLabel *>("vaobs_detail"), "No description paragraph below the status")) return 1;
    if (!require(list && list->topLevelItemCount() == 1 && list->topLevelItem(0)->text(1) == "Scanning...",
        "Initial source status")) return 1;
    for (const auto *state : {"Searching...", "Syncing...", "Listening...", "Paused", "Aligned - 1331 ms", "Unavailable"}) {
        set_state("test", state, "Exact failure reason; reload the player in VRChat.");
        QThread::msleep(550); application.processEvents();
        if (!require(list->topLevelItem(0)->text(1) == state && list->topLevelItem(0)->toolTip(1).contains("reload the player"),
            "Short live status with the reason in its tooltip")) return 1;
    }
    panel->findChild<QPushButton *>("vaobs_retry")->click();
    panel->findChild<QPushButton *>("vaobs_settings")->click();
    if (!require(retried && opened, "Actions target the selected source UUID")) return 1;
    set_state("searching", "Searching...", "", true);
    QThread::msleep(550); application.processEvents();
    if (!require(!next_button->isHidden() && next_button->isEnabled(), "Multiple URLs show Next")) return 1;
    next_button->click();
    if (!require(next_uuid == "test-uuid", "Next targets the selected source")) return 1;
    auto root = QJsonDocument::fromJson(status).object();
    auto rows = root["sources"].toArray();
    auto other = rows[0].toObject(); other["uuid"] = "other-uuid"; other["can_next"] = false;
    rows.append(other); root["sources"] = rows; status = QJsonDocument(root).toJson();
    QThread::msleep(550); application.processEvents();
    list->setCurrentItem(list->topLevelItem(1));
    if (!require(next_button->isHidden(), "Selecting a source with one URL hides Next")) return 1;
    other["can_next"] = true; rows[1] = other; root["sources"] = rows; status = QJsonDocument(root).toJson();
    QThread::msleep(550); application.processEvents(); next_button->click();
    if (!require(!next_button->isHidden() && next_uuid == "other-uuid", "Availability refresh preserves selection and targets its UUID")) return 1;
    status = R"({"version":"dev","build":"test-build","sources":[]})";
    QThread::msleep(550); application.processEvents();
    if (!require(list->topLevelItemCount() == 0 && !panel->findChild<QPushButton *>("vaobs_retry")->isEnabled() &&
        next_button->isHidden() && !next_button->isEnabled(), "Removed sources clear the dock")) return 1;
    stop(); application.processEvents();
    if (!require(removed && !panel, "Shutdown removes the dock and stops callbacks")) return 1;
    std::puts("PASS: status dock updates, selected-source actions, empty state, and shutdown");
    return 0;
}
