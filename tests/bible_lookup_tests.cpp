// Task BI.2 — the importer's session rules, driven through a scripted
// network manager: success, offline, timeout, cancel, a superseded reply,
// the size cap, HTTP answers that are not a passage, and the rule that
// changing the translation retires the answered verses. No real network
// is touched; the fake replies are the same QNetworkReply objects the
// production code drives.
#include "BibleImport.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <QVector>

#include <cstdio>
#include <cstring>

namespace {

int gFailed = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::fflush(stderr);
        ++gFailed;
    }
    return condition;
}

class FakeReply final : public QNetworkReply {
public:
    FakeReply(const QNetworkRequest& request, QByteArray body, QNetworkReply::NetworkError error,
              QString errorString, int status, int delayMs)
        : fBody(std::move(body)), fError(error), fErrorString(std::move(errorString)),
          fStatus(status), fDelayMs(delayMs) {
        setRequest(request);
        // QIODevice must be open for the production code's readAll() to
        // reach readData() at all.
        open(QIODevice::ReadOnly);
        fTimer.setSingleShot(true);
        connect(&fTimer, &QTimer::timeout, this, &FakeReply::deliver);
        fTimer.start(fDelayMs);
    }

    void abort() override {
        if (isFinished()) {
            return;
        }
        fTimer.stop();
        if (fError == QNetworkReply::NoError) {
            fError = QNetworkReply::OperationCanceledError;
            fErrorString = QStringLiteral("Operation canceled");
        }
        deliver();
    }

private:
    void deliver() {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, fStatus);
        if (fError != QNetworkReply::NoError) {
            setError(fError, fErrorString);
        }
        emit metaDataChanged();
        emit finished();
    }

protected:
    qint64 readData(char* data, qint64 maxlen) override {
        const qint64 n = qMin(maxlen, static_cast<qint64>(fBody.size()) - fPos);
        if (n > 0) {
            std::memcpy(data, fBody.constData() + fPos, static_cast<std::size_t>(n));
        }
        fPos += n;
        return n;
    }
    qint64 writeData(const char*, qint64) override { return 0; }

    QByteArray fBody;
    QNetworkReply::NetworkError fError;
    QString fErrorString;
    int fStatus;
    int fDelayMs;
    qint64 fPos = 0;
    QTimer fTimer;
};

class ScriptedManager final : public QNetworkAccessManager {
public:
    struct Step {
        QByteArray body;
        QNetworkReply::NetworkError error = QNetworkReply::NoError;
        QString errorString;
        int status = 200;
        int delayMs = 0;
    };

    QNetworkReply* createRequest(Operation, const QNetworkRequest& request,
                                 QIODevice*) override {
        ++requests;
        lastUrl = request.url();
        lastTimeoutMs = request.transferTimeout();
        lastCacheSave = request.attribute(QNetworkRequest::CacheSaveControlAttribute);
        lastCacheLoad = request.attribute(QNetworkRequest::CacheLoadControlAttribute);
        lastUserAgent = request.rawHeader("User-Agent");
        const Step step = steps.isEmpty() ? Step{} : steps.takeFirst();
        return new FakeReply(request, step.body, step.error, step.errorString, step.status,
                             step.delayMs);
    }

    int requests = 0;
    QVector<Step> steps;
    QUrl lastUrl;
    int lastTimeoutMs = 0;
    QVariant lastCacheSave;
    QVariant lastCacheLoad;
    QByteArray lastUserAgent;
};

// A two-verse passage in the live shape (BI.1's fixture structure), used
// as the body a reply answers with.
QByteArray passageBody(const QString& version) {
    return QStringLiteral(R"HTML(<div class="passage-text"><div class='passage-content'>
    <div class="version-%1 result-text-style-normal text-html">
    <p class="chapter-1"><span class="text 1Test-1-1"><span class="chapternum">1&#160;</span>First kept words.</span></p>
    <p><span class="text 1Test-1-2"><sup class="versenum">2&#160;</sup>Second kept words.</span></p>
    </div></div></div>
    <div class="publisher-info-bottom"><strong>Example Bible (%1)</strong>
    <p>Used by permission.</p></div>)HTML")
        .arg(version)
        .toUtf8();
}

