#include "application_session.h"
#include "core/sqlite.h"
#include "ui/update_model.h"

#include <QCommandLineParser>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlExtensionPlugin>
#include <QQuickStyle>
#include <QTextStream>
#include <QSaveFile>
#include <QTimer>
#include <QRegularExpression>
#include <QFileInfo>

Q_IMPORT_QML_PLUGIN(IntrovertUiPlugin)

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("Introvert"));
    QCoreApplication::setApplicationName(QStringLiteral("Introvert"));
    QCoreApplication::setApplicationVersion(QStringLiteral(INTROVERT_VERSION));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    application.setWindowIcon(QIcon(QStringLiteral(":/introvert/assets/app_icon.png")));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Launch Introvert"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption preview(QStringLiteral("ui-preview"),QStringLiteral("UI preview with isolated temporary data and credentials"));
#ifdef INTROVERT_PACKAGED
    preview.setFlags(QCommandLineOption::HiddenFromHelp);
#endif
    parser.addOption(preview);
    parser.addOption({QStringLiteral("data-dir"), QStringLiteral("Isolated application data directory"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("local-detector-updates"), QStringLiteral("Local detector update test origin; requires marked isolated --data-dir"), QStringLiteral("url")});
    parser.addOption({QStringLiteral("update-health"), QStringLiteral("Confirm startup to the application updater"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("update-token"), QStringLiteral("Application update transaction token"), QStringLiteral("token")});
    parser.addOption({QStringLiteral("update-result"), QStringLiteral("Application updater result"), QStringLiteral("path")});
    parser.process(application);
    if (parser.isSet("data-dir") && parser.isSet("ui-preview")) {
        QTextStream(stderr) << "--ui-preview uses temporary storage; omit --data-dir\n";
        return 2;
    }
    if (parser.isSet("local-detector-updates") && (!parser.isSet("data-dir") || parser.isSet("ui-preview")
        || parser.value("local-detector-updates").isEmpty())) {
        QTextStream(stderr) << "Local detector updates require a marked isolated --data-dir.\n"; return 2;
    }
#ifdef INTROVERT_PACKAGED
    if(parser.isSet("ui-preview")){QTextStream(stderr)<<"UI preview is available only in development builds.\n";return 2;}
#endif
    auto font = application.font();
    if (parser.isSet("update-health") != parser.isSet("update-token")
        || (parser.isSet("update-token") && (!QRegularExpression("^[a-f0-9]{64}$").match(parser.value("update-token")).hasMatch()
            || parser.isSet("ui-preview")))) return 2;
#ifdef Q_OS_MACOS
    font.setFamily(QStringLiteral("Helvetica Neue"));
#endif
    font.setWeight(QFont::Medium);
    application.setFont(font);

    try {
#ifdef Q_OS_MACOS
        application.setQuitOnLastWindowClosed(false);
#endif
        const auto dataRoot = parser.isSet("data-dir") ? std::optional<QString>(parser.value("data-dir")) : std::nullopt;
        introvert::ApplicationSession session(parser.isSet("ui-preview"), dataRoot,
            parser.isSet("local-detector-updates") ? QUrl(parser.value("local-detector-updates"), QUrl::StrictMode) : QUrl());
        if (!session.load()) return 1;
        if(parser.isSet("update-result") && QFileInfo(parser.value("update-result")).isFile()) {
            if(auto *updates=qobject_cast<introvert::ui::UpdateModel *>(session.root()->property("applicationUpdates").value<QObject *>()))updates->reportFailedInstallation();
        }
        if(parser.isSet("update-health")) {
            QTimer::singleShot(1000,&application,[path=parser.value("update-health"),token=parser.value("update-token")] {
                QSaveFile health(path);
                if(health.open(QIODevice::WriteOnly)) {
                    health.write(token.toUtf8()+"\n"+QCoreApplication::applicationVersion().toUtf8()+"\n");health.commit();
                }
            });
        }
        return application.exec();
    } catch (const std::invalid_argument &error) {
        QTextStream(stderr) << error.what() << '\n';
    } catch (const introvert::DatabaseError &error) {
        QTextStream(stderr) << "Cannot start Introvert: " << error.what() << '\n';
    } catch (...) {
        QTextStream(stderr) << "Cannot start Introvert. Check application-data access and the Qt runtime.\n";
    }
    return 1;
}
