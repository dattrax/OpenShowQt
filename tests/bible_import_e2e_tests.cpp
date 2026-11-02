/*
 * Task BI.6 acceptance: the whole Bible import in one journey, one real
 * worker, one sandboxed home.
 *
 * The page fixture (bible_import_page_tests) proves each seam of the page;
 * this fixture proves the MILESTONE's sentences end to end: a connected
 * (scripted-provider) import lands on today's schedule and saves; with the
 * provider unreachable afterwards, a new lookup fails on its own actionable
 * line while the SAVED slides still play — the reopened controller presents
 * the passage on a live worker with zero network requests, because the
 * schedule is the passage's only durable owner and playback of what it holds
 * needs no connection at all. And the disk itself is read as a witness:
 * HOME, cache, data and config are sandboxed into the scratch tree, so after
 * the journey the byte-for-byte question is answerable — the fetched HTML
 * and the deselected verse appear in NO file anywhere under the sandbox,
 * while the selected words appear only inside the service's own records.
 * Fixtures stay synthetic: no live provider is ever contacted from here.
 *
 * Needs a display (the worker opens its output window): desktop preset,
 * label display.
 */

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVariantMap>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "BibleImport.h"
#include "DocumentController.h"
#include "ScriptureImport.h"
#include "WorkerController.h"

namespace {

using openshow::ui::DocumentController;
using openshow::ui::ScriptureImport;
using openshow::ui::WorkerController;

int gFailed = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::fflush(stderr);
        ++gFailed;
    }
    return condition;
}

// --- the scripted network (the BI.2 fixture shape, kept local) --------------

class FakeReply final : public QNetworkReply {
public:
    FakeReply(const QNetworkRequest& request, QByteArray body, QNetworkReply::NetworkError error,
              QString errorString, int status, int delayMs)
        : fBody(std::move(body)), fError(error), fErrorString(std::move(errorString)),
          fStatus(status), fDelayMs(delayMs) {
        setRequest(request);
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
        const Step step = steps.isEmpty() ? Step{} : steps.takeFirst();
        return new FakeReply(request, step.body, step.error, step.errorString, step.status,
                             step.delayMs);
    }

    QVector<Step> steps;
    int requests = 0;
};

// Six verses in the live BI.1 shape. Verse 3 is the DESELECTED one: its
// distinctive words must never reach any file under the sandbox.
QByteArray passageBody() {
    return R"HTML(<div class="passage-text"><div class='passage-content'>
    <div class="version-EXAMPLE result-text-style-normal text-html">
    <p class="chapter-1"><span class="text 1Test-1-1"><span class="chapternum">1&#160;</span>First kept words of the passage.</span></p>
    <p><span class="text 1Test-1-2">Second kept words shine here.</span></p>
    <p><span class="text 1Test-1-3">Third kept words stand fast forever.</span></p>
    <p><span class="text 1Test-1-4">Fourth verse has three much longer sentences that keep running past the end of every line. They test the split boundary inside the measured packing engine. Nothing at all may ever be clipped from the operator's words.</span></p>
    <p><span class="text 1Test-1-5">Fifth kept words are near.</span></p>
    <p><span class="text 1Test-1-6">Sixth kept words close the line.</span></p>
    </div></div></div>
    <div class="publisher-info-bottom"><strong>Example Bible (EXAMPLE)</strong>
    <p>Used by permission.</p></div>)HTML";
}

// --- waiting helpers ---------------------------------------------------------

void waitForLookup(BibleImport& bible, int ms = 3000) {
    QElapsedTimer clock;
    clock.start();
    while (bible.busy() && clock.elapsed() < ms) {
        QTest::qWait(10);
    }
}

bool waitForMeasured(ScriptureImport& scripture, int ms = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (scripture.measuring() && clock.elapsed() < ms) {
        QTest::qWait(20);
    }
    return !scripture.measuring();
}