void waitForIdle(BibleImport& importer, int ms = 2000) {
    QElapsedTimer clock;
    clock.start();
    while (importer.busy() && clock.elapsed() < ms) {
        QTest::qWait(10);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    // --- a successful lookup lands verses, selection and attribution ----
    {
        BibleImport importer;
        ScriptedManager manager;
        importer.setNetworkAccessManager(&manager);
        manager.steps.push_back({passageBody("EXAMPLE"), {}, {}, 200, 0});
        importer.setReference(QStringLiteral("First Test 1"));
        importer.setTranslation(QStringLiteral("EXAMPLE"));
        // Before any lookup the importer carries the plan's notice.
        check(importer.status().contains(QStringLiteral("internet connection required")),
              "the importer opens on the internet notice");
        importer.lookup();
        check(importer.busy(), "a lookup is busy until the reply answers");
        waitForIdle(importer);
        check(!importer.busy(), "the reply retires the busy state");
        check(importer.verses().size() == 2, "the passage's two verses arrive");
        check(importer.verses().at(0).toMap().value(QStringLiteral("selected")).toBool(),
              "fetched verses start selected");
        check(importer.attribution().contains(QStringLiteral("permission")),
              "the licence text travels with the passage");
        const QUrlQuery query(manager.lastUrl);
        check(manager.lastUrl.scheme() == QStringLiteral("https") &&
                  query.queryItemValue(QStringLiteral("search")) ==
                      QStringLiteral("First Test 1") &&
                  query.queryItemValue(QStringLiteral("version")) == QStringLiteral("EXAMPLE"),
              "the request is HTTPS with the encoded search and version parameters");
        check(manager.lastTimeoutMs > 0, "the request carries a transfer timeout");
        // The disk-silence rule, asserted at the request itself: nothing
        // fetched may be cached, and everything must go to the network.
        check(manager.lastCacheSave.isValid() && manager.lastCacheSave.toBool() == false &&
                  manager.lastCacheLoad.toInt() == QNetworkRequest::AlwaysNetwork,
              "the lookup refuses every cache save and every cache read");
        check(!manager.lastUserAgent.isEmpty() &&
                  !manager.lastUserAgent.contains("Mozilla/5.0 (Linux") &&
                  manager.lastUserAgent.contains("passage lookup"),
              "the request identifies itself as one passage lookup");
        // Deselection is per verse and total.
        importer.setVerseSelected(0, false);
        check(importer.selectedVerses().size() == 1, "deselecting one leaves the other kept");
        importer.selectAllVerses(true);
        check(importer.selectedVerses().size() == 2, "select-all keeps the whole passage");
    }

    // --- offline, then a retry that works --------------------------------
    {
        BibleImport importer;
        ScriptedManager manager;
        importer.setNetworkAccessManager(&manager);
        manager.steps.push_back(
            {{}, QNetworkReply::HostNotFoundError, QStringLiteral("Host not found"), 0, 0});
        importer.setReference(QStringLiteral("Psalm 23"));
        importer.lookup();
        waitForIdle(importer);
        check(importer.verses().isEmpty() &&
                  importer.status().contains(QStringLiteral("internet connection required")),
              "a lost connection reports the notice, not a passage");
        manager.steps.push_back({passageBody("NIV"), {}, {}, 200, 0});
        importer.retry();
        waitForIdle(importer);
        check(manager.requests == 2 && importer.verses().size() == 2,
              "retry asks again and a live answer lands");
    }

    // --- a timed-out transfer says timed out ------------------------------
    {
        BibleImport importer;
        ScriptedManager manager;
        importer.setNetworkAccessManager(&manager);
        manager.steps.push_back({{},
                                 QNetworkReply::OperationCanceledError,
                                 QStringLiteral("Transfer timed out."),
                                 0,
                                 30});
        importer.setReference(QStringLiteral("John 3"));
        importer.lookup();
        waitForIdle(importer);
        check(importer.status().contains(QStringLiteral("timed out")),
              "the timeout gets its own sentence and a retryable state");
    }

    // --- a stale reply cannot replace the current passage ------------------
    {
        BibleImport importer;
        ScriptedManager manager;
        importer.setNetworkAccessManager(&manager);
        manager.steps.push_back({passageBody("EXAMPLE"), {}, {}, 200, 120});
        manager.steps.push_back({passageBody("EXAMPLE"), {}, {}, 200, 0});
        importer.setReference(QStringLiteral("Slow one"));
        importer.setTranslation(QStringLiteral("EXAMPLE"));
        importer.lookup();
        QTest::qWait(20);
        importer.setReference(QStringLiteral("Fast two"));
        importer.lookup();
        waitForIdle(importer);
        const int requestsAtAnswer = manager.requests;
        QTest::qWait(200);  // the slow first answer lands after being retired
        check(requestsAtAnswer == 2 && manager.requests == 2 && importer.verses().size() == 2,
              "the newer passage stands after the superseded reply arrives");
        check(!importer.busy(), "the superseded reply leaves the importer idle");
    }

    // --- cancel mid-flight: nothing arrives afterwards ----------------------
    {
        BibleImport importer;
        ScriptedManager manager;
        importer.setNetworkAccessManager(&manager);
        manager.steps.push_back({passageBody("EXAMPLE"), {}, {}, 200, 120});
        importer.setReference(QStringLiteral("Cancelled"));
        importer.lookup();
        QTest::qWait(10);
        importer.cancel();
        check(!importer.busy() && importer.status().contains(QStringLiteral("cancelled")),
              "cancel returns the importer to an idle, cancelled state");
        QTest::qWait(200);
        check(importer.verses().isEmpty() &&
                  importer.status().contains(QStringLiteral("cancelled")),
              "the retired reply cannot write a passage after the cancel");
    }

    // --- an answer over the size cap is refused -----------------------------
    {
        BibleImport importer;
        ScriptedManager manager;
        importer.setNetworkAccessManager(&manager);
        // The fake delivers the whole body at once, so the cap bites on
        // the finished reply's size rather than mid-progress; the plan's
        // rule is the same: nothing over the limit is parsed or kept.
        manager.steps.push_back({QByteArray(7 * 1024 * 1024, 'x'), {}, {}, 200, 0});
        importer.setReference(QStringLiteral("Huge"));
        importer.lookup();
        waitForIdle(importer);
        check(importer.verses().isEmpty() &&
                  importer.status().contains(QStringLiteral("larger than the importer's limit")),
              "the size cap refuses without parsing anything");
    }

    // --- an HTTP answer that is not a passage --------------------------------
    {
        BibleImport importer;
        ScriptedManager manager;
        importer.setNetworkAccessManager(&manager);
        manager.steps.push_back({QByteArray("<html>forbidden</html>"), {}, {}, 403, 0});
        importer.setReference(QStringLiteral("Nosey"));
        importer.lookup();
        waitForIdle(importer);
        check(importer.status().contains(QStringLiteral("403")),
              "an HTTP error answers with its number and imports nothing");
    }

    // --- an empty reference asks for a reference ------------------------------
    {
        BibleImport importer;
        ScriptedManager manager;
        importer.setNetworkAccessManager(&manager);
        importer.lookup();
        check(manager.requests == 0 &&
                  importer.status().contains(QStringLiteral("enter a reference")),
              "looking up nothing sends nothing");
    }

    // --- moving the translation retires the answered verses --------------------
    {
        BibleImport importer;
        ScriptedManager manager;
        importer.setNetworkAccessManager(&manager);
        manager.steps.push_back({passageBody("EXAMPLE"), {}, {}, 200, 0});
        importer.setReference(QStringLiteral("First Test 1"));
        importer.setTranslation(QStringLiteral("EXAMPLE"));
        importer.lookup();
        waitForIdle(importer);
        check(importer.verses().size() == 2, "the passage is on screen");
        importer.setTranslation(QStringLiteral("KJV"));
        check(importer.verses().isEmpty() && importer.attribution().isEmpty(),
              "a passage belongs to the translation it was fetched in; the menu change drops it");
    }

    // --- BIF.2: the untouched default asks for exactly what a chosen row asks
    {
        // Same reference, one importer that never touches the translation
        // (the operator's path once the picker opens on NIV) and one that
        // sets it as the menu's activation does. The URLs must be
        // identical — the default is a choice like any other, not a hole.
        BibleImport untouched;
        ScriptedManager untouchedManager;
        untouched.setNetworkAccessManager(&untouchedManager);
        untouchedManager.steps.push_back({passageBody("NIV"), {}, {}, 200, 0});
        untouched.setReference(QStringLiteral("First Test 1"));
        untouched.lookup();
        waitForIdle(untouched);

        BibleImport chosen;
        ScriptedManager chosenManager;
        chosen.setNetworkAccessManager(&chosenManager);
        chosenManager.steps.push_back({passageBody("NIV"), {}, {}, 200, 0});
        chosen.setReference(QStringLiteral("First Test 1"));
        chosen.setTranslation(QStringLiteral("NIV"));  // what onActivated does
        chosen.lookup();
        waitForIdle(chosen);

        check(untouched.translation() == QStringLiteral("NIV"),
              "the session's default translation is NIV");
        check(untouchedManager.requests == 1 && chosenManager.requests == 1,
              "each path sent exactly one request");
        check(untouchedManager.lastUrl == chosenManager.lastUrl,
              "the default translation sends exactly the menu-chosen request");
    }

    if (gFailed == 0) {
        std::printf("bible_lookup_tests: all fixtures passed\n");
        return 0;
    }
    std::printf("bible_lookup_tests: %d FAILURES\n", gFailed);
    return 1;
}
