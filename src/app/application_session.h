#pragma once

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <memory>
#include <optional>

class QObject;
class QQmlApplicationEngine;
class QTemporaryDir;
class QNetworkAccessManager;

namespace introvert {
class NavigationModel;
class WindowLifecycle;
namespace ui {
class LogModel;
class NotificationModel;
class PreviewSettings;
class SettingsModel;
class LibraryModel;
class SegmentManagementModel;
class EditorModel;
class SubmissionModel;
class DetectionModel;
class UpdateModel;
}

// The entry point and its acceptance tests use the same composition and lifetime.
// Local update exercises change only transport destinations and use this same UI.
class ApplicationSession final {
public:
    explicit ApplicationSession(bool preview, const std::optional<QString> &dataRoot = {}, const QUrl &localDetectorFeed = {});
    ~ApplicationSession();
    bool load();
    QObject *root() const;
    void close();
private:
    void refreshUpdateNotifications();
    bool m_preview, m_closed = false, m_loaded = false;
    std::unique_ptr<ui::LogModel> m_logs;
    std::unique_ptr<ui::NotificationModel> m_notifications;
    QString m_applicationNotice, m_detectorNotice, m_applicationOfferVersion, m_detectorOfferVersion;
    std::unique_ptr<QTemporaryDir> m_runtime;
    std::optional<QByteArray> m_previousQmlCache;
    std::unique_ptr<NavigationModel> m_navigation;
#ifndef INTROVERT_PACKAGED
    std::unique_ptr<ui::PreviewSettings> m_previewSettings;
#endif
    std::unique_ptr<ui::SettingsModel> m_settings;
    std::unique_ptr<ui::LibraryModel> m_library;
    std::unique_ptr<ui::SegmentManagementModel> m_segments;
    std::unique_ptr<ui::EditorModel> m_editor;
    std::unique_ptr<ui::SubmissionModel> m_submissions;
    std::unique_ptr<ui::DetectionModel> m_detector;
    std::unique_ptr<QNetworkAccessManager> m_updateTestNetwork;
    std::unique_ptr<ui::UpdateModel> m_updates;
    std::unique_ptr<WindowLifecycle> m_lifecycle;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QPointer<QObject> m_root;
};

} // namespace introvert
