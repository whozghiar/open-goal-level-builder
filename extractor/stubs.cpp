// The decompiler's Function owns the code analysis structures (IR2); the extractor never builds
// them, but their destructor is referenced.
#include "decompiler/IR2/Form.h"

namespace decompiler {
FormPool::~FormPool() {}
}  // namespace decompiler