bool waitForMeasureReady(WorkerController& worker, int ms = 30000) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        QVariantMap probe;
        probe.insert(QStringLiteral("key"), QStringLiteral("probe"));
        probe.insert(QStringLiteral("lines"), QVariantList{QStringLiteral("x")});
        probe.insert(QStringLiteral("fontSize"), 40.0);
        probe.insert(QStringLiteral("width"), 200.0);
        probe.insert(QStringLiteral("lineSpacing"), 1.25);
        if (worker.measureText(QVariantList{probe}) != 0) {
            return true;
        }
        QTest::qWait(50);
    }
    return false;
}

bool waitForOnAir(WorkerController& worker, int ms = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (!worker.wordsOnAir() && clock.elapsed() < ms) {
        QTest::qWait(20);
    }
    return worker.wordsOnAir();
}

// --- the disk as witness ------------------------------------------------------

// Every regular file under `root`, read whole. The sandbox holds the service
// directories AND HOME/config/cache/data, so one walk answers the plan's
// whole "does anything persist outside the schedule?" question.
struct Witness {
    int files = 0;
    std::vector<std::filesystem::path> containing;  // files holding a watched token
};

bool fileHolds(const std::filesystem::path& path, const std::string& token) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes.find(token) != std::string::npos;
}

Witness walk(const std::filesystem::path& root, const std::vector<std::string>& tokens) {
    Witness witness;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        ++witness.files;
        for (const std::string& token : tokens) {
            if (fileHolds(entry.path(), token)) {
                witness.containing.push_back(entry.path());
                break;
            }
        }
    }
    return witness;
}

}  // namespace

