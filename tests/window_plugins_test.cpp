// SPDX-License-Identifier: GPL-3.0-only

// The window test: plugins: their panels, actions and triggers.

#include "window_test.hpp"

#include "uicommon/library_tree_view.hpp"
#include "workspace/artwork_session.hpp"
#include "workspace/sources.hpp"

#include <QGroupBox>
#include <QMessageBox>
#include <QPainter>
#include <QStandardItemModel>
#include <sqlite3.h>

namespace trackknife::bench {

class BenchMainWindowTest::Plugins final : public window_test::Fixture {
    Q_OBJECT

  private slots:
    void aPluginActionSharesWhatIsChosen();
    void manyPluginActionsGoInASubmenu();
    void pluginTriggersRunOnTheLibrarysNews();
    void aPluginPanelShowsWhatItSaysAboutThePlayingTrack();
    void aPluginPanelIsToldTheLibrary();
    void aTogetherLinkAsksTheEngineToJoin();
    void aRoomPanelIsAskedAgainAsTheRoomChanges();
    void aPanelsControlsAskItAgain();
    void aPanelIsAskedAgainWhenItSays();
    void theListeningRoomPanelIsTheRoom();
    void lyricsProvidersAreAskedWhenTheFileHasNone();
    void aPluginPanelIsToldWhatAnotherClientStarted();
};

// ADR-0288: an enabled plugin's action in a tab's and in the library's
// context menu, handed what is chosen; its answer notified and copied.
void BenchMainWindowTest::Plugins::aPluginActionSharesWhatIsChosen() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto album_folder = media.filePath(QStringLiteral("music/Band/Record"));
    QVERIFY(QDir{}.mkpath(album_folder));
    const auto file = album_folder + QStringLiteral("/01.flac");
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), file));
    const auto raw = [](const QString& path) {
        const auto encoded = QFile::encodeName(path);
        return std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    };
    // A plugin answering with the album's folder within the music folder,
    // else with its name.
    const auto folder = pluginFolders().value(0) + QStringLiteral("/echo");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Echo", "actions": [{"id": "share",
                          "label": "Share album", "run": ["./share"],
                          "result": "notify-and-copy"},
                          {"id": "artist", "label": "Artist", "for": "tracks",
                           "run": ["./share", "artist"], "result": "copy"},
                          {"id": "list", "label": "List", "for": "tracks",
                           "run": ["./share", "list"], "result": "list"}],
                          "settings": [{"key": "server", "label": "Server",
                                        "default": "http://localhost"},
                                       {"key": "remote", "label": "Remote support",
                                        "kind": "toggle", "link": "https://example.org/remote"},
                                       {"key": "token", "label": "Token", "kind": "secret",
                                        "requires": "remote"}]})");
        QFile script{folder + QStringLiteral("/share")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"sh(#!/bin/sh
input=$(cat)
artist=$(printf '%s' "$input" | grep -o '"artist":"[^"]*"' | head -n 1 | cut -d '"' -f 4)
[ "$1" = artist ] && { echo "artist:$artist"; exit 0; }
[ "$1" = list ] && { printf '{"title": "%s: 2 missing", "items": [{"text": "First", "detail": "1987", "url": "https://example.org/1"}, {"text": "Second"}]}' "$artist"; exit 0; }
relative=$(printf '%s' "$input" | sed -n 's/.*"relative_folder":"\([^"]*\)".*/\1/p')
album=$(printf '%s' "$input" | sed -n 's/.*"album":"\([^"]*\)".*/\1/p')
echo "shared:${relative:-$album}"
)sh");
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("echo")), true);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QVERIFY(window.notifier_ != nullptr);
    QStringList notified;
    window.notifier_->setSendOverride([&notified](const QString& summary, const QString& body) {
        notified << summary + ": " + body;
    });
    window.openLocalPaths({raw(file)});
    Workspace::ListTab* tab = nullptr;
    QTRY_VERIFY((tab = [&window]() -> Workspace::ListTab* {
                    for (auto& candidate : window.list_tabs_) {
                        if (candidate->model->rowCount() == 1) {
                            return candidate.get();
                        }
                    }
                    return nullptr;
                }()) != nullptr);
    window.tabs_->setCurrentWidget(tab->view);
    QTRY_VERIFY_WITH_TIMEOUT(tab->model->rows().front().probed, 5'000);
    const auto album = QString::fromStdString(tab->model->rows().front().album);
    QVERIFY(!album.isEmpty());
    tab->view->selectionModel()->select(
        tab->model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(QMetaObject::invokeMethod(
        tab->view, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, tab->view->visualRect(tab->model->index(0, local_title_column)).center())));
    auto* share = window.findChild<QAction*>(QStringLiteral("action-plugin-echo-share"));
    QVERIFY(share != nullptr);
    QCOMPARE(share->text(), QStringLiteral("Share album"));
    window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"))->close();
    QGuiApplication::clipboard()->clear();
    share->trigger();
    // Not in a music folder of this computer's: its album's name.
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("shared:") + album);
    QTRY_VERIFY(notified.contains(QStringLiteral("Echo: shared:") + album));

    // The library's: the album's folder within the music folder, the
    // folder added as Settings adds it.
    auto* panel = window.localLibrary();
    QVERIFY(panel != nullptr);
    panel->browser().addRoot(raw(media.filePath(QStringLiteral("music"))));
    QTRY_VERIFY(!panel->browser().rootPaths().empty());
    {
        persistence::LibraryScanProgress progress;
        QVERIFY(window.localCatalogue()->open()->scan({}, progress).has_value());
    }
    panel->browser().reload();
    auto* tree = panel->findChild<QTreeView*>();
    QTRY_VERIFY(tree->model()->rowCount() > 0);
    const auto first = tree->model()->index(0, 0);
    tree->scrollTo(first);
    QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, tree->visualRect(first).center()));
    auto* menu = panel->findChild<QMenu*>(QStringLiteral("local-library-context-menu"));
    QVERIFY(menu != nullptr);
    auto* from_library = menu->findChild<QAction*>(QStringLiteral("action-plugin-echo-share"));
    QVERIFY(from_library != nullptr);
    menu->close();
    QGuiApplication::clipboard()->clear();
    from_library->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(),
                              QStringLiteral("shared:Band/Record"), 10'000);

    // From the artist's row too: the artist is said, as from an album.
    const auto artist = QString::fromStdString(tab->model->rows().front().album_artist.empty()
                                                   ? tab->model->rows().front().artist
                                                   : tab->model->rows().front().album_artist);
    QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, tree->visualRect(first).center()));
    menu = panel->findChild<QMenu*>(QStringLiteral("local-library-context-menu"));
    QVERIFY(menu != nullptr);
    auto* of_artist = menu->findChild<QAction*>(QStringLiteral("action-plugin-echo-artist"));
    QVERIFY(of_artist != nullptr);
    menu->close();
    QGuiApplication::clipboard()->clear();
    of_artist->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(),
                              QStringLiteral("artist:") + artist, 10'000);

    // A list answered: a window of its own, its rows drawn by Melody. The
    // menu again: one closed goes, and its entries with it.
    QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, tree->visualRect(first).center()));
    menu = panel->findChild<QMenu*>(QStringLiteral("local-library-context-menu"));
    QVERIFY(menu != nullptr);
    auto* as_list = menu->findChild<QAction*>(QStringLiteral("action-plugin-echo-list"));
    QVERIFY(as_list != nullptr);
    menu->close();
    as_list->trigger();
    QDialog* list_window = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (list_window = window.findChild<QDialog*>(QStringLiteral("bench-plugin-list"))) != nullptr,
        10'000);
    QCOMPARE(list_window->windowTitle(), QStringLiteral("Echo"));
    QCOMPARE(list_window->findChild<QLabel*>(QStringLiteral("bench-plugin-list-title"))->text(),
             artist + QStringLiteral(": 2 missing"));
    auto* rows = list_window->findChild<QListWidget*>(QStringLiteral("bench-plugin-list-rows"));
    QVERIFY(rows != nullptr);
    QCOMPARE(rows->count(), 2);
    QCOMPARE(rows->item(0)->text(), QStringLiteral("First"));
    QCOMPARE(rows->item(0)->data(Qt::UserRole + 1).toString(), QStringLiteral("1987"));
    QCOMPARE(rows->item(0)->toolTip(), QStringLiteral("https://example.org/1"));
    list_window->findChild<QPushButton*>(QStringLiteral("bench-plugin-list-copy"))->click();
    QCOMPARE(QGuiApplication::clipboard()->text(),
             artist + QStringLiteral(": 2 missing\nFirst -- 1987\nSecond"));
    list_window->close();

    // Settings has a page of plugins, each enabled there; turned off and
    // saved, its action is gone.
    {
        auto* dialog = window.showSettingsDialog(SettingsDialog::Page::plugins);
        QVERIFY(dialog != nullptr);
        auto* pages = dialog->findChild<QListWidget*>();
        QVERIFY(pages != nullptr);
        QCOMPARE(pages->currentItem()->text(), QStringLiteral("Plugins"));
        auto* enabled = dialog->findChild<QCheckBox*>(QStringLiteral("bench-settings-plugin-echo"));
        QVERIFY(enabled != nullptr && enabled->isChecked());
        QCOMPARE(enabled->text(), QStringLiteral("Echo"));
        // ADR-0300: what it asks to be set, behind its Settings button in a
        // dialog of its own; a secret not shown.
        QVERIFY(dialog->findChild<QLineEdit*>(
                    QStringLiteral("bench-settings-plugin-echo-server")) == nullptr);
        const auto edit = [dialog](const bool keep) {
            dialog->findChild<QPushButton*>(QStringLiteral("bench-settings-plugin-echo-settings"))
                ->click();
            QDialog* settings = nullptr;
            QTRY_VERIFY((settings = dialog->findChild<QDialog*>(
                             QStringLiteral("bench-plugin-settings-echo"))) != nullptr &&
                        settings->isVisible());
            auto* server = settings->findChild<QLineEdit*>(
                QStringLiteral("bench-settings-plugin-echo-server"));
            auto* token =
                settings->findChild<QLineEdit*>(QStringLiteral("bench-settings-plugin-echo-token"));
            QVERIFY(server != nullptr && token != nullptr);
            QCOMPARE(server->text(), QStringLiteral("http://localhost"));
            QCOMPARE(token->echoMode(), QLineEdit::Password);
            // The token needs Remote support: greyed until it is ticked.
            auto* remote = settings->findChild<QCheckBox*>(
                QStringLiteral("bench-settings-plugin-echo-remote"));
            QVERIFY(remote != nullptr);
            QCOMPARE(remote->text(), QStringLiteral("Remote support"));
            QVERIFY(!remote->isChecked() && !token->isEnabled());
            QVERIFY(settings
                        ->findChild<QLabel*>(
                            QStringLiteral("bench-settings-plugin-echo-remote-caption"))
                        ->text()
                        .contains(QStringLiteral("href=\"https://example.org/remote\"")));
            remote->setChecked(true);
            QVERIFY(token->isEnabled());
            // For a look at them: the dialog, and the page it is opened from.
            if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
                keep && !directory.isEmpty()) {
                QVERIFY(settings->grab().save(directory + QStringLiteral("/plugin-settings.png")));
                QVERIFY(dialog->grab().save(directory + QStringLiteral("/plugins-page.png")));
            }
            server->selectAll();
            QTest::keyClicks(server, QStringLiteral("http://nas:6600"));
            QTest::keyClicks(token, QStringLiteral("s3cret"));
            QPointer<QDialog> open = settings;
            settings->findChild<QDialogButtonBox*>(QStringLiteral("bench-plugin-settings-buttons"))
                ->button(keep ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)
                ->click();
            QTRY_VERIFY(open.isNull());
        };
        // Cancelled, nothing it set is kept: the dialog opens as it was.
        edit(false);
        if (QTest::currentTestFailed()) {
            return;
        }
        edit(true);
        if (QTest::currentTestFailed()) {
            return;
        }
        enabled->setChecked(false);
        QPointer<SettingsDialog> lifetime = dialog;
        dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-settings-buttons"))
            ->button(QDialogButtonBox::Save)
            ->click();
        QTRY_VERIFY(lifetime.isNull());
    }
    QVERIFY(!pluginEnabled(QStringLiteral("echo")));
    QVERIFY(enabledPlugins().empty());
    // Saved: the address with the settings, the secret apart from them.
    const auto echo = discoverPlugins();
    const auto found = std::ranges::find(echo, QStringLiteral("echo"), &FoundPlugin::name);
    QVERIFY(found != echo.end() && found->plugin);
    const auto told = pluginSettings(*found->plugin);
    QCOMPARE(told.value(QStringLiteral("server")).toString(), QStringLiteral("http://nas:6600"));
    QCOMPARE(told.value(QStringLiteral("token")).toString(), QStringLiteral("s3cret"));
    QCOMPARE(told.value(QStringLiteral("remote")).toBool(), true);
    QSettings{}.sync();
    {
        QFile settings_file{QSettings{}.fileName()};
        QVERIFY(settings_file.open(QIODevice::ReadOnly));
        QVERIFY(!settings_file.readAll().contains("s3cret"));
    }
    QSettings{}.remove(QStringLiteral("plugins"));
    QFile::remove(pluginSecretsFile());
    QDir{folder}.removeRecursively();
}

