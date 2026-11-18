// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/cover_loader.hpp"

#include <QBuffer>
#include <QSignalSpy>
#include <QtTest>

#include <atomic>
#include <future>
#include <memory>
#include <vector>

namespace trackknife::bench {
namespace {

[[nodiscard]] std::vector<unsigned char> png(const QColor& colour) {
    QImage image{8, 8, QImage::Format_RGB32};
    image.fill(colour);
    QByteArray bytes;
    QBuffer buffer{&bytes};
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] CoverLoader::Fetch answering(std::vector<unsigned char> bytes,
                                           std::shared_ptr<std::atomic_int> asked = {}) {
    return [bytes = std::move(bytes), asked = std::move(asked)](
               const core::CancellationToken&) -> core::Result<std::vector<unsigned char>> {
        if (asked) {
            ++*asked;
        }
        return bytes;
    };
}

} // namespace

// The tabs' and the library's covers: one asked at a time, kept, and asked
// again only when forgotten or not answered.
class CoverLoaderTest final : public QObject {
    Q_OBJECT

  private slots:
    void aCoverIsAskedOnceAndKept() {
        CoverLoader covers{16};
        QSignalSpy loaded{&covers, &CoverLoader::loaded};
        const auto asked = std::make_shared<std::atomic_int>(0);
        covers.request(QStringLiteral("a"), answering(png(Qt::red), asked));
        covers.request(QStringLiteral("a"), answering(png(Qt::red), asked));
        QVERIFY(covers.pending(QStringLiteral("a")));
        QTRY_COMPARE(loaded.count(), 1);
        QVERIFY(!covers.cached(QStringLiteral("a"))->isNull());
        covers.request(QStringLiteral("a"), answering(png(Qt::red), asked));
        QTest::qWait(50);
        QCOMPARE(asked->load(), 1);
    }

    void noCoverIsKeptAsNone() {
        CoverLoader covers{16};
        QSignalSpy loaded{&covers, &CoverLoader::loaded};
        covers.request(QStringLiteral("a"), answering({}));
        QTRY_COMPARE(loaded.count(), 1);
        QVERIFY(covers.cached(QStringLiteral("a")).has_value());
        QVERIFY(covers.cached(QStringLiteral("a"))->isNull());
    }

    void anEngineNotReachedIsNotTakenForNone() {
        CoverLoader covers{16};
        QSignalSpy loaded{&covers, &CoverLoader::loaded};
        covers.request(
            QStringLiteral("a"),
            [](const core::CancellationToken&) -> core::Result<std::vector<unsigned char>> {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::backend, .message = "not reached", .context = {}});
            });
        QTRY_VERIFY(!covers.pending(QStringLiteral("a")));
        QCOMPARE(loaded.count(), 0);
        QVERIFY(!covers.cached(QStringLiteral("a")));
        covers.request(QStringLiteral("a"), answering(png(Qt::blue)));
        QTRY_COMPARE(loaded.count(), 1);
    }

    void aCoverForgottenWhileAskedIsAskedAfresh() {
        CoverLoader covers{16};
        QSignalSpy loaded{&covers, &CoverLoader::loaded};
        std::promise<void> release;
        auto released = release.get_future().share();
        covers.request(
            QStringLiteral("a"),
            [released](const core::CancellationToken&) -> core::Result<std::vector<unsigned char>> {
                released.wait();
                return png(Qt::red);
            });
        QTRY_VERIFY(covers.pending(QStringLiteral("a")));
        covers.forget(QStringLiteral("a"));
        QVERIFY(!covers.pending(QStringLiteral("a")));
        covers.request(QStringLiteral("a"), answering(png(Qt::green)));
        release.set_value();
        QTRY_COMPARE(loaded.count(), 1);
        QCOMPARE(covers.cached(QStringLiteral("a"))->pixelColor(0, 0), QColor{Qt::green});
    }

    void onlyTheWantedAreAskedFor() {
        CoverLoader covers{16};
        QSignalSpy loaded{&covers, &CoverLoader::loaded};
        std::promise<void> release;
        auto released = release.get_future().share();
        covers.request(
            QStringLiteral("a"),
            [released](const core::CancellationToken&) -> core::Result<std::vector<unsigned char>> {
                released.wait();
                return png(Qt::red);
            });
        covers.request(QStringLiteral("b"), answering(png(Qt::red)));
        covers.request(QStringLiteral("c"), answering(png(Qt::red)));
        covers.keepOnly({QStringLiteral("c")});
        QVERIFY(!covers.pending(QStringLiteral("a")) && !covers.pending(QStringLiteral("b")));
        release.set_value();
        QTRY_COMPARE(loaded.count(), 1);
        QCOMPARE(loaded.front().front().toString(), QStringLiteral("c"));
        QVERIFY(!covers.cached(QStringLiteral("a")) && !covers.cached(QStringLiteral("b")));
    }
};

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::CoverLoaderTest)
#include "cover_loader_test.moc"
