#include "fasttrack.h"
#include "fasttrack_st_export.h"
#include <shared_mutex>

extern "C" FASTTRACK_ST_EXPORT Detector * CreateDetector() {
    return new drace::detector::Fasttrack<std::shared_mutex>();
}
