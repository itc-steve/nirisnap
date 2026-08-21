#pragma once

#include <QString>

/** Runs a detached pinned-image layer using the current Nirisnap process. */
[[nodiscard]] int runPinnedCapture(const QString &path);
