#include "application_session.h"
#include "window_lifecycle.h"
#include "core/storage_paths.h"
#include "ui/navigation_model.h"
#ifndef INTROVERT_PACKAGED
#include "ui/preview_settings.h"
#endif
#include "ui/library_model.h"
#include "ui/segment_management_model.h"
#include "ui/editor_model.h"
#include "ui/submission_model.h"
#include "ui/detection_model.h"
#include "ui/update_model.h"
#include "updates/local_detector_feed.h"
#include "notifications/webhook_service.h"
#include <QQmlApplicationEngine>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QDir>

namespace introvert {

ApplicationSession::ApplicationSession(bool preview, const std::optional<QString> &dataRoot, const QUrl &localDetectorFeed) : m_preview(preview)
{
    if (preview && dataRoot) throw std::invalid_argument("--ui-preview uses temporary storage; omit --data-dir");
    if (!localDetectorFeed.isEmpty()) {
        if (preview || !dataRoot) throw std::invalid_argument("Local detector updates require isolated test data.");
        updates::LocalDetectorFeed::validateSession(*dataRoot);
        m_updateTestNetwork = std::make_unique<updates::LocalDetectorFeed>(localDetectorFeed);
    }
    m_runtime = std::make_unique<QTemporaryDir>();
    if (!m_runtime->isValid()) throw std::runtime_error("Cannot create Introvert runtime storage.");
    m_navigation = std::make_unique<NavigationModel>();
    if (preview) {
#ifdef INTROVERT_PACKAGED
        throw std::invalid_argument("UI preview is available only in development builds.");
#else
        m_previewSettings = std::make_unique<ui::PreviewSettings>();
        m_updates = std::make_unique<ui::UpdateModel>(std::nullopt);
#endif
    }
    else {
        const auto paths = dataRoot ? StoragePaths::resolve(*dataRoot) : StoragePaths::resolve();
        paths.prepare();
        SettingsStore settings(paths.database); settings.initialize();
        segments::SegmentStore segments(paths.database); segments.initialize();
        submissions::SubmissionStore submissions(paths.database); submissions.initialize();
        m_settings = std::make_unique<ui::SettingsModel>(settings);
        m_updates = std::make_unique<ui::UpdateModel>(settings, nullptr, m_updateTestNetwork.get());
        m_updates->enableDetectorUpdates();
        if(dataRoot)m_updates->setRestartArguments({"--data-dir",QDir(*dataRoot).absolutePath()});
        m_library = std::make_unique<ui::LibraryModel>(*m_settings, settings, paths.artwork);
        m_segments = std::make_unique<ui::SegmentManagementModel>(*m_library, segments);
        m_editor = std::make_unique<ui::EditorModel>(*m_library, segments);
        m_submissions = std::make_unique<ui::SubmissionModel>(*m_library, settings, submissions, segments);
        m_detector = std::make_unique<ui::DetectionModel>(*m_library, settings, segments, nullptr,
            [webhook = notifications::WebhookService(settings)](const auto &job) { return webhook.deliverCompletion(job); });
    }
    // Framework compilation cache is temporary, separate from canonical app.db
    // and fetched artwork. Preview never resolves or opens application storage.
    if (qEnvironmentVariableIsSet("QML_DISK_CACHE_PATH")) m_previousQmlCache = qgetenv("QML_DISK_CACHE_PATH");
    qputenv("QML_DISK_CACHE_PATH", (m_runtime->path() + "/qml-cache").toUtf8());
}

ApplicationSession::~ApplicationSession() { close(); }

bool ApplicationSession::load()
{
    if (m_closed) return false;
    if (m_loaded) return !m_root.isNull();
    m_loaded = true;
    m_engine = std::make_unique<QQmlApplicationEngine>();
    auto *settings = static_cast<QObject *>(m_settings.get());
#ifndef INTROVERT_PACKAGED
    if(m_preview)settings=m_previewSettings.get();
#endif
    m_engine->setInitialProperties({{"appSettings", QVariant::fromValue(settings)},
        {"appNavigation", QVariant::fromValue<QObject *>(m_navigation.get())}, {"uiPreview", m_preview},
        {"mediaLibrary", QVariant::fromValue<QObject *>(m_library.get())},
        {"segmentManager", QVariant::fromValue<QObject *>(m_segments.get())},
        {"videoEditor", QVariant::fromValue<QObject *>(m_editor.get())},
        {"submissions", QVariant::fromValue<QObject *>(m_submissions.get())},
        {"detector", QVariant::fromValue<QObject *>(m_detector.get())},
        {"applicationUpdates", QVariant::fromValue<QObject *>(m_updates.get())}});
    m_engine->loadFromModule("Introvert.Ui", "Main");
    if (m_engine->rootObjects().isEmpty()) return false;
    m_root = m_engine->rootObjects().first();
    m_lifecycle = std::make_unique<WindowLifecycle>(qobject_cast<QWindow *>(m_root.data()));
    m_updates->setRestartGuard([this] {
        if(m_submissions)for(const auto &job:m_submissions->queue())if(job.toMap().value("status")=="running")return false;
        if(m_detector)for(const auto &job:m_detector->queue())if(job.toMap().value("status")=="running")return false;
        const auto editor = m_root ? m_root->findChild<QObject *>("videoEditorDialog") : nullptr;
        return (!editor || !editor->property("visible").toBool()) && (!m_library || !m_library->busy())
            && (!m_detector || !m_detector->active()) && (!m_submissions || !m_submissions->busy());
    });
    QObject::connect(m_updates.get(), &ui::UpdateModel::restartRequested, QCoreApplication::instance(), &QCoreApplication::quit);
    m_updates->start();
    if (m_library) m_library->load();
    if (m_submissions) m_submissions->start();
    return true;
}

QObject *ApplicationSession::root() const { return m_root.data(); }

void ApplicationSession::close()
{
    if (m_closed) return;
    m_closed = true;
    // Destroy the scene while every referenced model is alive. Stop/join all
    // workers before releasing their dependencies or temporary runtime files.
    m_lifecycle.reset();
    if (m_updates) m_updates->close();
    m_engine.reset(); m_root.clear();
    if (m_submissions) m_submissions->close();
    if (m_detector) m_detector->close();
    if (m_segments) m_segments->close();
    if (m_editor) m_editor->close();
    if (m_library) m_library->close();
    if (m_settings) m_settings->close();
    m_detector.reset(); m_submissions.reset(); m_editor.reset(); m_segments.reset(); m_library.reset(); m_settings.reset();
#ifndef INTROVERT_PACKAGED
    m_previewSettings.reset();
#endif
    m_navigation.reset();
    m_updates.reset();
    m_updateTestNetwork.reset();
    if (m_previousQmlCache) qputenv("QML_DISK_CACHE_PATH", *m_previousQmlCache);
    else qunsetenv("QML_DISK_CACHE_PATH");
    m_runtime.reset();
}

} // namespace introvert
