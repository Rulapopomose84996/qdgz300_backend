#pragma once

#include "qdgz300/common/types.h"

#include <cstdlib>

namespace qdgz300
{

    inline void free_plot_batch(PlotBatch *batch)
    {
        if (!batch)
            return;

        if (batch->plots)
        {
            std::free(batch->plots);
            batch->plots = nullptr;
        }

        delete batch;
    }

} // namespace qdgz300
