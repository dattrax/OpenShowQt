/*
 * Task BI.5 acceptance: the importer's page session, end to end.
 *
 * The real BibleImport with a scripted network manager (no real network),
 * the real DocumentController on a scratch service directory, and the real
 * WorkerController talking to a REAL openshow-worker — because the whole
 * point of the page is that the slides are packed by the audience's own
 * text layout, so no stand-in may measure them here. The flow: the page
 * opens on the notice; a lookup answers and the slides generate on their
 * own; a theme change re-paginates them WITHOUT another request; the
 * preview is off air (nothing the operator previews ever reaches the
 * audience); Add lands the passage on today's schedule as one undoable
 * item with its verse labels in the slides' notes — and the next click
 * has no batch left to duplicate. Cancel writes nothing, and without a
 * worker the failure is said in the page's own line rather than guessed
 * around.
 *
 * Needs a display (the worker opens its output window): desktop preset,
 * label display.
 */

#include <boost/json.hpp>

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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

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
        auto* reply = new FakeReply(request, step.body, step.error, step.errorString, step.status,
                                    step.delayMs);
        return reply;
    }

    QVector<Step> steps;
    int requests = 0;
};

// The production shape (milestone BI-F, task BIF.1): a WHOLE chapter —
// thirty-one synthetic verses of varying length in the live BI.1 markup —
// and a live-length publisher attribution: the NIV notice the operator
// actually met runs to about 140 characters, so this stand-in carries the
// same weight (its own synthetic words). Both matter at once: the chapter
// crosses the wire's 32-block batch cap, and the attribution reserves real
// measured lines no matter how the theme sizes the words.
QByteArray chapterBody() {
    std::string html = R"HTML(<div class="passage-text"><div class='passage-content'>
    <div class="version-EXAMPLE result-text-style-normal text-html">)HTML";
    for (int v = 1; v <= 31; ++v) {
        std::string text;
        switch (v % 5) {
            case 1:
                text = "Verse " + std::to_string(v) +
                       " opens the sequence with words long enough to wrap twice and enough "
                       "matter to measure honestly across the audience renderer's box.";
                break;
            case 2:
                text = "Verse " + std::to_string(v) +
                       " follows with a middling run of words, one sentence only.";
                break;
            case 3:
                text = "Verse " + std::to_string(v) +
                       " carries several sentences. It must split at a boundary. Yet nothing "
                       "may be clipped from it under any box.";
                break;
            case 4:
                text = "Verse " + std::to_string(v) + " is short.";
                break;
            default:
                text = "Verse " + std::to_string(v) +
                       " closes the little group with a longer cadence, testing how much a "
                       "slide really holds when the words keep coming.";
                break;
        }
        html += "<p><span class=\"text 1Test-1-" + std::to_string(v) + "\">" + text +
                "</span></p>\n";
    }
    html += R"HTML(    </div></div></div>
    <div class="publisher-info-bottom"><strong>Example Bible (EXAMPLE)</strong>
    <p>Example Bible, Example Version&#8482;, EXV&#8482; Copyright &#169; 1973, 2011 by Synthetic Publisher, Inc.&#8482; Used by permission. All rights reserved worldwide.</p></div>)HTML";
    return QByteArray::fromStdString(html);
}

// Six verses in the live BI.1 shape; verse four carries three sentences so
// a narrow theme box must split it, not clip it.
QByteArray passageBody() {
    return R"HTML(<div class="passage-text"><div class='passage-content'>
    <div class="version-EXAMPLE result-text-style-normal text-html">
    <p class="chapter-1"><span class="text 1Test-1-1"><span class="chapternum">1&#160;</span>First kept words of the passage.</span></p>
    <p><span class="text 1Test-1-2">Second kept words shine here.</span></p>
    <p><span class="text 1Test-1-3">Third kept words stand fast.</span></p>
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

// Wait for the live worker to be askable for measurements (connected AND
// hello'd: measureText answers 0 until the handshake names the capability).
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
            return true;  // connected, hello'd, and measurable
        }
        QTest::qWait(50);
    }
    return false;
}