int main(int argc, char* argv[]) {
    // The whole app-owned world goes into the scratch tree BEFORE the
    // application object exists, so the walk sees every byte the journey —
    // including anything Qt or the network layer would rather write than
    // forget. (The lookup asks for no cache at the request itself; this is
    // how that claim gets audited rather than trusted.)
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) {
        return 1;
    }
    const std::filesystem::path root = std::filesystem::path(sandbox.path().toStdString());
    std::filesystem::create_directories(root / "home");
    ::setenv("HOME", sandbox.path().toUtf8().constData(), 1);
    ::setenv("XDG_CONFIG_HOME", (root / "home" / "config").string().c_str(), 1);
    ::setenv("XDG_CACHE_HOME", (root / "home" / "cache").string().c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root / "home" / "data").string().c_str(), 1);

    QCoreApplication app(argc, argv);

    DocumentController docs(nullptr, root / "service");
    WorkerController worker;
    worker.start();
    if (!check(waitForMeasureReady(worker), "the worker came up and measures")) {
        worker.shutdownNow();
        return 1;
    }

    BibleImport bible;
    ScriptedManager manager;
    bible.setNetworkAccessManager(&manager);
    ScriptureImport scripture(&bible, &docs, &worker);

    // --- the connected import: lookup, selection edit, Add, save -----------
    bible.setReference(QStringLiteral("First Test 1:1-6"));
    bible.setTranslation(QStringLiteral("EXAMPLE"));
    manager.steps.push_back({passageBody(), {}, {}, 200, 0});
    bible.lookup();
    waitForLookup(bible);
    check(bible.verses().size() == 6, "the connected lookup answered with six verses");
    check(waitForMeasured(scripture), "the selected verses paginated");
    // The operator drops verse 3: its words must never be persisted anywhere.
    bible.setVerseSelected(2, false);
    check(waitForMeasured(scripture), "the shortened selection re-paginated");
    check(scripture.addToSchedule(), "Add lands the passage on today's schedule");
    check(docs.save(), "the service saves with its imported passage");
    check(manager.requests == 1, "selection, Add and save asked the network for nothing");

    // --- the disk as witness -------------------------------------------------
    // Watched bytes: the fetched HTML's own furniture, and the DESELECTED
    // verse's words. Neither may appear in any file under the sandbox. The
    // selected words may appear only inside the service's own records.
    const auto htmlWitness = walk(root, {"passage-text", "publisher-info-bottom",
                                         "version-EXAMPLE", "result-text-style-normal"});
    check(htmlWitness.files > 0, "the walk saw the sandbox's files");
    if (!htmlWitness.containing.empty()) {
        std::fprintf(stderr, "  fetched markup found in: %s\n",
                     htmlWitness.containing.front().string().c_str());
    }
    check(htmlWitness.containing.empty(),
          "no fetched page bytes persist anywhere under the sandbox (no cache, no scratch)");
    const auto droppedWitness = walk(root, {"stand fast forever"});
    check(droppedWitness.containing.empty(),
          "the deselected verse's words were never written anywhere");
    const auto keptWitness = walk(root, {"Sixth kept words close the line"});
    bool keptOnlyInService = !keptWitness.containing.empty();
    for (const auto& path : keptWitness.containing) {
        const std::string where = path.string();
        if (where.find((root / "service").string()) != 0) {
            keptOnlyInService = false;
        }
    }
    check(keptOnlyInService,
          "the imported words live only inside the service's own records");

    // --- the provider becomes unreachable; the saved item does not care ----
    bible.setReference(QStringLiteral("First Test 2:1"));
    manager.steps.push_back(
        {{}, QNetworkReply::HostNotFoundError, QStringLiteral("Host not found"), 0, 0});
    bible.lookup();
    waitForLookup(bible);
    check(!bible.busy() && bible.verses().isEmpty() && !bible.status().isEmpty(),
          "the disconnected lookup fails on its own actionable line");
    check(!scripture.canAdd() && docs.playlistEntries().size() == 1,
          "the failure changes nothing: the saved schedule row stands alone");

    // --- offline playback: the saved slides go on air with no network ------
    // A fresh controller reads the service back from disk (the journey's
    // in-memory state is gone), and the presentation path is the ordinary
    // one — the passage plays because the schedule owns it, not because a
    // connection does.
    worker.clearSlide();
    QTest::qWait(100);
    DocumentController reopened(nullptr, root / "service");
    if (!check(reopened.load(), "a fresh controller reopens the service from disk")) {
        worker.shutdownNow();
        return gFailed == 0 ? 0 : 1;
    }
    reopened.selectPlaylist(reopened.currentPlaylistId());
    const QVariantList rows = reopened.playlistEntries();
    const QString passageShow =
        rows.isEmpty() ? QString() : rows.at(0).toMap().value(QStringLiteral("showId")).toString();
    const QVariantList savedSlides = reopened.slidesOf(passageShow);
    check(!passageShow.isEmpty() && !savedSlides.isEmpty(),
          "the reopened schedule still holds the imported passage");
    int played = 0;
    for (const QVariant& slide : savedSlides) {
        const QString slideId = slide.toMap().value(QStringLiteral("id")).toString();
        const QString sceneJson = reopened.sceneJsonForSlide(slideId);
        if (sceneJson.isEmpty()) {
            break;
        }
        // The ordinary one-call go, through the same controller the panels
        // drive. Nothing in this path can reach the network: the scripted
        // manager would answer zero further requests, and its counter is
        // the assertion that it tried none.
        worker.presentSceneJson(sceneJson);
        if (!waitForOnAir(worker)) {
            break;
        }
        ++played;
    }
    check(played == savedSlides.size() && played >= 2,
          "every saved slide played back offline on the real output");
    check(worker.wordsOnAir(), "the audience holds the imported words with the provider down");
    check(manager.requests == 2,
          "playback of saved slides asked the network for nothing (only the two lookups ran)");

    worker.shutdownNow();
    if (gFailed == 0) {
        std::printf("bible_import_e2e_tests: all fixtures passed\n");
        return 0;
    }
    std::printf("bible_import_e2e_tests: %d FAILURES\n", gFailed);
    return 1;
}