// ADR-0302, ADR-0303: the official Listen together panel, drawn -- out of
// a room, a room's name and password to start one; started, the room's
// queue with its votes -- against a stand-in for melody-cli.
void BenchMainWindowTest::Plugins::theListeningRoomPanelIsTheRoom() {
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    const auto in_room = bin.filePath(QStringLiteral("in-room"));
    {
        QFile cli{bin.filePath(QStringLiteral("melody-cli"))};
        QVERIFY(cli.open(QIODevice::WriteOnly));
        cli.write(QStringLiteral(R"sh(#!/bin/sh
case "$3" in
  host|join) touch '%1'; echo '{}' ;;
  leave) rm -f '%1' ;;
  status)
    if [ -e '%1' ]; then
      echo '{"role":"host","room":"friday-doom","invite":"https://t.example/rooms/friday-doom#letmein","listeners":["Ana","Bo","Cy"],"track":"Royal Thunder — Time Machine","message":"","party":true,"suggestions":[{"id":7,"name":"Ana","title":"Glow","artist":"Royal Thunder","votes":3,"voted":true,"ready":true},{"id":8,"name":"Cy","title":"Wake Up","artist":"Royal Thunder","votes":1,"voted":false,"ready":false}],"by":"Ana","next":"Royal Thunder — Forget You","history":["Royal Thunder — One Day","Royal Thunder — Floor"],"upcoming":[]}'
    else
      echo '{"role":"","message":""}'
    fi ;;
esac
)sh")
                      .arg(in_room)
                      .toUtf8());
        cli.close();
        cli.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    const auto path = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(bin.path()) + ":" + path);
    // The official plugin, as installed.
    const auto folder = pluginFolders().value(0) + QStringLiteral("/together");
    QVERIFY(QDir{}.mkpath(folder));
    for (const auto* name : {"plugin.json", "together"}) {
        QFile::remove(folder + QLatin1Char('/') + QLatin1String(name));
        QVERIFY(QFile::copy(QStringLiteral(MELODY_AUDIO_FIXTURE_DIR "/../../../plugins/together/") +
                                QLatin1String(name),
                            folder + QLatin1Char('/') + QLatin1String(name)));
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("together")), true);
    QSettings{}.setValue(pluginSettingKey(QStringLiteral("together"), QStringLiteral("server")),
                         QStringLiteral("https://t.example"));

    BenchMainWindow window;
    window.resize(1400, 900);
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-together-room"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());
    QTest::qWait(500);
    window.transport_timer_->stop();
    window.refreshPluginPanels(EnginePlayback::State{});
    QLineEdit* room = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((room = dock->findChild<QLineEdit*>(
                                  QStringLiteral("bench-plugin-panel-input-room"))) != nullptr,
                             10'000);
    const auto shot = [&dock](const QString& name) {
        if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
            !directory.isEmpty()) {
            QVERIFY(dock->grab().save(directory + QLatin1Char('/') + name));
        }
    };
    shot(QStringLiteral("room-panel-out.png"));
    QTest::keyClicks(room, QStringLiteral("friday-doom"));
    for (auto* button :
         dock->findChildren<QPushButton*>(QStringLiteral("bench-plugin-panel-button"))) {
        if (button->property("action").toString() == QStringLiteral("start")) {
            button->click();
        }
    }
    QTRY_VERIFY_WITH_TIMEOUT(dock->findChild<QAbstractButton*>(
                                 QStringLiteral("bench-plugin-panel-toggle-party")) != nullptr,
                             10'000);
    QVERIFY(dock->findChild<QAbstractButton*>(QStringLiteral("bench-plugin-panel-toggle-party"))
                ->isChecked());
    // Two votes, two removes; the room's web page and Stop.
    QCOMPARE(
        dock->findChildren<QToolButton*>(QStringLiteral("bench-plugin-panel-item-button")).size(),
        6);
    // ADR-0304: drawn as Melody's -- what plays, the next, two suggestions,
    // two played; the status; the people.
    const auto tracks = dock->findChildren<QWidget*>(QStringLiteral("bench-plugin-panel-track"));
    QCOMPARE(tracks.size(), 6);
    QVERIFY(tracks.front()->property("playing").toBool());
    QCOMPARE(tracks.front()
                 ->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-track-title"))
                 ->text(),
             QStringLiteral("Time Machine"));
    QVERIFY(dock->findChild<QWidget*>(QStringLiteral("bench-plugin-panel-status")) != nullptr);
    QCOMPARE(dock->findChildren<QWidget*>(QStringLiteral("bench-plugin-panel-person")).size(), 3);
    // The host's remove, quiet: shown only under the pointer -- on a row;
    // the status line's buttons always.
    for (auto* track : tracks) {
        for (auto* button :
             track->findChildren<QToolButton*>(QStringLiteral("bench-plugin-panel-item-button"))) {
            QCOMPARE(button->isVisibleTo(dock), button->property("role").toString().isEmpty());
        }
    }
    auto* status = dock->findChild<QWidget*>(QStringLiteral("bench-plugin-panel-status"));
    for (auto* button :
         status->findChildren<QToolButton*>(QStringLiteral("bench-plugin-panel-item-button"))) {
        QVERIFY(button->isVisibleTo(dock));
    }
    // Asked with nothing playing too, it does not name a track under the
    // room's name.
    QVERIFY(dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-asked")) == nullptr);
    QTest::qWait(300);
    shot(QStringLiteral("room-panel-in.png"));
    qputenv("PATH", path);
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// ADR-0303: a panel of controls, asked with nothing playing; a button
// pressed runs it again, told what was pressed and what was typed -- which
// stays typed when it answers.
void BenchMainWindowTest::Plugins::aPanelsControlsAskItAgain() {
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const auto folder = pluginFolders().value(0) + QStringLiteral("/controls");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Controls", "panels": [{"id": "p",
                          "label": "Controls", "run": ["./p"], "always": true}]})");
        QFile script{folder + QStringLiteral("/p")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral(R"sh(#!/bin/sh
input=$(cat)
printf '%s\n' "$input" >> '%1'
did=$(printf '%s' "$input" | grep -o '"action":{"id":"[^"]*","value":"[^"]*"}' | sed 's/.*"id":"\([^"]*\)","value":"\([^"]*\)".*/\1:\2/')
printf '{"title": "did %s", "parts": [{"input": "room", "label": "Room"},
  {"button": "Start", "action": "start", "primary": true},
  {"text": "Glow", "buttons": [{"text": "vote", "action": "vote", "value": "7"}]}]}' "$did"
)sh")
                         .arg(out.filePath(QStringLiteral("told")))
                         .toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("controls")), true);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-controls-p"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());
    QTest::qWait(500);
    window.transport_timer_->stop();
    // Nothing playing: asked all the same.
    window.refreshPluginPanels(EnginePlayback::State{});
    QLabel* title = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (title = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"))) != nullptr,
        10'000);
    auto* room = dock->findChild<QLineEdit*>(QStringLiteral("bench-plugin-panel-input-room"));
    QVERIFY(room != nullptr);
    QTest::keyClicks(room, QStringLiteral("friday-doom"));
    QPushButton* start = nullptr;
    for (auto* button :
         dock->findChildren<QPushButton*>(QStringLiteral("bench-plugin-panel-button"))) {
        if (button->text() == QStringLiteral("Start")) {
            start = button;
        }
    }
    QVERIFY(start != nullptr && start->isDefault());
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        (title = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"))) != nullptr &&
            title->text() == QStringLiteral("did start:"),
        10'000);
    QFile told{out.filePath(QStringLiteral("told"))};
    QVERIFY(told.open(QIODevice::ReadOnly));
    const auto lines = told.readAll().trimmed().split('\n');
    const auto last = QJsonDocument::fromJson(lines.last()).object();
    QCOMPARE(
        last.value(QStringLiteral("inputs")).toObject().value(QStringLiteral("room")).toString(),
        QStringLiteral("friday-doom"));
    QVERIFY(last.value(QStringLiteral("playing")).isNull());
    // Typed, it stays typed over the answer.
    QCOMPARE(dock->findChild<QLineEdit*>(QStringLiteral("bench-plugin-panel-input-room"))->text(),
             QStringLiteral("friday-doom"));
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QVERIFY(dock->grab().save(directory + QStringLiteral("/panel-controls.png")));
    }
    // An item's button: its action and value.
    auto* vote = dock->findChild<QToolButton*>(QStringLiteral("bench-plugin-panel-item-button"));
    QVERIFY(vote != nullptr);
    vote->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"))->text() ==
            QStringLiteral("did vote:7"),
        10'000);
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// ADR-0312: a panel whose answer says so is asked again about the same,
// after the seconds it names -- and no more once it stops saying so.
void BenchMainWindowTest::Plugins::aPanelIsAskedAgainWhenItSays() {
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const auto folder = pluginFolders().value(0) + QStringLiteral("/coming");
    QVERIFY(QDir{}.mkpath(folder));
    const auto asked = out.filePath(QStringLiteral("asked"));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Coming", "panels": [{"id": "p",
                          "label": "Coming", "run": ["./p"], "always": true}]})");
        QFile script{folder + QStringLiteral("/p")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        // Asked three times: twice asking again, the third not.
        script.write(QStringLiteral(R"sh(#!/bin/sh
cat > /dev/null
echo x >> '%1'
n=$(wc -l < '%1')
if [ "$n" -lt 3 ]; then
  printf '{"title": "run %s", "parts": [], "again_after": 1}' "$n"
else
  printf '{"title": "run %s", "parts": []}' "$n"
fi
)sh")
                         .arg(asked)
                         .toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("coming")), true);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-coming-p"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());
    QTest::qWait(500);
    window.transport_timer_->stop();
    window.refreshPluginPanels(EnginePlayback::State{});
    const auto title = [dock] {
        auto* label = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"));
        return label == nullptr ? QString{} : label->text();
    };
    QTRY_COMPARE_WITH_TIMEOUT(title(), QStringLiteral("run 3"), 10'000);
    // Not asked since: the third answer did not ask (the others asked
    // again after a second).
    QTest::qWait(1'500);
    QFile told{asked};
    QVERIFY(told.open(QIODevice::ReadOnly));
    QCOMPARE(told.readAll().count('\n'), 3);
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// ADR-0302: a panel showing the listening room is asked again as the room
// changes -- a vote, a friend arriving -- coalesced; one that did not ask
// for it is not.
void BenchMainWindowTest::Plugins::aRoomPanelIsAskedAgainAsTheRoomChanges() {
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const auto write_plugin = [&out](const QString& name, const QString& refresh) {
        const auto folder = pluginFolders().value(0) + QLatin1Char('/') + name;
        QDir{}.mkpath(folder);
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(QStringLiteral(R"({"schema": 1, "name": "%1", "panels": [{"id": "p",
                          "label": "%1", "run": ["./p"]%2}]})")
                           .arg(name, refresh)
                           .toUtf8());
        QFile script{folder + QStringLiteral("/p")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral("#!/bin/sh\ncat > /dev/null\necho x >> '%1'\n"
                                    "printf '{\"title\": \"%2\", \"parts\": []}'\n")
                         .arg(out.filePath(name), name)
                         .toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QSettings{}.setValue(pluginEnabledKey(name), true);
    };
    write_plugin(QStringLiteral("room"), QStringLiteral(R"(, "refresh": ["listening-room"])"));
    write_plugin(QStringLiteral("plain"), QString{});
    const auto asked = [&out](const QString& name) {
        QFile file{out.filePath(name)};
        return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
    };

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    for (const auto* name : {"room", "plain"}) {
        auto* dock = window.findChild<QDockWidget*>(
            QStringLiteral("bench-plugin-panel-%1-p").arg(QLatin1String(name)));
        QVERIFY(dock != nullptr);
        if (dock->isHidden()) {
            window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
        }
        QTRY_VERIFY(dock->isVisible());
    }
    QTest::qWait(500);
    window.transport_timer_->stop();
    EnginePlayback::State state;
    state.status = QStringLiteral("playing");
    state.entry = QStringLiteral("e1");
    state.path = QStringLiteral("/srv/music/a.flac");
    window.refreshPluginPanels(state);
    QTRY_VERIFY_WITH_TIMEOUT(
        asked(QStringLiteral("room")) >= 1 && asked(QStringLiteral("plain")) >= 1, 10'000);
    QTest::qWait(300);
    const auto room_before = asked(QStringLiteral("room"));
    const auto plain_before = asked(QStringLiteral("plain"));
    window.listeningRoomChanged();
    window.listeningRoomChanged();
    QTRY_VERIFY_WITH_TIMEOUT(asked(QStringLiteral("room")) == room_before + 1, 5'000);
    QTest::qWait(800);
    QCOMPARE(asked(QStringLiteral("room")), room_before + 1);
    QCOMPARE(asked(QStringLiteral("plain")), plain_before);
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{pluginFolders().value(0) + QStringLiteral("/room")}.removeRecursively();
    QDir{pluginFolders().value(0) + QStringLiteral("/plain")}.removeRecursively();
}

// ADR-0302: an invite opened in a browser hands Melody a melody:// link, and
// Melody asks its engine to join -- told here of a server not there.
void BenchMainWindowTest::Plugins::aTogetherLinkAsksTheEngineToJoin() {
    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().playback != nullptr &&
                                 window.localEngine().playback->active(),
                             10'000);
    QVERIFY(!window.openLink(QStringLiteral("https://example.org/rooms/r#k")));
    QVERIFY(window.openLink(QStringLiteral("melody://together/join")));
    QTRY_VERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("no invite")));
    // Every message the window shows from here: the status bar is shared,
    // and the refusal may be followed at once by another, from a window
    // still starting -- shown is what counts, not still showing.
    QStringList said;
    connect(window.statusBar(), &QStatusBar::messageChanged, &window, [&said](const QString& text) {
        if (!text.isEmpty()) {
            said << text;
        }
    });
    // Port 9 on this machine: nothing answers.
    QVERIFY(window.openLink(QStringLiteral("melody://together/join?invite=") +
                            QString::fromLatin1(QUrl::toPercentEncoding(
                                QStringLiteral("http://127.0.0.1:9/rooms/r#k")))));
    QTRY_VERIFY2_WITH_TIMEOUT(std::ranges::any_of(said,
                                                  [](const QString& text) {
                                                      return text.startsWith(QStringLiteral(
                                                          "Could not join the listening room"));
                                                  }),
                              qPrintable(said.join(QStringLiteral(" | "))), 15'000);
}

// ADR-0301: a panel is told every tag the library has of what plays, and,
// asking for them, the artist's albums there -- by the album artist's
// MusicBrainz id and name, one for each release group.
void BenchMainWindowTest::Plugins::aPluginPanelIsToldTheLibrary() {
    const auto folder = pluginFolders().value(0) + QStringLiteral("/told");
    QVERIFY(QDir{}.mkpath(folder));
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto input = media.filePath(QStringLiteral("input.json"));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Told", "panels": [{"id": "told",
                          "label": "Told", "run": ["./told"], "library": ["artist-albums"]}]})");
        QFile script{folder + QStringLiteral("/told")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        // What it was given, kept for the test to read.
        script.write(QStringLiteral("#!/bin/sh\ncat > '%1'\n"
                                    "printf '{\"title\": \"Told\", \"parts\": []}'\n")
                         .arg(input)
                         .toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("told")), true);
    // Two tracks of one release group, and another artist's.
    const auto music = media.filePath(QStringLiteral("music"));
    for (const auto* name : {"Credit/Rich/one.flac", "Credit/Rich/two.flac"}) {
        const auto path = music + QLatin1Char('/') + QLatin1String(name);
        QVERIFY(QDir{}.mkpath(QFileInfo{path}.absolutePath()));
        QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    }
    const auto other = music + QStringLiteral("/Project/Tone/tone.flac");
    QVERIFY(QDir{}.mkpath(QFileInfo{other}.absolutePath()));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), other));

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    auto* panel = window.localLibrary();
    QVERIFY(panel != nullptr);
    panel->browser().addRoot(QFile::encodeName(music).toStdString());
    QTRY_VERIFY(!panel->browser().rootPaths().empty());
    {
        persistence::LibraryScanProgress progress;
        QVERIFY(window.localCatalogue()->open()->scan({}, progress).has_value());
    }
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-told-told"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());

    // Told here that it plays; the engine, playing nothing, says nothing
    // of it, and the library says the rest.
    QTest::qWait(500);
    window.transport_timer_->stop();
    EnginePlayback::State state;
    state.status = QStringLiteral("playing");
    state.entry = QStringLiteral("e1");
    state.path = music + QStringLiteral("/Credit/Rich/one.flac");
    window.refreshPluginPanels(state);
    QTRY_VERIFY_WITH_TIMEOUT(
        dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title")) != nullptr, 10'000);
    QFile told_file{input};
    QVERIFY(told_file.open(QIODevice::ReadOnly));
    const auto told = QJsonDocument::fromJson(told_file.readAll()).object();
    const auto tags =
        told.value(QStringLiteral("playing")).toObject().value(QStringLiteral("tags")).toObject();
    QCOMPARE(tags.value(QStringLiteral("musicbrainzalbumartistid")).toArray().first().toString(),
             QStringLiteral("77777777-7777-7777-7777-777777777777"));
    QCOMPARE(tags.value(QStringLiteral("artist")).toArray().size(), 2);
    // A tag the library does not know as it is spelled, case aside.
    QCOMPARE(tags.value(QStringLiteral("custom_field")).toArray().last().toString(),
             QStringLiteral("second custom value"));
    const auto albums = told.value(QStringLiteral("artist_albums")).toArray();
    QCOMPARE(albums.size(), 1);
    const auto album = albums.first().toObject();
    QCOMPARE(album.value(QStringLiteral("album")).toString(), QStringLiteral("Rich Metadata"));
    QCOMPARE(album.value(QStringLiteral("album_artist")).toString(),
             QStringLiteral("Album Credit"));
    QCOMPARE(album.value(QStringLiteral("tracks")).toInt(), 2);
    QCOMPARE(album.value(QStringLiteral("relative_folder")).toString(),
             QStringLiteral("Credit/Rich"));
    QCOMPARE(album.value(QStringLiteral("tags"))
                 .toObject()
                 .value(QStringLiteral("musicbrainzreleasegroupid"))
                 .toArray()
                 .first()
                 .toString(),
             QStringLiteral("44444444-4444-4444-4444-444444444444"));
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// ADR-0291: a plugin's panel, toggled in the Playback menu beside Lyrics,
// asked about what plays while open and drawn as the parts it answers.
void BenchMainWindowTest::Plugins::aPluginPanelShowsWhatItSaysAboutThePlayingTrack() {
    const auto folder = pluginFolders().value(0) + QStringLiteral("/about");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "About", "panels": [{"id": "artist",
                          "label": "About the artist", "run": ["./about"]}],
                          "settings": [{"key": "greeting", "label": "Greeting",
                                        "default": "Hello"}]})");
        QFile script{folder + QStringLiteral("/about")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"sh(#!/bin/sh
input=$(cat)
artist=$(printf '%s' "$input" | grep -o '"artist":"[^"]*"' | head -n 1 | cut -d '"' -f 4)
greeting=$(printf '%s' "$input" | grep -o '"greeting":"[^"]*"' | cut -d '"' -f 4)
path=$(printf '%s' "$input" | grep -o '"path":"[^"]*"' | head -n 1 | cut -d '"' -f 4)
printf '{"title": "About %s", "parts": [{"heading": "About"}, {"paragraph": "%s from %s."}, {"text": "Site", "detail": "official", "url": "https://example.org"}]}' "$artist" "$greeting" "$path"
)sh");
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("about")), true);

    BenchMainWindow window;
    window.show();
    auto* toggle =
        window.findChild<QAction*>(QStringLiteral("action-show-plugin-panel-about-artist"));
    QVERIFY(toggle != nullptr);
    QCOMPARE(toggle->text(), QStringLiteral("About the artist"));
    // ADR-0299: no switch of its own in a menu -- enabled, it is there,
    // shown with the side panels by Panels.
    auto* lyrics = window.findChild<QAction*>(QStringLiteral("action-show-lyrics"));
    for (auto* menu : window.menuBar()->findChildren<QMenu*>()) {
        QVERIFY(!menu->actions().contains(lyrics) && !menu->actions().contains(toggle));
    }
    QVERIFY(window.createPopupMenu() == nullptr);
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-about-artist"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());
    QVERIFY(dock->findChild<QToolButton*>(QStringLiteral("bench-plugin-panel-close")) == nullptr);
    auto* message = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-message"));
    QVERIFY(message != nullptr);
    QCOMPARE(message->text(), QStringLiteral("Nothing is playing"));

    // A track plays -- told here, not by the engine, which plays nothing in
    // this test: asked about it, the answer drawn as its parts.
    // Once the engine has said what it has, it says nothing more here.
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QTest::qWait(500);
    window.transport_timer_->stop();
    EnginePlayback::State state;
    state.status = QStringLiteral("playing");
    state.entry = QStringLiteral("e1");
    state.path = QStringLiteral("/srv/music/Testament/01.flac");
    window.refreshPluginPanels(state);
    QLabel* title = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (title = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"))) != nullptr,
        10'000);
    QStringList parts;
    for (const auto* part :
         dock->findChildren<QLabel*>(QStringLiteral("bench-plugin-panel-part"))) {
        parts << part->property("part").toString();
    }
    QVERIFY2(parts.contains(QStringLiteral("heading")) &&
                 parts.contains(QStringLiteral("paragraph")) &&
                 parts.contains(QStringLiteral("item")),
             qPrintable(parts.join(QLatin1Char(','))));
    const auto paragraphs = dock->findChildren<QLabel*>(QStringLiteral("bench-plugin-panel-part"));
    QVERIFY(std::ranges::any_of(paragraphs, [](const QLabel* label) {
        return label->property("part").toString() == QStringLiteral("paragraph") &&
               label->text() == QStringLiteral("Hello from /srv/music/Testament/01.flac.");
    }));
    QCOMPARE(dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-detail"))->text(),
             QStringLiteral("official"));
    // For a look at it: an About as the official plugin answers one.
    if (const auto directory = qEnvironmentVariable("MELODY_TEST_SCREENSHOT_DIR");
        !directory.isEmpty()) {
        QImage photo{480, 320, QImage::Format_RGB32};
        photo.fill(QColor{90, 110, 140});
        const auto photo_path = directory + QStringLiteral("/photo.png");
        QVERIFY(photo.save(photo_path));
        PluginPanelContent about;
        about.title = QStringLiteral("Testament");
        using Part = PluginPanelContent::Part;
        about.parts = {
            Part{.kind = Part::Kind::image,
                 .text = {},
                 .detail = {},
                 .url = {},
                 .image = photo_path,
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::item,
                 .text = QStringLiteral("Group from Berkeley, United States"),
                 .detail = QStringLiteral("since 1983"),
                 .url = {},
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::paragraph,
                 .text = QStringLiteral(
                     "Testament is an American thrash metal band from Berkeley, California. "
                     "Formed in 1983 under the name Legacy, the band's current lineup comprises "
                     "vocalist Chuck Billy, guitarists Eric Peterson and Alex Skolnick, bassist "
                     "Steve Di Giorgio and drummer Chris Dovas. Testament has released fourteen "
                     "studio albums, and is one of the most successful thrash metal bands of "
                     "the Bay Area scene, credited with influencing many that followed."),
                 .detail = {},
                 .url = {},
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Wikipedia"),
                 .detail = QStringLiteral("Testament"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Official site"),
                 .detail = QStringLiteral("Testament"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("MusicBrainz"),
                 .detail = QStringLiteral("Testament"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::heading,
                 .text = QStringLiteral("The Legacy"),
                 .detail = {},
                 .url = {},
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::item,
                 .text = QStringLiteral("Album · 1987"),
                 .detail = QStringLiteral("Atlantic, Megaforce Worldwide"),
                 .url = {},
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::paragraph,
                 .text = QStringLiteral("The Legacy is the debut studio album by Testament."),
                 .detail = {},
                 .url = {},
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Wikipedia"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("MusicBrainz"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Discogs"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Bandcamp"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("Rate Your Music"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
            Part{.kind = Part::Kind::link,
                 .text = QStringLiteral("AllMusic"),
                 .detail = QStringLiteral("The Legacy"),
                 .url = QStringLiteral("https://x.org"),
                 .image = {},
                 .key = {},
                 .value = {},
                 .placeholder = {},
                 .action = {},
                 .secret = false,
                 .on = false,
                 .buttons = {},
                 .artist = {},
                 .cover = {},
                 .people = {},
                 .role = {}},
        };
        PluginPanelView shown{QStringLiteral("About"), nullptr};
        shown.resize(300, 900);
        shown.setSubject(QStringLiteral("Testament — Over the Wall"));
        shown.showContent(about);
        shown.show();
        QTest::qWait(200);
        QVERIFY(shown.grab().save(directory + QStringLiteral("/about.png")));
    }
    // Escaped and shown as rich text: a quote or an ampersand reads as one.
    for (const auto* part : dock->findChildren<QLabel*>()) {
        if (QStringList{QStringLiteral("bench-plugin-panel-part"),
                        QStringLiteral("bench-plugin-panel-title"),
                        QStringLiteral("bench-plugin-panel-detail")}
                .contains(part->objectName())) {
            QCOMPARE(part->textFormat(), Qt::RichText);
        }
    }

    // Turned off and saved, its panel goes.
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("about")), false);
    window.buildPluginPanels();
    QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-about-artist")) ==
            nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-show-plugin-panel-about-artist")) ==
            nullptr);
    QSettings{}.remove(QStringLiteral("plugins"));
    QSettings{}.remove(QStringLiteral("plugin-panels"));
    QDir{folder}.removeRecursively();
}

// ADR-0293: no lyrics in the file -- the lyrics providers asked in turn,
// the first to find them shown in the Lyrics panel, saying where from.
void BenchMainWindowTest::Plugins::lyricsProvidersAreAskedWhenTheFileHasNone() {
    const auto provider = [](const QString& name, const QByteArray& answer) {
        const auto folder = pluginFolders().value(0) + QLatin1Char('/') + name;
        QVERIFY(QDir{}.mkpath(folder));
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(QStringLiteral(R"({"schema": 1, "name": "%1", "providers": [{"id": "l",
                                         "provides": "lyrics", "run": ["./words"]}]})")
                           .arg(name)
                           .toUtf8());
        QFile script{folder + QStringLiteral("/words")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\ncat > /dev/null\nprintf '%s' '" + answer + "'\n");
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QSettings{}.setValue(pluginEnabledKey(name), true);
    };
    provider(QStringLiteral("a-nothing"), R"({"lyrics": null})");
    provider(
        QStringLiteral("words"),
        R"({"lyrics": "[00:01.00]First line\n[00:02.00]Second line", "source": "Words test"})");
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto file = media.filePath(QStringLiteral("no-lyrics.wav"));
    write_wave(file, 44'100);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.localEngine().does_file_work, 10'000);
    QTest::qWait(500);
    window.transport_timer_->stop();
    auto* show_lyrics = window.findChild<QAction*>(QStringLiteral("action-show-lyrics"));
    QVERIFY(show_lyrics != nullptr);
    if (!show_lyrics->isChecked()) {
        show_lyrics->trigger();
    }
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-lyrics"));
    QTRY_VERIFY(dock != nullptr && dock->isVisible());
    EnginePlayback::State state;
    state.status = QStringLiteral("playing");
    state.entry = QStringLiteral("e1");
    state.path = file;
    window.refreshLyrics(state);
    auto* lines = dock->findChild<QListWidget*>(QStringLiteral("lyrics-lines"));
    QVERIFY(lines != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(lines->count(), 2, 15'000);
    QCOMPARE(lines->item(0)->text(), QStringLiteral("First line"));
    QCOMPARE(dock->findChild<QLabel*>(QStringLiteral("lyrics-source"))->text(),
             QStringLiteral("Synced · from Words test"));
    show_lyrics->trigger();
    QSettings{}.remove(QStringLiteral("plugins"));
    QSettings{}.remove(QStringLiteral("lyrics"));
    for (const auto* name : {"a-nothing", "words"}) {
        QDir{pluginFolders().value(0) + QLatin1Char('/') + QLatin1String(name)}.removeRecursively();
    }
}

// ADR-0288: more than a few plugin entries go in a submenu for each plugin,
// under its name; a few stand in the menu itself (above).
void BenchMainWindowTest::Plugins::manyPluginActionsGoInASubmenu() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto file = media.filePath(QStringLiteral("01.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), file));
    const auto folder = pluginFolders().value(0) + QStringLiteral("/many");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Many", "actions": [
            {"id": "a", "label": "First", "run": ["true"]},
            {"id": "b", "label": "Second", "run": ["true"]},
            {"id": "c", "label": "Third", "run": ["true"]},
            {"id": "d", "label": "Fourth", "run": ["true"]}]})");
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("many")), true);
    // A plugin with only a panel, and one with only a provider: nothing of
    // theirs in the menu.
    const auto panel_only = pluginFolders().value(0) + QStringLiteral("/side");
    const auto provider_only = pluginFolders().value(0) + QStringLiteral("/finder");
    for (const auto& [where, json] :
         {std::pair{panel_only, QByteArray{R"({"schema": 1, "name": "Side", "panels": [
             {"id": "p", "label": "Side panel", "run": ["true"]}]})"}},
          std::pair{provider_only, QByteArray{R"({"schema": 1, "name": "Finder", "providers": [
             {"id": "l", "provides": "lyrics", "run": ["true"]}]})"}}}) {
        QVERIFY(QDir{}.mkpath(where));
        QFile manifest{where + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(json);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("side")), true);
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("finder")), true);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    const auto encoded = QFile::encodeName(file);
    window.openLocalPaths(
        {std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())}});
    Workspace::ListTab* tab = nullptr;
    QTRY_VERIFY((tab = [&window]() -> Workspace::ListTab* {
                    for (auto& candidate : window.list_tabs_) {
                        if (candidate->model->rowCount() == 1) {
                            return candidate.get();
                        }
                    }
                    return nullptr;
                }()) != nullptr);
    window.tabs_->setCurrentWidget(tab->view);
    tab->view->selectionModel()->select(
        tab->model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(QMetaObject::invokeMethod(
        tab->view, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, tab->view->visualRect(tab->model->index(0, local_title_column)).center())));
    auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
    QVERIFY(menu != nullptr);
    auto* submenu = menu->findChild<QMenu*>(QStringLiteral("menu-plugin-many"));
    QVERIFY(submenu != nullptr);
    QCOMPARE(submenu->title(), QStringLiteral("Many"));
    QCOMPARE(submenu->actions().size(), 4);
    QCOMPARE(submenu->actions().front()->text(), QStringLiteral("First"));
    // None of them in the menu itself.
    QVERIFY(std::ranges::none_of(menu->actions(), [](const QAction* action) {
        return action->text() == QStringLiteral("First");
    }));
    QVERIFY(menu->findChild<QMenu*>(QStringLiteral("menu-plugin-side")) == nullptr);
    QVERIFY(menu->findChild<QMenu*>(QStringLiteral("menu-plugin-finder")) == nullptr);
    menu->close();
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
    QDir{panel_only}.removeRecursively();
    QDir{provider_only}.removeRecursively();
}

