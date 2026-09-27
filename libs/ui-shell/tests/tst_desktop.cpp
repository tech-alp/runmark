#include "StatusViewModel.h"
#include "FindingFilterModel.h"
#include <QQuickWindow>
#include <QProcess>
#include <QSignalSpy>
#include <QJsonDocument>
#include <QJsonObject>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QSettings>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

class DesktopTest : public QObject
{
    Q_OBJECT
private slots:
    void projectSetup()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        qputenv("RUNMARK_SMOKE", "1");
        // Setup registers the project; keep it out of the real projects.json.
        qputenv("RUNMARK_CONFIG_HOME", directory.filePath("config").toUtf8());
        const auto resetSmoke = qScopeGuard([] { qunsetenv("RUNMARK_SMOKE"); qunsetenv("RUNMARK_CONFIG_HOME"); });
        const auto wait = qScopeGuard([] { QThreadPool::globalInstance()->waitForDone(); });
        const auto git = [&](const QStringList& args) {
            QProcess process;
            process.setWorkingDirectory(directory.path());
            process.start("git", args);
            return process.waitForFinished(5000) && process.exitCode() == 0;
        };
        QVERIFY(git({"init", "-b", "main"}));
        QVERIFY(git({"remote", "add", "origin", directory.path()}));
        QFile plan(directory.filePath("plan.md"));
        QVERIFY(plan.open(QIODevice::WriteOnly));
        plan.write("- [ ] PROJ-1 First task\n");
        plan.close();
        runmark::StatusViewModel view;
        QSignalSpy requested(&view, &runmark::StatusViewModel::setupRequested);
        QSignalSpy failed(&view, &runmark::StatusViewModel::setupFailed);
        QSignalSpy created(&view, &runmark::StatusViewModel::setupCreated);
        const auto folder = QUrl::fromLocalFile(directory.path());
        const QString path = directory.filePath(".runmark/project.json");
        view.openFolder(folder);
        QCOMPARE(requested.count(), 1);
        QVERIFY(!QFileInfo::exists(path)); // Opening/cancelling never initializes.
        view.createProject(folder, "Test", "origin", "main", "missing.md", "PROJ");
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(!QFileInfo::exists(path));
        view.createProject(folder, "Test", "origin", "main", "plan.md", "PROJ");
        QTRY_COMPARE(created.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!view.busy(), 10000);
        QCOMPARE(view.project(), QString("Test"));
        QFile config(path);
        QVERIFY(config.open(QIODevice::ReadOnly));
        const auto original = config.readAll();
        config.close();
        QCOMPARE(QJsonDocument::fromJson(original).object().value("task_id_pattern").toString(), QString("PROJ-\\d+"));
        view.createProject(folder, "Replacement", "origin", "main", "plan.md", "PROJ");
        QTRY_COMPARE(failed.count(), 2);
        QVERIFY(config.open(QIODevice::ReadOnly));
        QCOMPARE(config.readAll(), original);
        config.close();
        view.openFolder(folder);
        QTRY_VERIFY_WITH_TIMEOUT(!view.busy(), 10000);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(view.project(), QString("Test"));
        QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
        config.write("invalid json");
        config.close();
        view.openFolder(folder);
        QTRY_VERIFY(!view.busy());
        QVERIFY(!view.error().isEmpty());
        QCOMPARE(requested.count(), 1); // Invalid existing configs are not replaced.
    }
    void findingPresentation()
    {
        runmark::FindingModel model;
        model.reset({
            {"context.unresolved_without_ref", "info", "context", "Unresolved note has no reference", "First original note", ""},
            {"context.unresolved_without_ref", "info", "context", "Unresolved note has no reference", "Second original note", ""},
            {"plan.execution_without_plan_link", "info", "plan", "Execution has no plan link", "20260922T123129Z-RM-6 has no matching task in plan", ""},
            {"git.dirty_workspace", "warning", "git", "Workspace has uncommitted changes", "runmark is dirty", ""}});
        QCOMPARE(model.rowCount(), 3);
        runmark::FindingFilterModel filter;
        filter.setSourceModel(&model);
        QCOMPARE(filter.totalCount(), 4);
        QCOMPARE(filter.warningCount(), 1);
        QCOMPARE(filter.infoCount(), 3);
        QCOMPARE(filter.index(0, 0).data(runmark::FindingModel::IdRole).toString(), QString("git.dirty_workspace"));
        filter.setShowInfo(false);
        QCOMPARE(filter.count(), 1);
        filter.setQuery("Second original");
        QCOMPARE(filter.count(), 1);
        filter.setSelectedKey(filter.keyAt(0));
        auto finding = filter.selectedFinding();
        QCOMPARE(finding.value("notes").toStringList().size(), 2);
        QCOMPARE(finding.value("displayTitle").toString(), QString::fromUtf8("2 notta kaynak bağlantısı eksik"));
        filter.setQuery("RM-6");
        QCOMPARE(filter.count(), 1);
        filter.setSelectedKey(filter.keyAt(0));
        finding = filter.selectedFinding();
        QVERIFY(finding.value("displayTitle").toString().startsWith("RM-6:"));
        QVERIFY(finding.value("impact").toString().contains(QString::fromUtf8("bugün planda olmadığı anlamına gelmez")));
        filter.setQuery("");
        QCOMPARE(filter.count(), 1);
        QVERIFY(filter.selectedKey().isEmpty());
        filter.setDomain("context");
        QCOMPARE(filter.count(), 1); // Domain selection explicitly includes info.
        QCOMPARE(filter.totalCount(), 4);
    }
    void filtering()
    {
        runmark::FindingModel model;
        const runmark::FindingModel::Row first{"git.behind", "warning", "git", "First repository", "Repo A", "Fetch"};
        const runmark::FindingModel::Row second{"git.behind", "warning", "git", "Second repository", "Repo B", "Fetch"};
        const runmark::FindingModel::Row plan{"plan.no_tests", "info", "plan", "Tests missing", "Task 12", "Run tests"};
        model.reset({first, second, plan});
        runmark::FindingFilterModel filter;
        filter.setSourceModel(&model);
        QCOMPARE(filter.count(), 3);
        QCOMPARE(filter.domains().size(), 2);
        const auto key = model.index(1, 0).data(runmark::FindingModel::KeyRole).toString();
        filter.setSelectedKey(key);
        QCOMPARE(filter.selectedFinding().value("title").toString(), second.title);
        model.reset({plan, second, first});
        QCOMPARE(filter.selectedKey(), key);
        QCOMPARE(filter.selectedFinding().value("title").toString(), second.title);
        filter.setQuery("REPO B");
        QCOMPARE(filter.count(), 1);
        QCOMPARE(filter.selectedKey(), key);
        filter.setDomain("plan");
        QCOMPARE(filter.count(), 0);
        QVERIFY(filter.selectedKey().isEmpty());
        filter.setQuery("");
        QCOMPARE(filter.count(), 1);
        QCOMPARE(filter.totalCount(), 3);
        filter.setDomain("");
        filter.setSelectedKey(key);
        model.reset({first, plan});
        QVERIFY(filter.selectedFinding().isEmpty());
        QVERIFY(filter.selectedKey().isEmpty());
    }
    void projectWorkflow()
    {
        QCoreApplication::setOrganizationName("RunmarkTests");
        QCoreApplication::setApplicationName("DesktopWorkflow");
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, temporary.path());
        const QString previousDirectory = QDir::currentPath();
        QVERIFY(QDir::setCurrent(temporary.path()));
        const auto restoreDirectory = qScopeGuard([&] { QDir::setCurrent(previousDirectory); });
        const auto waitForWorkers = qScopeGuard([] { QThreadPool::globalInstance()->waitForDone(); });
        const QString config = temporary.filePath("project.json");

        runmark::StatusViewModel view;
        view.restoreProject();
        QVERIFY(view.configPath().isEmpty());
        QVERIFY(view.error().isEmpty());
        QVERIFY(!view.busy());

        view.openProject(QUrl::fromLocalFile(config));
        QVERIFY(view.busy());
        view.setConfigPath("ignored.json");
        QCOMPARE(view.configPath(), config);
        QTRY_VERIFY_WITH_TIMEOUT(!view.busy(), 5000);
        QVERIFY(!view.error().isEmpty());
        QVERIFY(QSettings().value("desktop/lastProject").toString().isEmpty());

        QFile file(config);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"version":1,"name":"desktop-test","worktree_root":"worktrees","repos":[{"name":"missing","path":"missing","base":{"remote":"origin","branch":"main"}}],"plan":{"paths":["plan.md"]},"task_id_pattern":"T-\\d+"})");
        file.close();
        view.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!view.busy(), 5000);
        QCOMPARE(view.error(), QString());
        QCOMPARE(view.project(), QString("desktop-test"));
        QCOMPARE(QSettings().value("desktop/lastProject").toString(), config);

        runmark::StatusViewModel restored;
        restored.restoreProject();
        QCOMPARE(restored.configPath(), config);
        QTRY_VERIFY_WITH_TIMEOUT(!restored.busy(), 5000);
        QCOMPARE(restored.project(), QString("desktop-test"));

        restored.openProject(QUrl::fromLocalFile(temporary.filePath("missing.json")));
        QTRY_VERIFY_WITH_TIMEOUT(!restored.busy(), 5000);
        QVERIFY(!restored.error().isEmpty());
        QVERIFY(restored.project().isEmpty());
        QCOMPARE(restored.findings()->rowCount(), 0);
        QCOMPARE(QSettings().value("desktop/lastProject").toString(), config);

        // Explicit launch path takes precedence over remembered project.
        runmark::StatusViewModel explicitLaunch;
        explicitLaunch.restoreProject(temporary.filePath("explicit.json"));
        QVERIFY(explicitLaunch.configPath().endsWith("explicit.json"));
        QTRY_VERIFY_WITH_TIMEOUT(!explicitLaunch.busy(), 5000);
        QVERIFY(!explicitLaunch.error().isEmpty());

        // Without a saved project, a project in the working directory is usable.
        QSettings().remove("desktop/lastProject");
        QVERIFY(QDir().mkpath(".runmark"));
        QVERIFY(QFile::copy(config, ".runmark/project.json"));
        runmark::StatusViewModel local;
        local.restoreProject();
        QCOMPARE(QFileInfo(local.configPath()).canonicalFilePath(),
            QFileInfo(temporary.filePath(".runmark/project.json")).canonicalFilePath());
        QTRY_VERIFY_WITH_TIMEOUT(!local.busy(), 5000);
        QCOMPARE(local.project(), QString("desktop-test"));
        QSettings().setValue("desktop/lastProject", config);
        runmark::StatusViewModel remembered;
        remembered.restoreProject();
        QCOMPARE(remembered.configPath(), config);
        QTRY_VERIFY_WITH_TIMEOUT(!remembered.busy(), 5000);

        auto* closing = new runmark::StatusViewModel;
        closing->openProject(QUrl::fromLocalFile(config));
        delete closing;
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::processEvents();

        QQmlApplicationEngine engine;
        engine.setInitialProperties({
            {"configPath", temporary.filePath("missing.json")},
            {"themeIndexPath", QStringLiteral(DESKTOP_THEME_INDEX)}});
        engine.loadFromModule("Runmark.Shell", "Shell");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = engine.rootObjects().first();
        auto* error = root->findChild<QQuickItem*>("projectError");
        auto* toolbar = root->findChild<QQuickItem*>("projectToolbar");
        QVERIFY(error);
        QVERIFY(toolbar);
        QTRY_VERIFY_WITH_TIMEOUT(error->isVisible() && error->height() > 0, 5000);
        for (const int width : {1000, 480}) {
            root->setProperty("width", width);
            QTest::qWait(100);
            QVERIFY(error->mapToScene(QPointF()).y()
                    >= toolbar->mapToScene(QPointF(0, toolbar->height())).y());
            QVERIFY(error->mapToScene(QPointF(error->width(), 0)).x() <= width);
        }

        auto* window = qobject_cast<QQuickWindow*>(root);
        auto* status = root->findChild<runmark::StatusViewModel*>();
        auto* filter = root->findChild<runmark::FindingFilterModel*>();
        auto* search = root->findChild<QQuickItem*>("findingsSearch");
        auto* details = root->findChild<QQuickItem*>("findingDetails");
        auto* list = root->findChild<QQuickItem*>("findingsList");
        QVERIFY(window && status && filter && search && details && list);
        QTemporaryDir newProject;
        auto* setup = root->findChild<QObject*>("projectSetupDialog");
        QVERIFY(setup);
        status->openFolder(QUrl::fromLocalFile(newProject.path()));
        QTRY_VERIFY(setup->property("visible").toBool());
        QDir().mkpath(QStringLiteral(DESKTOP_CAPTURE_DIR));
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(QStringLiteral(DESKTOP_CAPTURE_DIR "/project-setup.png")));
        const int previousHeight = window->height();
        window->setHeight(320);
        QTest::qWait(100);
        QVERIFY(setup->property("height").toDouble() <= 288);
        auto* createButton = root->findChild<QQuickItem*>("setupCreate");
        QVERIFY(createButton && createButton->isVisible());
        QVERIFY(createButton->mapToScene(QPointF(0, createButton->height())).y() <= 320);
        window->setHeight(previousHeight);
        QVERIFY(QMetaObject::invokeMethod(setup, "reject"));
        QTRY_VERIFY(!setup->property("visible").toBool());
        QVERIFY(!QFileInfo::exists(newProject.filePath(".runmark/project.json")));
        status->openProject(QUrl::fromLocalFile(config));
        QTRY_VERIFY_WITH_TIMEOUT(!status->busy(), 5000);
        QVERIFY(status->error().isEmpty());
        status->findings()->reset({
            {"plan.done_without_evidence", "warning", "plan", "Done plan task has no evidence", "RM-8", ""},
            {"git.dirty_workspace", "warning", "git", "Workspace has uncommitted changes", "Repo A is dirty", ""},
            {"git.behind", "info", "git", "İkinci çalışma alanı", "Repo B ölçümü.", ""}});
        root->setProperty("width", 1440);
        root->setProperty("height", 900);
        QTest::qWait(150);
        search->setProperty("text", "Repo B");
        QTRY_COMPARE(filter->count(), 1);
        search->setProperty("text", "");
        QTRY_COMPARE(filter->count(), 2);
        filter->setSelectedKey(filter->keyAt(0));
        QTRY_VERIFY(details->isVisible());
        QDir().mkpath(QStringLiteral(DESKTOP_CAPTURE_DIR));
        QTest::qWait(200);
        QVERIFY(window->grabWindow().save(QStringLiteral(DESKTOP_CAPTURE_DIR "/findings-wide.png")));
        QVERIFY(QMetaObject::invokeMethod(toolbar, "themeRequested"));
        QTest::qWait(200);
        QVERIFY(window->grabWindow().save(QStringLiteral(DESKTOP_CAPTURE_DIR "/findings-light.png")));
        QVERIFY(QMetaObject::invokeMethod(toolbar, "themeRequested"));
        root->setProperty("width", 480);
        QTest::qWait(200);
        QVERIFY(!list->isVisible());
        QVERIFY(details->isVisible());
        QVERIFY(details->mapToScene(QPointF(details->width(), 0)).x() <= 480);
        QVERIFY(window->grabWindow().save(QStringLiteral(DESKTOP_CAPTURE_DIR "/findings-narrow.png")));
        QVERIFY(QMetaObject::invokeMethod(details, "backRequested"));
        QTRY_VERIFY(list->isVisible());
        QVERIFY(filter->selectedKey().isEmpty());
        QVERIFY(list->hasActiveFocus());
        list->setProperty("currentIndex", 0);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!filter->selectedKey().isEmpty());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(filter->selectedKey().isEmpty());
        status->findings()->reset({
            {"context.unresolved_without_ref", "info", "context", "Unresolved note has no reference", "CI sonucunun bağlantısı takip notuna eklenecek.", ""},
            {"context.unresolved_without_ref", "info", "context", "Unresolved note has no reference", "Ana dal değişiklikleri çalışma dalıyla karşılaştırılacak.", ""},
            {"context.unresolved_without_ref", "info", "context", "Unresolved note has no reference", "Açık oturum kontrolünün kaynağı belirtilmemiş.", ""},
            {"context.unresolved_without_ref", "info", "context", "Unresolved note has no reference", "Devir kaydına ilişkin kararın kaynağı eklenecek.", ""},
            {"context.unresolved_without_ref", "info", "context", "Unresolved note has no reference", "Önceki test raporunun konumu doğrulanacak.", ""},
            {"git.dirty_workspace", "warning", "git", "Workspace has uncommitted changes", "runmark is dirty", ""},
            {"plan.execution_without_plan_link", "info", "plan", "Execution has no plan link", "20260922T123129Z-RM-6 has no matching task in plan", ""},
            {"plan.execution_without_plan_link", "info", "plan", "Execution has no plan link", "20260922T174528Z-RM-10 has no matching task in plan", ""},
            {"plan.execution_without_plan_link", "info", "plan", "Execution has no plan link", "20260922T182629Z-RM-12 has no matching task in plan", ""}});
        QCOMPARE(filter->totalCount(), 9);
        QCOMPARE(filter->warningCount(), 1);
        QCOMPARE(filter->infoCount(), 8);
        QCOMPARE(filter->count(), 1);
        root->setProperty("width", 1440);
        root->setProperty("height", 900);
        filter->setSelectedKey(filter->keyAt(0));
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(QStringLiteral(DESKTOP_CAPTURE_DIR "/findings-priority.png")));
        filter->setShowInfo(true);
        QCOMPARE(filter->count(), 5);
        filter->setSelectedKey(filter->keyAt(2));
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(QStringLiteral(DESKTOP_CAPTURE_DIR "/findings-expanded.png")));
        filter->setSelectedKey(filter->keyAt(1));
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(QStringLiteral(DESKTOP_CAPTURE_DIR "/findings-notes.png")));

    }
};

QTEST_MAIN(DesktopTest)
#include "tst_desktop.moc"
