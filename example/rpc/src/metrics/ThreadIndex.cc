#include "ThreadIndex.h"

namespace metrics {
std::atomic<int> ThreadIndex::next_index_{0};
}
