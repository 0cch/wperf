#pragma once

#include "requests.h"

namespace wperf {
int RunAnnotate(const AnnotateRequest& request);
int RunDiff(const DiffRequest& request);
int RunArchive(const ArchiveRequest& request);
int RunBuildIdList(const BuildIdListRequest& request);
}
