/*
 * Task BI.6 acceptance, plus the BIF.4 re-acceptance: the whole Bible
 * import in one journey, one real worker, one sandboxed home.
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
 * Then the BIF.4 journey walks the PRODUCTION shape once more the way the
 * operator actually moves: the completer's whole-chapter answer taken from
 * three typed letters, the translation picker never touched (the untouched
 * default must be the request that goes out), Add, save, and offline
 * playback of the chapter on the real worker.
 * Then the TF.4 re-acceptance: the SAME chapter again after the service
 * wears a theme that has grown a `scripture` reading context — the plan
 * names the context, the saved slides are the previewed slides line for
 * line, every saved scene compiles inside the context's box, and the PGM
 * picture is the previewed flow.
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
#include <QUrl>
#include <QUrlQuery>
#include <QVariantMap>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <boost/json.hpp>

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
        urls.push_back(request.url());
        const Step step = steps.isEmpty() ? Step{} : steps.takeFirst();
        return new FakeReply(request, step.body, step.error, step.errorString, step.status,
                             step.delayMs);
    }

    QVector<Step> steps;
    QVector<QUrl> urls;
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

// The BIF.4 production shape's chapter: the live BI.1 furniture again, but
// nothing deselected — six verses of a whole chapter, one of them long
// enough to make the packer split. It answers NIV, because the importer
// refuses any page that answers a different translation than it asked for.
// Its phrases are the journey's watched tokens: they must end up only
// inside the service's records.
QByteArray chapterBody() {
    return R"HTML(<div class="passage-text"><div class='passage-content'>
    <div class="version-NIV result-text-style-normal text-html">
    <p class="chapter-1"><span class="text 1Cor-1-1"><span class="chapternum">1&#160;</span>Whole chapter words one open the reading.</span></p>
    <p><span class="text 1Cor-1-2">Whole chapter words two carry the tune.</span></p>
    <p><span class="text 1Cor-1-3">Whole chapter words three stand in the light.</span></p>
    <p><span class="text 1Cor-1-4">Fourth of the chapter runs long enough to force the packer to split words across measured slides the way a real reading does. It keeps going and going until the box simply runs out of room for another line. Nothing at all may ever be clipped from the operator's words.</span></p>
    <p><span class="text 1Cor-1-5">Whole chapter words five near the close.</span></p>
    <p><span class="text 1Cor-1-6">Whole chapter words six seal the paragraph.</span></p>
    </div></div></div>
    <div class="publisher-info-bottom"><strong>Example Bible (NIV)</strong>
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

    // --- BIF.4: the production shape, end to end ------------------------------
    // The operator's real route through the fixed page: three typed letters,
    // the completer's whole-chapter answer taken as-is, the translation
    // picker never touched, Add, save — then playback from what the DISK
    // alone knows. One fresh session plays the next service's operator.
    const QVariantList suggestions = bible.suggestBooks(QStringLiteral("1 cor 1"));
    const QString chapterRef =
        suggestions.isEmpty() ? QString()
                              : suggestions.first()
                                        .toMap()
                                        .value(QStringLiteral("completion"))
                                        .toString();
    check(chapterRef == QStringLiteral("1 Corinthians 1"),
          "the completer answers three letters with the whole chapter");

    BibleImport liveSession;
    liveSession.setNetworkAccessManager(&manager);
    ScriptureImport liveScripture(&liveSession, &reopened, &worker);
    check(liveSession.translation() == QStringLiteral("NIV"),
          "a fresh session opens on the default translation, untouched");
    liveSession.setReference(chapterRef);
    manager.steps.push_back({chapterBody(), {}, {}, 200, 0});
    liveSession.lookup();
    waitForLookup(liveSession);
    check(liveSession.verses().size() == 6, "the whole chapter answered");
    const QUrlQuery lastQuery(manager.urls.last());
    check(lastQuery.queryItemValue(QStringLiteral("version")) == QStringLiteral("NIV") &&
              lastQuery.queryItemValue(QStringLiteral("search")) == chapterRef,
          "the untouched default sent exactly the request the menu would have sent");
    check(waitForMeasured(liveScripture), "the whole chapter packed into multi-verse slides");
    check(liveScripture.canAdd() && liveScripture.addToSchedule(),
          "Add wakes on the production shape and lands the chapter");
    check(reopened.playlistEntries().size() == 2,
          "the chapter joined today's schedule beside the first passage");
    check(reopened.save(), "the service saves with the whole chapter");

    // The disk keeps its witness a second time, and gives the same answer:
    // fetched markup leaves no trace anywhere — not even inside the
    // service's records, which hold only the parsed words.
    const auto chapterWitness = walk(root, {"passage-text", "publisher-info-bottom",
                                          "version-NIV"});
    if (!chapterWitness.containing.empty()) {
        std::fprintf(stderr, "  chapter markup found in: %s\n",
                     chapterWitness.containing.front().string().c_str());
    }
    check(chapterWitness.containing.empty(),
          "the chapter's fetched page bytes persist nowhere under the sandbox");

    // Offline playback of the chapter through a controller that knows only
    // the disk: the words go on air because the schedule owns them.
    worker.clearSlide();
    QTest::qWait(100);
    DocumentController diskOnly(nullptr, root / "service");
    if (!check(diskOnly.load(), "the whole chapter survives a reopen from disk")) {
        worker.shutdownNow();
        return 1;
    }
    diskOnly.selectPlaylist(diskOnly.currentPlaylistId());
    QVariantList chapterScenes;
    for (const QVariant& entry : diskOnly.playlistEntries()) {
        const QString showId = entry.toMap().value(QStringLiteral("showId")).toString();
        for (const QVariant& slide : diskOnly.slidesOf(showId)) {
            const QString scene =
                diskOnly.sceneJsonForSlide(slide.toMap().value(QStringLiteral("id")).toString());
            if (scene.contains(QStringLiteral("Whole chapter words"))) {
                chapterScenes.push_back(scene);
            }
        }
    }
    int playedChapter = 0;
    for (const QVariant& scene : chapterScenes) {
        worker.presentSceneJson(scene.toString());
        if (!waitForOnAir(worker)) {
            break;
        }
        ++playedChapter;
    }
    check(static_cast<int>(chapterScenes.size()) >= 2 &&
              playedChapter == static_cast<int>(chapterScenes.size()),
          "every whole-chapter slide played back offline on the real output");
    check(worker.wordsOnAir(),
          "the congregation holds the chapter the picker never touched");
    check(manager.requests == 3,
          "the production journey fetched exactly its one page, and playback nothing");

    // --- TF.4: the reading context, end to end --------------------------------
    // The re-acceptance the text-flow plan demands: a theme that has GROWN
    // a scripture context (a reading size in a box of its own) decides how
    // the SAME chapter is set — and the flow the operator previewed is the
    // flow the schedule saves and the audience finally sees. One fresh
    // session plays the next service's operator: context applied through
    // the editor's own verbs, chapter looked up once, Add, save, reopen,
    // present. The count can only fall (same words, reading size, wider
    // box), the saved lines are the previewed lines, and every saved scene
    // compiles inside the context's numbers with its name on the slide.
    const int chapterWithoutContext = static_cast<int>(chapterScenes.size());
    DocumentController lectern(nullptr, root / "service");
    if (!check(lectern.load(), "a fresh session reopens the service for the reading context")) {
        worker.shutdownNow();
        return 1;
    }
    lectern.selectPlaylist(lectern.currentPlaylistId());
    check(lectern.createBlankTheme("Lectern"), "the service grows a reading theme");
    QString lecternId;
    for (const QVariant& theme : lectern.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Lectern")) {
            lecternId = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    QVariantMap ctxFields;
    ctxFields.insert(QStringLiteral("fontSize"), 30.0);
    ctxFields.insert(QStringLiteral("align"), QStringLiteral("left"));
    QVariantMap ctxBox;
    ctxBox.insert(QStringLiteral("x"), 100.0);
    ctxBox.insert(QStringLiteral("y"), 200.0);
    ctxBox.insert(QStringLiteral("width"), 1720.0);
    ctxBox.insert(QStringLiteral("height"), 700.0);
    ctxFields.insert(QStringLiteral("box"), ctxBox);
    check(!lecternId.isEmpty() &&
              lectern.applyThemeContext(lecternId, QStringLiteral("scripture"), ctxFields) &&
              lectern.useThemeAsCurrent(lecternId),
          "the theme carries a scripture context and the service wears it");
    const QVariantMap planStyle = lectern.scriptureBoxStyle();
    check(planStyle.value(QStringLiteral("context")).toString() == QStringLiteral("scripture") &&
              planStyle.value(QStringLiteral("fontSize")).toDouble() == 30.0 &&
              planStyle.value(QStringLiteral("boxHeight")).toDouble() == 700.0,
          "the importer's measurement style answers through the context, and names it");

    BibleImport lecternBible;
    lecternBible.setNetworkAccessManager(&manager);
    ScriptureImport lecternScripture(&lecternBible, &lectern, &worker);
    lecternBible.setReference(chapterRef);
    manager.steps.push_back({chapterBody(), {}, {}, 200, 0});
    lecternBible.lookup();
    waitForLookup(lecternBible);
    check(waitForMeasured(lecternScripture), "the chapter repacks under the reading context");
    check(lecternScripture.context() == QStringLiteral("scripture") &&
              lecternScripture.status().contains(QStringLiteral("theme's scripture context")),
          "the plan sentence says out loud which context it was measured in");
    const qsizetype previewedCount = lecternScripture.slides().size();
    check(previewedCount >= 1 && previewedCount <= chapterWithoutContext,
          "the same chapter needs no MORE slides at a reading size in a wider box "
          "than it needed plain");

    const QVariantList previewedSlides = lecternScripture.slides();
    check(lecternScripture.canAdd() && lecternScripture.addToSchedule(),
          "Add lands the contexted chapter on today's schedule");
    const QVariantList entries = lectern.playlistEntries();
    check(entries.size() == 3, "the passage joined beside the first passage and the plain chapter");
    const QString contextedShow =
        entries.last().toMap().value(QStringLiteral("showId")).toString();
    check(lectern.slidesOf(contextedShow).size() == previewedCount,
          "the schedule holds exactly the slides the preview showed — one flow, not two");
    check(lectern.save(), "the contexted service saves");

    DocumentController contextedFromDisk(nullptr, root / "service");
    if (!check(contextedFromDisk.load(), "the contexted service reopens from disk")) {
        worker.shutdownNow();
        return 1;
    }
    const QVariantList savedSlidesOfChapter = contextedFromDisk.slidesOf(contextedShow);
    bool scenesFollowContext = savedSlidesOfChapter.size() == previewedCount &&
                               !savedSlidesOfChapter.isEmpty();
    QStringList savedSceneLines;
    for (qsizetype i = 0; scenesFollowContext && i < savedSlidesOfChapter.size(); ++i) {
        const QString sceneJson = contextedFromDisk.sceneJsonForSlide(
            savedSlidesOfChapter.at(i).toMap().value(QStringLiteral("id")).toString());
        boost::json::object item;
        try {
            item = boost::json::parse(sceneJson.toStdString())
                       .at("items")
                       .as_array()
                       .at(0)
                       .as_object();
        } catch (...) {
            scenesFollowContext = false;
            break;
        }
        if (boost::json::value_to<double>(item.at("fontSize")) != 30.0 ||
            boost::json::value_to<double>(item.at("y")) != 200.0 ||
            boost::json::value_to<double>(item.at("height")) != 700.0 ||
            item.at("align").as_string() != "left") {
            scenesFollowContext = false;
            break;
        }
        QStringList lines;
        for (const boost::json::value& line : item.at("lines").as_array()) {
            lines.push_back(QString::fromStdString(boost::json::value_to<std::string>(line)));
        }
        savedSceneLines.push_back(lines.join(QLatin1Char('\n')));
    }
    check(scenesFollowContext,
          "every saved scene compiles at the context's size inside the context's box — "
          "playback resolves what the preview measured, through the same chain");
    bool linesAreTheFlow = savedSceneLines.size() == previewedCount;
    for (qsizetype i = 0; linesAreTheFlow && i < previewedCount; ++i) {
        // The scene carries exactly the lines the preview was built from —
        // the schedule did not re-flow the passage on the way to the file.
        linesAreTheFlow = savedSceneLines.at(i) ==
                          previewedSlides.at(i).toMap()
                              .value(QStringLiteral("lines"))
                              .toStringList()
                              .join(QLatin1Char('\n'));
    }
    check(linesAreTheFlow,
          "slide for slide, the words on the schedule are the words the preview showed");

    worker.clearSlide();
    QTest::qWait(100);
    const QString firstContextedScene = contextedFromDisk.sceneJsonForSlide(
        savedSlidesOfChapter.first().toMap().value(QStringLiteral("id")).toString());
    worker.presentSceneJson(firstContextedScene);
    check(waitForOnAir(worker) && worker.wordsOnAir(),
          "the PGM slide is the flow the operator previewed — the audience draws the "
          "context's picture");
    check(manager.requests == 4,
          "the contexted journey fetched its one page and playback asked for nothing");

    worker.shutdownNow();
    if (gFailed == 0) {
        std::printf("bible_import_e2e_tests: all fixtures passed\n");
        return 0;
    }
    std::printf("bible_import_e2e_tests: %d FAILURES\n", gFailed);
    return 1;
}
