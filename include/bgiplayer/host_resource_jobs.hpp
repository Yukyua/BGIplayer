#pragma once
#include <cstddef>
#include <functional>

namespace bgiplayer {
// Prepare independent pieces of a resource in bounded host workers. The
// caller owns private output, waits for all pieces and publishes it in VM order.
// Exceptions are rethrown in logical index order after every worker retires.
void prepare_resource_parts(size_t count,size_t work_pixels,
    const std::function<void(size_t)>& prepare);
}