// ADR-0295: a plugin's triggers -- at start, by hand from Workspace › Run,
// when the library has changed and been still, for new albums -- one run of
// each at a time, asked again while it runs, once more after.
void BenchMainWindowTest::Plugins::pluginTriggersRunOnTheLibrarysNews() {
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const auto log = out.filePath(QStringLiteral("log"));
    const auto folder = pluginFolders().value(0) + QStringLiteral("/hooks");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "Hooks", "triggers": [
            {"id": "page", "label": "Write the page", "run": ["./hook"],
             "on": ["start", "hand", "library-changed"]},
            {"id": "new", "on": "albums-added", "run": ["./hook"]}]})");
        QFile script{folder + QStringLiteral("/hook")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral(R"sh(#!/bin/sh
input=$(cat)
on=$(printf '%s' "$input" | grep -o '"on":"[^"]*"' | cut -d '"' -f 4)
folders=$(printf '%s' "$input" | grep -o '"folder":"[^"]*"' | cut -d '"' -f 4 | tr '\n' ' ')
echo "$on $folders" >> '%1'
[ "$on" = library-changed ] && sleep 1
exit 0
)sh")
                         .arg(log)
                         .toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("hooks")), true);
    qputenv("MELODY_LIBRARY_SETTLE_SECONDS", "0");
    const auto lines = [&log] {
        QFile file{log};
        if (!file.open(QIODevice::ReadOnly)) {
            return QStringList{};
        }
        QStringList read;
        for (const auto& line :
             QString::fromUtf8(file.readAll()).trimmed().split(QLatin1Char('\n'))) {
            read << line.trimmed();
        }
        return read;
    };

    BenchMainWindow window;
    window.show();
    // At start, once.
    QTRY_COMPARE(lines(), QStringList{QStringLiteral("start")});

    // By hand, from Workspace › Run.
    auto* menu = window.findChild<QMenu*>(QStringLiteral("menu-run-plugins"));
    QVERIFY(menu != nullptr && menu->menuAction()->isVisible());
    auto* page = window.findChild<QAction*>(QStringLiteral("action-run-plugin-hooks-page"));
    QVERIFY(page != nullptr && menu->actions().contains(page));
    QCOMPARE(page->text(), QStringLiteral("Write the page"));
    page->trigger();
    QTRY_COMPARE(lines().size(), 2);
    QCOMPARE(lines().back(), QStringLiteral("hand"));

    // The library changed, three times while it ran: once, and once after.
    window.libraryChanged();
    QTRY_COMPARE(lines().size(), 3);
    window.libraryChanged();
    window.libraryChanged();
    QTRY_COMPARE(lines().size(), 4);
    QTest::qWait(1'500);
    QCOMPARE(lines().size(), 4);
    QCOMPARE(lines().mid(2),
             (QStringList{QStringLiteral("library-changed"), QStringLiteral("library-changed")}));

    // New albums, told with their folders.
    window.albumsAdded({AlbumArrival{.artist = "A",
                                     .album = "B",
                                     .date = "1999",
                                     .folder = "/srv/music/A/B",
                                     .paths = {"/srv/music/A/B/01.flac"}}});
    QTRY_COMPARE(lines().size(), 5);
    QCOMPARE(lines().back(), QStringLiteral("albums-added /srv/music/A/B"));

    // Turned off: its entry goes, and the menu with it.
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("hooks")), false);
    window.buildPluginTriggers();
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-run-plugin-hooks-page")) == nullptr);
    QVERIFY(!menu->menuAction()->isVisible());
    window.libraryChanged();
    QTest::qWait(300);
    QCOMPARE(lines().size(), 5);

    qunsetenv("MELODY_LIBRARY_SETTLE_SECONDS");
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