QString strip(const QString& text) {
    QString out;
    for (const QChar c : text) {
        if (!c.isSpace()) {
            out += c;
        }
    }
    return out;
}

// The identity the pagination promises: the slides' words ARE the selected
// verses, whitespace aside.
bool identityHolds(ScriptureImport& scripture, BibleImport& bible) {
    QString slides;
    for (const QVariant& slide : scripture.slides()) {
        slides += slide.toMap().value(QStringLiteral("lines")).toStringList().join(QString());
    }
    QString passage;
    for (const QVariant& verse : bible.verses()) {
        if (verse.toMap().value(QStringLiteral("selected")).toBool()) {
            passage += verse.toMap().value(QStringLiteral("text")).toString();
        }
    }
    const QString a = strip(slides);
    const QString b = strip(passage);
    if (a != b) {
        std::fprintf(stderr, "  slides:   %s\n  passage:  %s\n", a.toUtf8().constData(),
                     b.toUtf8().constData());
    }
    return a == b;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        return 1;
    }
    const std::filesystem::path root = std::filesystem::path(scratch.path().toStdString());

    // --- the page opens on the notice, before anything is typed -----------
    {
        DocumentController docs(nullptr, root / "notice");
        WorkerController worker;  // never started on purpose
        BibleImport bible;
        ScriptureImport scripture(&bible, &docs, &worker);
        check(bible.status().contains(QStringLiteral("Online")) &&
                  bible.status().contains(QStringLiteral("internet")),
              "the import page opens on the online notice");
        check(!scripture.canAdd() && scripture.slides().isEmpty(),
              "an unasked page has nothing to add");
        check(!scripture.addToSchedule(),
              "Add without a passage refuses rather than inventing one");
    }

    // --- the full journey on a real worker ---------------------------------
    {
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

        bible.setReference(QStringLiteral("First Test 1:1-6"));
        bible.setTranslation(QStringLiteral("EXAMPLE"));
        manager.steps.push_back({passageBody(), {}, {}, 200, 0});
        bible.lookup();
        waitForLookup(bible);
        check(bible.verses().size() == 6, "the lookup answered with the six verses");
        check(waitForMeasured(scripture), "the selection paginated without being asked twice");
        check(!scripture.slides().isEmpty(), "slides generated from the selected verses");
        check(identityHolds(scripture, bible),
              "the generated slides carry exactly the selected passage");
        check(scripture.canAdd(), "a measured batch makes Add live");

        // The audience has seen NOTHING all along: the page only ever asks
        // for the off-air selection picture.
        check(!worker.wordsOnAir(), "previewing generated slides never puts words on air");
        scripture.previewSlide(scripture.slides().size() > 1 ? 1 : 0);
        QTest::qWait(120);
        check(!worker.wordsOnAir(), "a clicked preview is still off air");

        // --- a theme change re-paginates, with no second request -----------
        const int requestsAfterLookup = manager.requests;
        const qsizetype slidesBefore = scripture.slides().size();
        // The surfaces name themes by id; the blank theme's id is the row
        // the list carries under its (unique) name.
        check(docs.createBlankTheme(QStringLiteral("Tight")), "a blank theme is created");
        QString tight;
        for (const QVariant& row : docs.themes()) {
            const QVariantMap map = row.toMap();
            if (map.value(QStringLiteral("name")).toString() == QStringLiteral("Tight")) {
                tight = map.value(QStringLiteral("id")).toString();
            }
        }
        // A small chosen size keeps the reservation honest rather than
        // impossible: the attribution still reserves real measured lines,
        // but the box retains room for the words.
        check(!tight.isEmpty() &&
                  docs.editTheme(tight, QStringLiteral("Tight"), QStringLiteral("#ffffff"),
                                 QStringLiteral("#000000"), QString(), QString(), 24.0,
                                 QStringLiteral("center"), 1.25) &&
                  docs.useThemeAsCurrent(tight),
              "a small-type blank theme is worn");
        check(docs.setThemePlacement(tight, 160, 100, 500, 150),
              "the worn theme places its box small");
        scripture.regenerate();
        check(waitForMeasured(scripture), "the narrower box re-measured");
        check(manager.requests == requestsAfterLookup,
              "re-pagination used no network — the words were already held");
        check(scripture.slides().size() > slidesBefore ||
                  scripture.slides().size() >= 2,
              "the narrow box split the passage into more slides, never smaller words");
        check(identityHolds(scripture, bible), "the narrow re-pagination preserved the passage");

        // Long-verse continuation is labelled, and the label says so.
        bool sawContinuation = false;
        for (const QVariant& slide : scripture.slides()) {
            const QString label = slide.toMap().value(QStringLiteral("label")).toString();
            if (label.contains(QStringLiteral("continues")) ||
                label.contains(QStringLiteral("cont."))) {
                sawContinuation = true;
            }
        }
        check(sawContinuation,
              "a split verse's slides say so in their labels (which ride in notes)");

        // Back to a wide box: fewer slides, same words, still no request.
        check(docs.setThemePlacement(tight, 100, 60, 1720, 960),
              "the theme widens its box");
        scripture.regenerate();
        check(waitForMeasured(scripture), "the wide box re-measured");
        check(manager.requests == requestsAfterLookup, "still no network for re-pagination");
        check(identityHolds(scripture, bible), "the wide re-pagination preserved the passage");

        // --- Add: one item, labels in notes, and no second bite -----------
        const int libraryBefore = docs.songs().size();
        const qsizetype slidesAtAdd = scripture.slides().size();
        check(scripture.addToSchedule(), "Add lands the passage on today's schedule");
        check(docs.playlistEntries().size() == 1,
              "the schedule gained exactly one row");
        check(docs.songs().size() == libraryBefore,
              "the Library gained nothing — the page has no door there");
        check(!scripture.canAdd() && !scripture.addToSchedule(),
              "the batch was consumed: a repeated Add cannot duplicate the insertion");
        const QString passageShow =
            docs.playlistEntries().at(0).toMap().value(QStringLiteral("showId")).toString();
        check(!passageShow.isEmpty() && docs.slidesOf(passageShow).size() == slidesAtAdd,
              "the schedule-owned show carries exactly the generated slides");
        docs.undo();
        check(docs.playlistEntries().isEmpty(),
              "one undo removes the whole passage from the schedule");
        docs.redo();
        check(docs.playlistEntries().size() == 1, "one redo brings it all back");

        // The saved file carries the verse labels in NOTES — the slide's
        // words stayed exactly the passage.
        check(docs.save(), "the service saves");
        const boost::json::value manifest = boost::json::parse(
            readTextFile(root / "service" / "service" / "service.json"));
        bool notesCarried = false;
        bool wordsPure = true;
        for (const boost::json::value& showValue : manifest.at("shows").as_array()) {
            const boost::json::object& show = showValue.as_object();
            if (!show.contains("scripture")) {
                continue;
            }
            for (const boost::json::value& slideValue : show.at("slides").as_array()) {
                const boost::json::object& slide = slideValue.as_object();
                const std::string notes = slide.contains("notes")
                                              ? std::string(slide.at("notes").as_string())
                                              : std::string();
                if (notes.find("1:1") != std::string::npos) {
                    notesCarried = true;
                }
                for (const boost::json::value& boxValue : slide.at("items").as_array()) {
                    const boost::json::object& item = boxValue.as_object();
                    if (!item.contains("lines")) {
                        continue;  // image items carry no words
                    }
                    for (const boost::json::value& line : item.at("lines").as_array()) {
                        const std::string text(line.as_string());
                        if (text.find("cont.") != std::string::npos ||
                            text.find("(continues)") != std::string::npos) {
                            wordsPure = false;
                        }
                    }
                }
            }
        }
        check(notesCarried, "the saved slides carry their verse labels in notes");
        check(wordsPure, "no label text ever rode on a slide's words");

        // --- cancel writes nothing ----------------------------------------
        const qsizetype entriesBefore = docs.playlistEntries().size();
        bible.setReference(QStringLiteral("First Test 1:2-3"));
        bible.setTranslation(QStringLiteral("EXAMPLE"));
        manager.steps.push_back({passageBody(), {}, {}, 200, 0});
        bible.lookup();
        waitForLookup(bible);
        waitForMeasured(scripture);
        check(!scripture.slides().isEmpty(), "the second lookup generated slides too");
        scripture.cancel();
        check(scripture.slides().isEmpty() && !scripture.measuring(),
              "cancel discarded the fetched words and the generated slides");
        check(docs.playlistEntries().size() == entriesBefore,
              "cancel wrote nothing to the schedule");
        check(scripture.status().contains(QStringLiteral("Nothing was written")),
              "and said so in its own line");

        worker.shutdownNow();
    }

    // --- BIF.1: the production shape ----------------------------------------
    // A whole chapter, a live-length attribution, the plain look the
    // operator wore (no theme edits), the worker's preview stream up from
    // the first published slide — and the page's documentsChanged handler
    // (which calls regenerate) firing WHILE a measure batch is in flight.
    // The end state this fixture demands is the one the operator never
    // reached: the run settles, Add is live, and one Add lands the item.
    {
        DocumentController docs(nullptr, root / "prodshape");
        WorkerController worker;
        worker.start();
        if (!check(waitForMeasureReady(worker), "the production-shape worker came up")) {
            worker.shutdownNow();
            return 1;
        }
        BibleImport bible;
        ScriptedManager manager;
        bible.setNetworkAccessManager(&manager);
        ScriptureImport scripture(&bible, &docs, &worker);

        bible.setReference(QStringLiteral("First Test 1"));
        bible.setTranslation(QStringLiteral("EXAMPLE"));
        manager.steps.push_back({chapterBody(), {}, {}, 200, 0});
        bible.lookup();
        waitForLookup(bible);
        check(bible.verses().size() == 31, "the chapter lookup answered with thirty-one verses");
        check(bible.attribution().size() > 120,
              "the fixture carries a live-length attribution, not a stub");

        // Race the page's re-pagination across BOTH the answer and the
        // in-flight batches — exactly what the QML Connections handler
        // does when the documents move under an open page: some calls
        // land before pagination, some land on a batch in flight.
        QElapsedTimer churn;
        churn.start();
        while (churn.elapsed() < 4000 && (bible.busy() || scripture.measuring())) {
            QTest::qWait(8);
            scripture.regenerate();
        }
        check(!bible.busy(), "the chapter lookup completed under the churn");
        check(waitForMeasured(scripture, 30000),
              "the racing regenerate did not strand the run on measuring");
        if (!scripture.slides().isEmpty()) {
            // The preview stream is up: the first slide went off air.
            QTest::qWait(120);
            check(!worker.wordsOnAir(), "the production-shape preview stayed off air");
        }
        // Whatever the plain look measured out, the operator's rule holds:
        // if the words fit, Add is live and one Add lands; if they cannot,
        // the page says WHY and never sits silent on measuring.
        // BIF.1's outcome, demanded outright: the provenance reserves
        // caption room rather than eating the box in the words' display
        // face, so the plain look paginates, packs multiple verses per
        // slide, wakes Add, and one Add lands the whole chapter.
        std::fprintf(stderr, "  prodshape: slides=%lld status=%s\n",
                     static_cast<long long>(scripture.slides().size()),
                     scripture.status().toUtf8().constData());
        check(!scripture.slides().isEmpty(),
              "the chapter paginates on the plain look — the caption reservation leaves room "
              "instead of refusing");
        check(scripture.slides().size() < bible.verses().size(),
              "the chapter packs multiple verses per slide — never one verse per slide");
        check(scripture.canAdd(), "the production shape wakes Add");
        const qsizetype slidesAtAdd = scripture.slides().size();
        check(scripture.addToSchedule(), "Add landed the chapter on today's schedule");
        check(docs.playlistEntries().size() == 1, "exactly one row joined the schedule");
        const QString showId =
            docs.playlistEntries().at(0).toMap().value(QStringLiteral("showId")).toString();
        check(!showId.isEmpty() && docs.slidesOf(showId).size() == slidesAtAdd,
              "the schedule-owned show carries every generated slide");
        worker.shutdownNow();
    }

    // --- BIF.1: a batch that can never be answered retires, with its own
    // sentence — the page must never sit on "measuring" for a worker that
    // has gone away mid-ask. -----------------------------------------------
    {
        DocumentController docs(nullptr, root / "unanswerable");
        WorkerController worker;
        worker.start();
        if (!check(waitForMeasureReady(worker), "the unanswerable-shape worker came up")) {
            worker.shutdownNow();
            return 1;
        }
        BibleImport bible;
        ScriptedManager manager;
        bible.setNetworkAccessManager(&manager);
        ScriptureImport scripture(&bible, &docs, &worker);
        bible.setReference(QStringLiteral("First Test 1"));
        bible.setTranslation(QStringLiteral("EXAMPLE"));
        manager.steps.push_back({chapterBody(), {}, {}, 200, 0});
        bible.lookup();
        waitForLookup(bible);
        check(bible.verses().size() == 31, "the chapter answered before the worker died");
        // The batch is in flight by now (or queued); kill the answerer.
        worker.shutdownNow();
        check(waitForMeasured(scripture, 15000),
              "with no worker to answer, the run retired instead of hanging");
        check(!scripture.measuring(), "the page does not sit on measuring");
        if (scripture.slides().isEmpty()) {
            // The death cut a run short (the normal outcome at this
            // timing): no slides survive an unmeasured passage, Add stays
            // asleep, and the sentence says what went away.
            check(!scripture.canAdd(), "no Add on a run that never finished");
            check(scripture.status().contains(QStringLiteral("renderer")) ||
                      scripture.status().contains(QStringLiteral("worker")),
                  "the unanswerable batch's sentence names what went away");
        }
    }

    // --- without a worker the failure is actionable, not guessed -----------
    {
        DocumentController docs(nullptr, root / "offline");
        WorkerController worker;  // never started
        BibleImport bible;
        ScriptedManager manager;
        bible.setNetworkAccessManager(&manager);
        ScriptureImport scripture(&bible, &docs, &worker);
        bible.setReference(QStringLiteral("First Test 1:1-2"));
        bible.setTranslation(QStringLiteral("EXAMPLE"));
        manager.steps.push_back({passageBody(), {}, {}, 200, 0});
        bible.lookup();
        waitForLookup(bible);
        check(bible.verses().size() == 6, "the lookup itself still answered");
        check(waitForMeasured(scripture), "the run settled instead of hanging");
        check(scripture.slides().isEmpty(),
              "no worker means no measured slides — never estimated ones");
        check(scripture.status().contains(QStringLiteral("renderer")) ||
                  scripture.status().contains(QStringLiteral("worker")),
              "the failure names the renderer as the thing that is missing");
        check(!scripture.canAdd() && !scripture.addToSchedule(),
              "Add stays asleep while the words cannot be measured");
    }

    // --- an offline lookup fails actionably on its own line ----------------
    {
        DocumentController docs(nullptr, root / "noweb");
        WorkerController worker;
        BibleImport bible;
        ScriptedManager manager;
        bible.setNetworkAccessManager(&manager);
        ScriptureImport scripture(&bible, &docs, &worker);
        bible.setReference(QStringLiteral("First Test 1"));
        bible.setTranslation(QStringLiteral("EXAMPLE"));
        manager.steps.push_back(
            {{}, QNetworkReply::HostNotFoundError, QStringLiteral("Host not found"), 0, 0});
        bible.lookup();
        waitForLookup(bible);
        check(!bible.busy() && bible.verses().isEmpty(),
              "an unreachable provider leaves the page with no passage");
        check(!bible.status().isEmpty(),
              "the lookup line says what went wrong (the BI.2 sentence)");
        check(!scripture.canAdd(), "and nothing is offerable to the schedule from it");
    }

    if (gFailed == 0) {
        std::printf("bible_import_page_tests: all fixtures passed\n");
        return 0;
    }
    std::printf("bible_import_page_tests: %d FAILURES\n", gFailed);
    return 1;
}
