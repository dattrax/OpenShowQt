#pragma once

#include <QString>

#include <cstdint>

namespace openshow::ui {

/*
 * What one scene.prepare asks the worker for beyond the scene itself
 * (task 6.19a2).
 *
 * The prepare is the only session-owned request on this socket, so every
 * preview ask that has to wait for a scene rides on it: the prepare is sent
 * first so the worker has the scene before it is told which slide to render,
 * and the preview.select / thumbnails goes out only once the prepare has
 * COMMITTED (an acked revision covers the scene; a generation change means
 * the worker swapped the scene on its own and this payload must be re-sent),
 * because the worker answers preview.select by re-rendering the exact
 * revision named — sending it for a scene the worker never took would ask
 * for a picture of something that is not there.
 *
 * Both features decorate the same record because the prepare can carry one,
 * the other, or both: the ack that lands is what both were waiting for, and
 * one table entry keeps the two from disagreeing about which prepare they
 * were each waiting on.
 */
struct ScenePrepareAsk {
    QString prepareJson;       // the payload sent; the ack carries no scene
    bool autoPresent = false;  // the operator pulled the lever: present when ready

    // Non-zero when the editor's off-air target / the slide list's pictures
    // were folded into this prepare, carrying the request id the ask named
    // (task 6.16a, 6.16b). The ack sends the ask only for the request that
    // is still the newest one: a newer edit (or a newer batch) has its own
    // prepare on its way, and answering an old request would put an
    // overtake picture on the canvas for a slide nobody is looking at
    // anymore.
    std::uint32_t selectionRequest = 0;
    std::uint32_t thumbnailRequest = 0;
};

}  // namespace openshow::ui