// An album melody-cli queued and played is in no list of the window's until
// its queue has come, after the entry that plays: a panel asked then was told
// no tags, and the About plugin said the track names no artist. What plays is
// the engine's to say.
void BenchMainWindowTest::Plugins::aPluginPanelIsToldWhatAnotherClientStarted() {
    const auto folder = pluginFolders().value(0) + QStringLiteral("/about");
    QVERIFY(QDir{}.mkpath(folder));
    {
        QFile manifest{folder + QStringLiteral("/plugin.json")};
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(R"({"schema": 1, "name": "About", "panels": [{"id": "artist",
                          "label": "About the artist", "run": ["./about"]}]})");
        QFile script{folder + QStringLiteral("/about")};
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"sh(#!/bin/sh
artist=$(cat | grep -o '"artist":"[^"]*"' | head -n 1 | cut -d '"' -f 4)
printf '{"title": "About %s", "parts": []}' "$artist"
)sh");
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QSettings{}.setValue(pluginEnabledKey(QStringLiteral("about")), true);

    QTemporaryDir remote_state;
    QTemporaryDir media;
    QVERIFY(remote_state.isValid() && media.isValid());
    testing::TestEngine remote;
    QVERIFY2(remote.start(remote_state.path().toStdString(), true), remote.log().constData());
    const auto path = media.filePath(QStringLiteral("track.wav"));
    write_wave(path, wave_sample_rate * 60U);

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(window.lists_restored_);
    QTRY_VERIFY(window.remotePlayback() != nullptr && window.remotePlayback()->active());
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("bench-plugin-panel-about-artist"));
    QVERIFY(dock != nullptr);
    if (dock->isHidden()) {
        window.findChild<QAction*>(QStringLiteral("action-toggle-panels"))->trigger();
    }
    QTRY_VERIFY(dock->isVisible());

    auto other = protocol::Client::connect(protocol::Endpoint{
        .socket = remote.socket().toStdString(), .host = {}, .port = 0, .token = {}});
    QVERIFY(other.has_value());
    const auto entry = core::StableId::random().to_string();
    auto entries = protocol::Json::array();
    entries.push_back(protocol::Json{
        {"entry", entry},
        {"path", protocol::encode_raw_path(QFile::encodeName(path).toStdString())},
        {"title", "Over the Wall"},
        {"group",
         protocol::Json{{"artist", "Testament"}, {"album", "The Legacy"}, {"date", "1987"}}}});
    QVERIFY((*other)
                ->call("playback.replace_queue", protocol::Json{{"entries", std::move(entries)}})
                .has_value());
    QVERIFY((*other)->call("playback.play", protocol::Json{{"entry", entry}}).has_value());

    QLabel* title = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (title = dock->findChild<QLabel*>(QStringLiteral("bench-plugin-panel-title"))) != nullptr,
        10'000);
    // Its first answer, not one asked again once the queue had come.
    QCOMPARE(title->text(), QStringLiteral("About Testament"));
    QSettings{}.remove(QStringLiteral("plugins"));
    QDir{folder}.removeRecursively();
}

} // namespace trackknife::bench

MELODY_QTEST_SHARDED_MAIN(trackknife::bench::BenchMainWindowTest::Plugins)

#include "window_plugins_test.moc"
